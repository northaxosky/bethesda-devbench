#include "SkyrimSaveHeader.h"

#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dvb::skyrimse::save
{
	namespace
	{
		constexpr std::string_view kMagic = "TESV_SAVEGAME";
		constexpr std::size_t      kPrefixBytes = kMagic.size() + sizeof(std::uint32_t);
		constexpr std::uintmax_t   kBytesPerScreenshotPixel = 3;

		class HeaderReader
		{
		public:
			explicit HeaderReader(std::span<const std::byte> a_bytes) :
				bytes_(a_bytes)
			{}

			template <class T>
			std::optional<T> Read()
			{
				static_assert(std::is_trivially_copyable_v<T>);
				if (remaining() < sizeof(T))
					return std::nullopt;
				T value{};
				std::memcpy(std::addressof(value), bytes_.data() + offset_, sizeof(T));
				offset_ += sizeof(T);
				return value;
			}

			std::optional<std::string> ReadString()
			{
				const auto length = Read<std::uint16_t>();
				if (!length || *length > kMaxSkyrimSaveMetadataStringBytes || remaining() < *length)
					return std::nullopt;
				const auto* data = reinterpret_cast<const char*>(bytes_.data() + offset_);
				std::string value(data, *length);
				offset_ += *length;
				return value;
			}

			[[nodiscard]] std::size_t remaining() const { return bytes_.size() - offset_; }

		private:
			std::span<const std::byte> bytes_;
			std::size_t                offset_ = 0;
		};

		tools::game::SaveMetadataResult Error(std::string a_error)
		{
			return { .error = std::move(a_error) };
		}
	}

	tools::game::SaveMetadataResult ParseSkyrimSaveHeader(
		std::span<const std::byte> a_headerBytes, std::uintmax_t a_fileSize)
	{
		if (a_headerBytes.size() < kPrefixBytes)
			return Error("truncated before the Skyrim save header");
		if (std::memcmp(a_headerBytes.data(), kMagic.data(), kMagic.size()) != 0)
			return Error("invalid Skyrim save magic");

		std::uint32_t headerSize = 0;
		std::memcpy(
			std::addressof(headerSize), a_headerBytes.data() + kMagic.size(), sizeof(headerSize));
		if (headerSize == 0 || headerSize > kMaxSkyrimSaveHeaderBytes)
			return Error("Skyrim save header size is outside the supported bound");
		if (a_headerBytes.size() < kPrefixBytes + headerSize)
			return Error("truncated Skyrim save header");

		HeaderReader reader(a_headerBytes.subspan(kPrefixBytes, headerSize));
		const auto   formatVersion = reader.Read<std::uint32_t>();
		const auto   saveNumber = reader.Read<std::uint32_t>();
		const auto   characterName = reader.ReadString();
		const auto   level = reader.Read<std::uint32_t>();
		const auto   location = reader.ReadString();
		const auto   playTime = reader.ReadString();
		const auto   race = reader.ReadString();
		const auto   sex = reader.Read<std::uint16_t>();
		const auto   currentExperience = reader.Read<float>();
		const auto   requiredExperience = reader.Read<float>();
		const auto   fileTime = reader.Read<std::uint64_t>();
		const auto   screenshotWidth = reader.Read<std::uint32_t>();
		const auto   screenshotHeight = reader.Read<std::uint32_t>();
		if (!formatVersion || !saveNumber || !characterName || !level || !location ||
			!playTime || !race || !sex || !currentExperience || !requiredExperience ||
			!fileTime || !screenshotWidth || !screenshotHeight)
			return Error("truncated or invalid Skyrim save metadata fields");
		if (reader.remaining() != 0)
			return Error("Skyrim save header size does not match its metadata fields");
		if (*screenshotWidth == 0 || *screenshotHeight == 0 ||
			*screenshotWidth > kMaxSkyrimSaveScreenshotDimension ||
			*screenshotHeight > kMaxSkyrimSaveScreenshotDimension)
			return Error("Skyrim save screenshot dimensions are outside the supported bound");

		const auto width = static_cast<std::uintmax_t>(*screenshotWidth);
		const auto height = static_cast<std::uintmax_t>(*screenshotHeight);
		if (height > std::numeric_limits<std::uintmax_t>::max() / width ||
			width * height > std::numeric_limits<std::uintmax_t>::max() / kBytesPerScreenshotPixel)
			return Error("Skyrim save screenshot size overflows");
		const auto screenshotBytes = width * height * kBytesPerScreenshotPixel;
		const auto screenshotOffset = static_cast<std::uintmax_t>(kPrefixBytes) + headerSize;
		if (screenshotOffset > a_fileSize || screenshotBytes > a_fileSize - screenshotOffset)
			return Error("truncated Skyrim save screenshot");

		tools::game::SaveMetadata metadata{
			.formatVersion = *formatVersion,
			.saveNumber = *saveNumber,
			.characterName = *characterName,
			.level = *level,
			.location = *location,
			.playTime = *playTime,
			.race = *race,
			.sex = *sex,
			.currentExperience = *currentExperience,
			.requiredExperience = *requiredExperience,
			.screenshotWidth = *screenshotWidth,
			.screenshotHeight = *screenshotHeight,
		};
		if (*fileTime != 0)
			metadata.fileTimeUnix = SkyrimFileTimeToUnix(*fileTime);
		return { .metadata = std::move(metadata) };
	}

	tools::game::SaveMetadataResult ReadSkyrimSaveHeader(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		const auto      fileSize = std::filesystem::file_size(a_path, ec);
		if (ec)
			return Error(std::format("could not read save size: {}", ec.message()));
		if (fileSize < kPrefixBytes)
			return Error("truncated before the Skyrim save header");

		std::ifstream input(a_path, std::ios::binary);
		if (!input)
			return Error("could not open the Skyrim save");

		std::array<std::byte, kPrefixBytes> prefix{};
		if (!input.read(reinterpret_cast<char*>(prefix.data()), prefix.size()))
			return Error("truncated before the Skyrim save header");
		if (std::memcmp(prefix.data(), kMagic.data(), kMagic.size()) != 0)
			return Error("invalid Skyrim save magic");

		std::uint32_t headerSize = 0;
		std::memcpy(std::addressof(headerSize), prefix.data() + kMagic.size(), sizeof(headerSize));
		if (headerSize == 0 || headerSize > kMaxSkyrimSaveHeaderBytes)
			return Error("Skyrim save header size is outside the supported bound");
		if (fileSize < kPrefixBytes + headerSize)
			return Error("truncated Skyrim save header");

		std::vector<std::byte> bytes(kPrefixBytes + headerSize);
		std::memcpy(bytes.data(), prefix.data(), prefix.size());
		if (!input.read(
				reinterpret_cast<char*>(bytes.data() + prefix.size()),
				static_cast<std::streamsize>(headerSize)))
			return Error("truncated Skyrim save header");
		return ParseSkyrimSaveHeader(bytes, fileSize);
	}

	std::int64_t SkyrimFileTimeToUnix(std::uint64_t a_fileTime)
	{
		constexpr std::uint64_t kTicksPerSecond = 10'000'000;
		constexpr std::int64_t  kUnixEpochOffsetSeconds = 11'644'473'600;
		return static_cast<std::int64_t>(a_fileTime / kTicksPerSecond) -
		       kUnixEpochOffsetSeconds;
	}
}

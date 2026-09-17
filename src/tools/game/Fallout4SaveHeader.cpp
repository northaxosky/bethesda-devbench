#include "Fallout4SaveHeader.h"

#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dvb::tools::game
{
	namespace
	{
		constexpr std::string_view kMagic = "FO4_SAVEGAME";
		constexpr std::size_t      kPrefixBytes = kMagic.size() + sizeof(std::uint32_t);
		// The FO4 load-menu header is followed by an uncompressed BGRA screenshot.
		// Validate its declared extent against file size, but never read those pixels.
		constexpr std::uintmax_t kBytesPerScreenshotPixel = 4;

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
				if (!length || *length > kMaxFallout4SaveMetadataStringBytes || remaining() < *length)
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

		SaveMetadataResult Error(std::string a_error)
		{
			return { .error = std::move(a_error) };
		}

		bool IsValidUtf8(std::string_view a_value)
		{
			for (std::size_t index = 0; index < a_value.size();)
			{
				const auto lead = static_cast<unsigned char>(a_value[index]);
				if (lead < 0x80)
				{
					++index;
					continue;
				}

				std::size_t   continuationCount = 0;
				std::uint32_t codePoint = 0;
				if ((lead & 0xE0) == 0xC0)
				{
					continuationCount = 1;
					codePoint = lead & 0x1F;
				}
				else if ((lead & 0xF0) == 0xE0)
				{
					continuationCount = 2;
					codePoint = lead & 0x0F;
				}
				else if ((lead & 0xF8) == 0xF0)
				{
					continuationCount = 3;
					codePoint = lead & 0x07;
				}
				else
				{
					return false;
				}
				if (index + continuationCount >= a_value.size())
					return false;
				for (std::size_t offset = 1; offset <= continuationCount; ++offset)
				{
					const auto continuation =
						static_cast<unsigned char>(a_value[index + offset]);
					if ((continuation & 0xC0) != 0x80)
						return false;
					codePoint = (codePoint << 6) | (continuation & 0x3F);
				}
				const auto minimum =
					continuationCount == 1 ? 0x80u :
					continuationCount == 2 ? 0x800u :
											 0x10000u;
				if (codePoint < minimum || codePoint > 0x10FFFFu ||
					(codePoint >= 0xD800u && codePoint <= 0xDFFFu))
					return false;
				index += continuationCount + 1;
			}
			return true;
		}
	}

	SaveMetadataResult ParseFallout4SaveHeader(
		std::span<const std::byte> a_headerBytes, std::uintmax_t a_fileSize)
	{
		if (a_headerBytes.size() < kPrefixBytes)
			return Error("truncated before the Fallout 4 save header");
		if (std::memcmp(a_headerBytes.data(), kMagic.data(), kMagic.size()) != 0)
			return Error("invalid Fallout 4 save magic");

		std::uint32_t headerSize = 0;
		std::memcpy(
			std::addressof(headerSize), a_headerBytes.data() + kMagic.size(), sizeof(headerSize));
		if (headerSize == 0 || headerSize > kMaxFallout4SaveHeaderBytes)
			return Error("Fallout 4 save header size is outside the supported bound");
		if (a_headerBytes.size() < kPrefixBytes + headerSize)
			return Error("truncated Fallout 4 save header");

		HeaderReader reader(a_headerBytes.subspan(kPrefixBytes, headerSize));
		SaveMetadata metadata;
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
		if (!formatVersion || !saveNumber || !characterName || !level || !location || !playTime ||
			!race || !sex || !currentExperience || !requiredExperience || !fileTime ||
			!screenshotWidth || !screenshotHeight)
			return Error("truncated or invalid Fallout 4 save metadata fields");
		if (!IsValidUtf8(*characterName) || !IsValidUtf8(*location) ||
			!IsValidUtf8(*playTime) || !IsValidUtf8(*race))
			return Error("Fallout 4 save metadata contains invalid UTF-8");
		if (reader.remaining() != 0)
			return Error("Fallout 4 save header size does not match its metadata fields");
		if (*screenshotWidth == 0 || *screenshotHeight == 0 ||
			*screenshotWidth > kMaxFallout4SaveScreenshotDimension ||
			*screenshotHeight > kMaxFallout4SaveScreenshotDimension)
			return Error("Fallout 4 save screenshot dimensions are outside the supported bound");

		const auto width = static_cast<std::uintmax_t>(*screenshotWidth);
		const auto height = static_cast<std::uintmax_t>(*screenshotHeight);
		if (height > std::numeric_limits<std::uintmax_t>::max() / width ||
			width * height > std::numeric_limits<std::uintmax_t>::max() / kBytesPerScreenshotPixel)
			return Error("Fallout 4 save screenshot size overflows");
		const auto screenshotBytes = width * height * kBytesPerScreenshotPixel;
		const auto screenshotOffset = static_cast<std::uintmax_t>(kPrefixBytes) + headerSize;
		if (screenshotOffset > a_fileSize || screenshotBytes > a_fileSize - screenshotOffset)
			return Error("truncated Fallout 4 save screenshot");

		metadata.formatVersion = *formatVersion;
		metadata.saveNumber = *saveNumber;
		metadata.characterName = *characterName;
		metadata.level = *level;
		metadata.location = *location;
		metadata.playTime = *playTime;
		metadata.race = *race;
		metadata.sex = *sex;
		metadata.currentExperience = *currentExperience;
		metadata.requiredExperience = *requiredExperience;
		if (*fileTime != 0)
			metadata.fileTimeUnix = Fallout4FileTimeToUnix(*fileTime);
		metadata.screenshotWidth = *screenshotWidth;
		metadata.screenshotHeight = *screenshotHeight;
		return { .metadata = std::move(metadata) };
	}

	SaveMetadataResult ReadFallout4SaveHeader(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		const auto      fileSize = std::filesystem::file_size(a_path, ec);
		if (ec)
			return Error(std::format("could not read save size: {}", ec.message()));
		if (fileSize < kPrefixBytes)
			return Error("truncated before the Fallout 4 save header");

		std::ifstream input(a_path, std::ios::binary);
		if (!input)
			return Error("could not open the Fallout 4 save");

		std::array<std::byte, kPrefixBytes> prefix{};
		if (!input.read(reinterpret_cast<char*>(prefix.data()), prefix.size()))
			return Error("truncated before the Fallout 4 save header");
		if (std::memcmp(prefix.data(), kMagic.data(), kMagic.size()) != 0)
			return Error("invalid Fallout 4 save magic");

		std::uint32_t headerSize = 0;
		std::memcpy(std::addressof(headerSize), prefix.data() + kMagic.size(), sizeof(headerSize));
		if (headerSize == 0 || headerSize > kMaxFallout4SaveHeaderBytes)
			return Error("Fallout 4 save header size is outside the supported bound");
		if (fileSize < kPrefixBytes + headerSize)
			return Error("truncated Fallout 4 save header");

		std::vector<std::byte> bytes(kPrefixBytes + headerSize);
		std::memcpy(bytes.data(), prefix.data(), prefix.size());
		if (!input.read(
				reinterpret_cast<char*>(bytes.data() + prefix.size()),
				static_cast<std::streamsize>(headerSize)))
			return Error("truncated Fallout 4 save header");
		return ParseFallout4SaveHeader(bytes, fileSize);
	}

	std::int64_t Fallout4FileTimeToUnix(std::uint64_t a_fileTime)
	{
		constexpr std::uint64_t kTicksPerSecond = 10'000'000;
		constexpr std::int64_t  kUnixEpochOffsetSeconds = 11'644'473'600;
		return static_cast<std::int64_t>(a_fileTime / kTicksPerSecond) -
		       kUnixEpochOffsetSeconds;
	}
}

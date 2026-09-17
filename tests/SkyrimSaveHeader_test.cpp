#include "test_framework.h"

#include "game/skyrimse/save/SkyrimSaveHeader.h"

#include <cstring>
#include <vector>

namespace
{
	template <class T>
	void Append(std::vector<std::byte>& a_bytes, const T& a_value)
	{
		const auto oldSize = a_bytes.size();
		a_bytes.resize(oldSize + sizeof(T));
		std::memcpy(a_bytes.data() + oldSize, std::addressof(a_value), sizeof(T));
	}

	void AppendString(std::vector<std::byte>& a_bytes, std::string_view a_value)
	{
		const auto length = static_cast<std::uint16_t>(a_value.size());
		Append(a_bytes, length);
		for (const auto ch : a_value)
			a_bytes.push_back(static_cast<std::byte>(ch));
	}

	std::vector<std::byte> MakeHeader()
	{
		std::vector<std::byte> bytes;
		for (const auto ch : std::string_view("TESV_SAVEGAME"))
			bytes.push_back(static_cast<std::byte>(ch));
		const auto headerSizeOffset = bytes.size();
		Append(bytes, std::uint32_t{ 0 });

		Append(bytes, std::uint32_t{ 12 });
		Append(bytes, std::uint32_t{ 42 });
		AppendString(bytes, "Prisoner");
		Append(bytes, std::uint32_t{ 18 });
		AppendString(bytes, "Whiterun");
		AppendString(bytes, "12.34.56");
		AppendString(bytes, "NordRace");
		Append(bytes, std::uint16_t{ 0 });
		Append(bytes, 225.0F);
		Append(bytes, 300.0F);
		Append(bytes, std::uint64_t{ 116444736000000000ULL });
		Append(bytes, std::uint32_t{ 2 });
		Append(bytes, std::uint32_t{ 1 });

		const auto headerSize =
			static_cast<std::uint32_t>(bytes.size() - headerSizeOffset - sizeof(std::uint32_t));
		std::memcpy(bytes.data() + headerSizeOffset, std::addressof(headerSize), sizeof(headerSize));
		return bytes;
	}
}

TEST_CASE("Skyrim save header parser reads bounded metadata without screenshot pixels")
{
	const auto bytes = MakeHeader();
	const auto result =
		dvb::skyrimse::save::ParseSkyrimSaveHeader(bytes, bytes.size() + 6);
	CHECK(result.metadata.has_value());
	CHECK(result.error.empty());
	CHECK(result.metadata->formatVersion == 12);
	CHECK(result.metadata->saveNumber == 42);
	CHECK(result.metadata->characterName == "Prisoner");
	CHECK(result.metadata->level == 18);
	CHECK(result.metadata->location == "Whiterun");
	CHECK(result.metadata->race == "NordRace");
	CHECK(result.metadata->screenshotWidth == 2);
	CHECK(result.metadata->screenshotHeight == 1);
	CHECK(result.metadata->fileTimeUnix == 0);
}

TEST_CASE("Skyrim save header parser rejects malformed and truncated data")
{
	auto badMagic = MakeHeader();
	badMagic[0] = std::byte{ 0 };
	const auto invalid =
		dvb::skyrimse::save::ParseSkyrimSaveHeader(badMagic, badMagic.size() + 6);
	CHECK(!invalid.metadata.has_value());
	CHECK(invalid.error.find("magic") != std::string::npos);

	const auto header = MakeHeader();
	const auto truncatedHeader = dvb::skyrimse::save::ParseSkyrimSaveHeader(
		std::span<const std::byte>(header).first(header.size() - 1), header.size() + 6);
	CHECK(!truncatedHeader.metadata.has_value());
	CHECK(truncatedHeader.error.find("truncated") != std::string::npos);

	const auto truncatedScreenshot =
		dvb::skyrimse::save::ParseSkyrimSaveHeader(header, header.size() + 5);
	CHECK(!truncatedScreenshot.metadata.has_value());
	CHECK(truncatedScreenshot.error.find("screenshot") != std::string::npos);
}

TEST_CASE("Skyrim save header parser bounds declared header and screenshot sizes")
{
	auto       oversizedHeader = MakeHeader();
	const auto declared =
		static_cast<std::uint32_t>(dvb::skyrimse::save::kMaxSkyrimSaveHeaderBytes + 1);
	std::memcpy(
		oversizedHeader.data() + std::string_view("TESV_SAVEGAME").size(),
		std::addressof(declared), sizeof(declared));
	const auto headerResult = dvb::skyrimse::save::ParseSkyrimSaveHeader(
		oversizedHeader, oversizedHeader.size() + 6);
	CHECK(!headerResult.metadata.has_value());
	CHECK(headerResult.error.find("bound") != std::string::npos);

	auto       oversizedScreenshot = MakeHeader();
	const auto width = dvb::skyrimse::save::kMaxSkyrimSaveScreenshotDimension + 1;
	std::memcpy(
		oversizedScreenshot.data() + oversizedScreenshot.size() - 8,
		std::addressof(width), sizeof(width));
	const auto screenshotResult = dvb::skyrimse::save::ParseSkyrimSaveHeader(
		oversizedScreenshot, oversizedScreenshot.size() + 6);
	CHECK(!screenshotResult.metadata.has_value());
	CHECK(screenshotResult.error.find("dimensions") != std::string::npos);
}

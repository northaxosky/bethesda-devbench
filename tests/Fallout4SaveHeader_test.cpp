#include "test_framework.h"

#include "tools/game/Fallout4SaveHeader.h"

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
		for (const auto ch : std::string_view("FO4_SAVEGAME"))
			bytes.push_back(static_cast<std::byte>(ch));
		const auto headerSizeOffset = bytes.size();
		Append(bytes, std::uint32_t{ 0 });

		Append(bytes, std::uint32_t{ 15 });
		Append(bytes, std::uint32_t{ 9 });
		AppendString(bytes, "Nora");
		Append(bytes, std::uint32_t{ 12 });
		AppendString(bytes, "Cambridge");
		AppendString(bytes, "0d.1h.35m");
		AppendString(bytes, "HumanRace");
		Append(bytes, std::uint16_t{ 1 });
		Append(bytes, 192.5F);
		Append(bytes, 275.0F);
		Append(bytes, std::uint64_t{ 116444736000000000ULL });
		Append(bytes, std::uint32_t{ 2 });
		Append(bytes, std::uint32_t{ 1 });

		const auto headerSize =
			static_cast<std::uint32_t>(bytes.size() - headerSizeOffset - sizeof(std::uint32_t));
		std::memcpy(bytes.data() + headerSizeOffset, std::addressof(headerSize), sizeof(headerSize));
		return bytes;
	}
}

TEST_CASE("Fallout 4 save header parser reads metadata without screenshot pixels")
{
	const auto bytes = MakeHeader();
	const auto result =
		dvb::tools::game::ParseFallout4SaveHeader(bytes, bytes.size() + 8);
	CHECK(result.metadata.has_value());
	CHECK(result.error.empty());
	CHECK(result.metadata->formatVersion == 15);
	CHECK(result.metadata->saveNumber == 9);
	CHECK(result.metadata->characterName == "Nora");
	CHECK(result.metadata->level == 12);
	CHECK(result.metadata->location == "Cambridge");
	CHECK(result.metadata->race == "HumanRace");
	CHECK(result.metadata->screenshotWidth == 2);
	CHECK(result.metadata->screenshotHeight == 1);
	CHECK(result.metadata->fileTimeUnix == 0);
}

TEST_CASE("Fallout 4 save header parser rejects malformed and truncated data")
{
	auto badMagic = MakeHeader();
	badMagic[0] = std::byte{ 0 };
	const auto invalid =
		dvb::tools::game::ParseFallout4SaveHeader(badMagic, badMagic.size() + 8);
	CHECK(!invalid.metadata.has_value());
	CHECK(invalid.error.find("magic") != std::string::npos);

	const auto header = MakeHeader();
	const auto truncatedHeader = dvb::tools::game::ParseFallout4SaveHeader(
		std::span<const std::byte>(header).first(header.size() - 1), header.size() + 8);
	CHECK(!truncatedHeader.metadata.has_value());
	CHECK(truncatedHeader.error.find("truncated") != std::string::npos);

	const auto truncatedScreenshot =
		dvb::tools::game::ParseFallout4SaveHeader(header, header.size() + 7);
	CHECK(!truncatedScreenshot.metadata.has_value());
	CHECK(truncatedScreenshot.error.find("screenshot") != std::string::npos);

	auto invalidText = MakeHeader();
	// Character name begins at byte 26 in this fixed fixture.
	invalidText[26] = std::byte{ 0xFF };
	const auto invalidUtf8 =
		dvb::tools::game::ParseFallout4SaveHeader(invalidText, invalidText.size() + 8);
	CHECK(!invalidUtf8.metadata.has_value());
	CHECK(invalidUtf8.error.find("UTF-8") != std::string::npos);
}

TEST_CASE("Fallout 4 save header parser bounds declared header and screenshot sizes")
{
	auto       oversizedHeader = MakeHeader();
	const auto declared =
		static_cast<std::uint32_t>(dvb::tools::game::kMaxFallout4SaveHeaderBytes + 1);
	std::memcpy(
		oversizedHeader.data() + std::string_view("FO4_SAVEGAME").size(),
		std::addressof(declared), sizeof(declared));
	const auto headerResult = dvb::tools::game::ParseFallout4SaveHeader(
		oversizedHeader, oversizedHeader.size() + 8);
	CHECK(!headerResult.metadata.has_value());
	CHECK(headerResult.error.find("bound") != std::string::npos);

	auto       oversizedScreenshot = MakeHeader();
	const auto width = dvb::tools::game::kMaxFallout4SaveScreenshotDimension + 1;
	std::memcpy(
		oversizedScreenshot.data() + oversizedScreenshot.size() - 8,
		std::addressof(width), sizeof(width));
	const auto screenshotResult = dvb::tools::game::ParseFallout4SaveHeader(
		oversizedScreenshot, oversizedScreenshot.size() + 8);
	CHECK(!screenshotResult.metadata.has_value());
	CHECK(screenshotResult.error.find("dimensions") != std::string::npos);
}

#pragma once

#include "tools/game/SaveMetadata.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace dvb::tools::game
{
	inline constexpr std::size_t   kMaxFallout4SaveHeaderBytes = 256 * 1024;
	inline constexpr std::size_t   kMaxFallout4SaveMetadataStringBytes = 4096;
	inline constexpr std::uint32_t kMaxFallout4SaveScreenshotDimension = 16384;

	// Parses only the bounded FO4_SAVEGAME header. a_fileSize is used to verify
	// that the declared BGRA screenshot is present without reading its pixels.
	SaveMetadataResult ParseFallout4SaveHeader(
		std::span<const std::byte> a_headerBytes, std::uintmax_t a_fileSize);
	SaveMetadataResult ReadFallout4SaveHeader(const std::filesystem::path& a_path);

	std::int64_t Fallout4FileTimeToUnix(std::uint64_t a_fileTime);
}

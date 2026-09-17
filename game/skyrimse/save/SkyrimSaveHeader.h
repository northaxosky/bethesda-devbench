#pragma once

#include "tools/game/SaveMetadata.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace dvb::skyrimse::save
{
	inline constexpr std::size_t   kMaxSkyrimSaveHeaderBytes = 256 * 1024;
	inline constexpr std::size_t   kMaxSkyrimSaveMetadataStringBytes = 4096;
	inline constexpr std::uint32_t kMaxSkyrimSaveScreenshotDimension = 16384;

	// Parses only the bounded TESV_SAVEGAME header. a_fileSize verifies that the
	// declared RGB screenshot is present without reading its pixels.
	tools::game::SaveMetadataResult ParseSkyrimSaveHeader(
		std::span<const std::byte> a_headerBytes, std::uintmax_t a_fileSize);
	tools::game::SaveMetadataResult ReadSkyrimSaveHeader(const std::filesystem::path& a_path);

	std::int64_t SkyrimFileTimeToUnix(std::uint64_t a_fileTime);
}

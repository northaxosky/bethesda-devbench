#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dvb::tools::game
{
	struct SaveMetadata
	{
		std::uint32_t               formatVersion = 0;
		std::uint32_t               saveNumber = 0;
		std::string                 characterName;
		std::uint32_t               level = 0;
		std::string                 location;
		std::string                 playTime;
		std::string                 race;
		std::uint16_t               sex = 0;
		float                       currentExperience = 0.0F;
		float                       requiredExperience = 0.0F;
		std::optional<std::int64_t> fileTimeUnix;
		std::uint32_t               screenshotWidth = 0;
		std::uint32_t               screenshotHeight = 0;
	};

	struct SaveMetadataResult
	{
		std::optional<SaveMetadata> metadata;
		std::string                 error;
	};
}

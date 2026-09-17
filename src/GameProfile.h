#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dvb
{
	enum class GameFamily
	{
		kFallout4,
		kSkyrim,
	};

	enum class RuntimeVariant
	{
		kFallout4AE,
		kSkyrimSE,
		kSkyrimAE,
		kSkyrimVR,
	};

	// Engine-free identity and filesystem policy for one native adapter build.
	// A process hosts exactly one profile; incompatible RE SDKs are selected at
	// build time rather than discovered through a runtime adapter registry.
	struct GameProfile
	{
		GameFamily                    family = GameFamily::kFallout4;
		RuntimeVariant                variant = RuntimeVariant::kFallout4AE;
		std::string                   id;
		std::string                   displayName;
		std::string                   runtimeVariant;
		std::string                   extenderName;
		std::string                   executableName;
		std::vector<std::string>      aliases;
		int                           defaultPort = 0;
		std::filesystem::path         pluginDataDirectory;
		std::filesystem::path         externalStateDirectory;
		std::filesystem::path         recordingsDirectory;
		std::filesystem::path         captureDirectory;
		std::vector<std::filesystem::path> captureScanDirectories;
		std::string                   saveExtension;
		std::string                   coSaveExtension;
		std::vector<std::string>      recordingGameIds;
		std::vector<std::string>      recordingRuntimeCompatibility;
		std::vector<std::string>      contextFreeMenus;
		bool                          vr = false;
	};

	GameProfile Fallout4Profile();
	GameProfile SkyrimProfile(RuntimeVariant a_variant);

	bool IsRecordingGameCompatible(
		const GameProfile& a_profile, std::string_view a_game);
	bool IsContextFreeMenu(
		const GameProfile& a_profile, std::string_view a_menu);
}

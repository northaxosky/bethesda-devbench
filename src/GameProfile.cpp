#include "GameProfile.h"

#include <algorithm>

namespace dvb
{
	namespace
	{
		std::string LowerAscii(std::string_view a_value)
		{
			std::string out(a_value);
			std::ranges::transform(out, out.begin(), [](unsigned char a_ch) {
				return static_cast<char>(
					a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return out;
		}
	}

	GameProfile Fallout4Profile()
	{
		return {
			.family = GameFamily::kFallout4,
			.variant = RuntimeVariant::kFallout4AE,
			.id = "fo4",
			.displayName = "Fallout 4",
			.runtimeVariant = "ae",
			.extenderName = "F4SE",
			.executableName = "Fallout4.exe",
			.aliases = { "fallout4" },
			.defaultPort = 8930,
			.pluginDataDirectory = "Data/F4SE/Plugins/devbench",
			.externalStateDirectory = "devbench/fo4",
			.recordingsDirectory = "Data/F4SE/Plugins/devbench/recordings",
			.captureDirectory = "Data/F4SE/Plugins/devbench/captures",
			.captureScanDirectories = { "", "Screenshots" },
			.saveExtension = ".fos",
			.coSaveExtension = ".f4se",
			.recordingGameIds = { "fo4", "fallout4", "fallout 4" },
			.recordingRuntimeCompatibility = { "ae" },
			.contextFreeMenus = { "PauseMenu" },
			.vr = false,
		};
	}

	GameProfile SkyrimProfile(RuntimeVariant a_variant)
	{
		const bool vr = a_variant == RuntimeVariant::kSkyrimVR;
		const bool ae = a_variant == RuntimeVariant::kSkyrimAE;
		return {
			.family = GameFamily::kSkyrim,
			.variant = a_variant,
			.id = vr ? "vr" : "se",
			.displayName = vr ? "Skyrim VR" : "Skyrim Special Edition",
			.runtimeVariant = vr ? "vr" : (ae ? "ae" : "se"),
			.extenderName = "SKSE",
			.executableName = vr ? "SkyrimVR.exe" : "SkyrimSE.exe",
			.aliases = vr ? std::vector<std::string>{ "skyrimvr" } :
			               std::vector<std::string>{ "skyrimse", "sse" },
			.defaultPort = vr ? 8921 : 8920,
			.pluginDataDirectory = "Data/SKSE/Plugins/devbench",
			.externalStateDirectory = vr ? "devbench/vr" : "devbench/se",
			.recordingsDirectory = "Data/SKSE/Plugins/devbench/recordings",
			.captureDirectory = "Data/SKSE/Plugins/devbench/captures",
			.captureScanDirectories = { "", "Screenshots" },
			.saveExtension = ".ess",
			.coSaveExtension = ".skse",
			.recordingGameIds = { "se", "vr", "skyrim", "skyrimse",
				"skyrim special edition", "skyrim vr" },
			.recordingRuntimeCompatibility =
				vr ? std::vector<std::string>{ "vr" } :
				     std::vector<std::string>{ "se", "ae" },
			.contextFreeMenus = { "Journal Menu" },
			.vr = vr,
		};
	}

	bool IsRecordingGameCompatible(
		const GameProfile& a_profile, std::string_view a_game)
	{
		if (a_game.empty())
			return true;
		const auto game = LowerAscii(a_game);
		return std::ranges::any_of(
			a_profile.recordingGameIds,
			[&](const std::string& a_candidate) {
				return LowerAscii(a_candidate) == game;
			});
	}

	bool IsContextFreeMenu(
		const GameProfile& a_profile, std::string_view a_menu)
	{
		return std::ranges::find(a_profile.contextFreeMenus, a_menu) !=
		       a_profile.contextFreeMenus.end();
	}
}

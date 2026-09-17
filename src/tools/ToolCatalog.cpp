#include "ToolCatalog.h"

#include "BridgeSetupTool.h"
#include "Compatibility.h"
#include "ConsoleTool.h"
#include "GameTool.h"
#include "InspectTool.h"
#include "PingTool.h"
#include "camera/CameraTool.h"
#include "capture/CaptureTool.h"
#include "input/InputTool.h"
#include "menu/MenuTool.h"
#include "papyrus/PapyrusTool.h"
#include "recording/RecordingTool.h"
#include "rest/RestTool.h"
#include "scenario/ScenarioTool.h"

#include <array>

namespace dvb::tools
{
	json BuildCoreToolCatalog()
	{
		return BuildCoreToolCatalog(Fallout4Profile());
	}

	json BuildCoreToolCatalog(const GameProfile& a_profile)
	{
		constexpr std::array<std::string_view, 3> inspectExtensions{ "saves", "saveload", "screenshots" };
		const GameSavePolicy gamePolicy{
			.gameName = a_profile.displayName,
			.saveExtension = a_profile.saveExtension,
			.coSaveExtension = a_profile.coSaveExtension,
		};
		const input::InputBackend inputBackend{
			.gameName = a_profile.displayName,
			.extenderName = a_profile.extenderName,
			.nativeDevices = a_profile.vr ?
			                     std::vector<std::string>{ "vrTrackedSet" } :
			                     std::vector<std::string>{},
		};
		const recording::RecordingBackend recordingBackend{
			.gameId = a_profile.id,
			.gameName = a_profile.displayName,
			.compatibleGameIds = a_profile.recordingGameIds,
			.compatibleRuntimeVariants =
				a_profile.recordingRuntimeCompatibility,
			.recordedOnVR = a_profile.vr,
		};
		const MenuBackend menuBackend{
			.gameName = a_profile.displayName,
			.contextFreeOpenMenus = a_profile.contextFreeMenus,
		};
		const std::array                          descriptors{
			BuildPingDescriptor(),
			BuildConsoleDescriptor(),
			BuildInspectDescriptor(inspectExtensions),
			BuildGameDescriptor(gamePolicy),
			scenario::BuildScenarioDescriptor(),
			BuildBridgeSetupDescriptor(),
			papyrus::BuildPapyrusDescriptor(),
			input::BuildInputDescriptor(&inputBackend),
			recording::BuildRecordDescriptor(&recordingBackend),
			recording::BuildRecordingsDescriptor(),
			BuildMenuDescriptor(&menuBackend),
			BuildCameraDescriptor(),
			capture::BuildCaptureDescriptor(),
			rest::BuildWaitDescriptor(),
			rest::BuildSleepDescriptor(),
		};
		json tools = json::array();
		for (const auto& descriptor : descriptors)
		{
			tools.push_back(json{
				{ "name", descriptor.name },
				{ "description", descriptor.description },
				{ "inputSchema", descriptor.inputSchema },
				{ "readOnly", descriptor.readOnly },
			});
		}
		return json{
			{ "format", "devbench.core-tools-1" },
			{ "game", a_profile.id },
			{ "profile",
				json{
					{ "id", a_profile.id },
					{ "displayName", a_profile.displayName },
					{ "extender", a_profile.extenderName },
					{ "executable", a_profile.executableName },
					{ "aliases", a_profile.aliases },
				} },
			{ "gameName", a_profile.displayName },
			{ "runtimeVariant", a_profile.runtimeVariant },
			{ "vr", a_profile.vr },
			{ "compatibility", CompatibilityMetadata() },
			{ "tools", std::move(tools) },
		};
	}

	json BuildCoreToolCatalogBundle()
	{
		return json{
			{ "format", "devbench.core-tools-2" },
			{ "catalogs",
				json::array({
					BuildCoreToolCatalog(Fallout4Profile()),
					BuildCoreToolCatalog(
						SkyrimProfile(RuntimeVariant::kSkyrimAE)),
					BuildCoreToolCatalog(
						SkyrimProfile(RuntimeVariant::kSkyrimVR)),
				}) },
		};
	}
}

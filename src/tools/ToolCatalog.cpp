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
		constexpr std::array<std::string_view, 3> inspectExtensions{ "saves", "saveload", "screenshots" };
		const std::array                          descriptors{
			BuildPingDescriptor(),
			BuildConsoleDescriptor(),
			BuildInspectDescriptor(inspectExtensions),
			BuildGameDescriptor(),
			scenario::BuildScenarioDescriptor(),
			BuildBridgeSetupDescriptor(),
			papyrus::BuildPapyrusDescriptor(),
			input::BuildInputDescriptor(),
			recording::BuildRecordDescriptor(),
			recording::BuildRecordingsDescriptor(),
			BuildMenuDescriptor(),
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
			{ "game", "fo4" },
			{ "compatibility", CompatibilityMetadata() },
			{ "tools", std::move(tools) },
		};
	}
}

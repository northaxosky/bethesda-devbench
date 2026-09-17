#include "BridgeSetupTool.h"

#include <memory>

namespace dvb::tools
{
	namespace
	{
		json HandleBridgeSetup(const json& a_args, const BridgeDiscoveryBackend& a_discovery)
		{
			if (!a_args.is_object())
				throw ToolError(400, "mcp_bridge_setup arguments must be an object");
			if (!a_args.empty())
				throw ToolError(400, "mcp_bridge_setup does not accept arguments");
			if (!a_discovery)
				throw ToolError(503, "bridge discovery is unavailable");
			return a_discovery();
		}
	}

	ToolDescriptor BuildBridgeSetupDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "mcp_bridge_setup";
		descriptor.description =
			"Read installation and MCP client configuration details for the packaged external bridge. "
			"This tool never installs the bridge or modifies client configuration.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "additionalProperties", false },
		};
		descriptor.readOnly = true;
		return descriptor;
	}

	void RegisterBridgeSetupTool(
		ToolRegistry& a_registry, BridgeDiscoveryBackend a_discovery)
	{
		auto discovery = std::make_shared<BridgeDiscoveryBackend>(std::move(a_discovery));
		a_registry.Register(BuildBridgeSetupDescriptor(),
			[discovery = std::move(discovery)](const json& a_args, const ToolContext&) {
				return HandleBridgeSetup(a_args, *discovery);
			});
	}
}

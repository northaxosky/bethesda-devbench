#pragma once

#include "ToolRegistry.h"

#include <functional>

namespace dvb::tools
{
	using BridgeDiscoveryBackend = std::function<json()>;

	ToolDescriptor BuildBridgeSetupDescriptor();
	void           RegisterBridgeSetupTool(
		ToolRegistry& a_registry, BridgeDiscoveryBackend a_discovery);
}

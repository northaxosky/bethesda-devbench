#pragma once

#include "ToolRegistry.h"
#include "tools/inspection/InspectionQuery.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace dvb::tools
{
	struct PlayerReadinessInput
	{
		bool gameDataReady = false;
		bool inMainMenu = false;
		bool inLoadingMenu = false;
		bool loadInProgress = false;
		bool loadFailed = false;
		bool hasPlayer = false;
		bool hasNpc = false;
		bool has3D = false;
	};

	struct PlayerReadiness
	{
		bool        loaded = false;
		std::string reason;
	};

	PlayerReadiness AssessPlayerReadiness(const PlayerReadinessInput& a_input);

	struct InspectBackend
	{
		std::function<json()>                           health;
		std::function<json()>                           state;
		std::function<json()>                           player;
		std::function<json()>                           scene;
		std::function<json()>                           mods;
		std::function<json(const inspection::Request&)> vm;
		std::function<json(const inspection::Request&)> inventory;
		std::function<json(const inspection::Request&)> quests;
		std::function<json(const inspection::Request&)> effects;
		std::function<json(const inspection::Request&)> refs;
		std::function<json(const inspection::Request&)> registrants;
	};

	ToolDescriptor BuildInspectDescriptor(std::span<const std::string_view> a_declaredExtensions = {});
	void           RegisterInspectTool(
		ToolRegistry&  a_registry,
		InspectBackend a_backend,
		bool           a_refreshDescriptorOnExtension = false);
}

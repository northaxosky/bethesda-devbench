#pragma once

#include "tools/InspectTool.h"

namespace dvb::fallout4
{
	struct LivePlayer
	{
		RE::PlayerCharacter*   player = nullptr;
		RE::TESNPC*            npc = nullptr;
		bool                   inMainMenu = false;
		bool                   inLoadingMenu = false;
		tools::PlayerReadiness readiness;
	};

	/// The one loaded-world gate; main thread only. player/npc stay null until ready.
	LivePlayer ReadLivePlayer();
}

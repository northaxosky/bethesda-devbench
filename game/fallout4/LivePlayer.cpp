#include "LivePlayer.h"

#include "Lifecycle.h"

namespace dvb::fallout4
{
	LivePlayer ReadLivePlayer()
	{
		const auto                  lifecycle = Lifecycle::GetSnapshot();
		const auto                  ui = RE::UI::GetSingleton();
		const bool                  inMainMenu = ui ? ui->GetMenuOpen<RE::MainMenu>() : lifecycle.inMainMenu;
		const bool                  inLoadingMenu = ui ? ui->GetMenuOpen<RE::LoadingMenu>() : lifecycle.inLoadingMenu;
		tools::PlayerReadinessInput input{
			.gameDataReady = lifecycle.gameDataReady,
			.inMainMenu = inMainMenu,
			.inLoadingMenu = inLoadingMenu,
			.loadInProgress = lifecycle.loadInProgress,
			.loadFailed = lifecycle.postLoadSucceeded == false,
		};
		// A pre-load notification can precede the loading menu while the old actor still exists.
		if (!input.gameDataReady || input.inMainMenu || input.inLoadingMenu || input.loadInProgress || input.loadFailed)
			return { nullptr, nullptr, inMainMenu, inLoadingMenu, tools::AssessPlayerReadiness(input) };
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto npc = player ? player->GetNPC() : nullptr;
		input.hasPlayer = player != nullptr;
		input.hasNpc = npc != nullptr;
		input.has3D = npc && player->Get3D() != nullptr;
		return { player, npc, inMainMenu, inLoadingMenu, tools::AssessPlayerReadiness(input) };
	}
}

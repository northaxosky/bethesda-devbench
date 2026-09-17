#include "InspectBackend.h"

#include "GameState.h"
#include "Lifecycle.h"
#include "MainThread.h"
#include "Server.h"
#include "Version.h"
#include "inspection/Form.h"
#include "inspection/Readers.h"

#include <cmath>

namespace dvb::fallout4
{
	namespace
	{
		json Identity(std::string_view a_runtimeVersion)
		{
			json identity{
				{ "game", "Fallout 4" },
				{ "runtime", a_runtimeVersion },
			};
			identity.update(InstanceIdentity());
			return identity;
		}

		bool HasVisibleText(std::string_view a_text)
		{
			return a_text.find_first_not_of(" \t\r\n\f\v") != std::string_view::npos;
		}

		struct LivePlayer
		{
			RE::PlayerCharacter*   player = nullptr;
			RE::TESNPC*            npc = nullptr;
			bool                   inMainMenu = false;
			bool                   inLoadingMenu = false;
			tools::PlayerReadiness readiness;
		};

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

		json UnavailablePlayer(const LivePlayer& a_live)
		{
			return json{
				{ "playerLoaded", false },
				{ "unavailableReason", a_live.readiness.reason },
			};
		}

		json ActorValue(const RE::PlayerCharacter& a_player, const RE::ActorValueInfo* a_info)
		{
			if (!a_info)
				return nullptr;
			auto number = [](float a_value) -> json {
				return std::isfinite(a_value) ? json(a_value) : json(nullptr);
			};
			return json{
				{ "current", number(a_player.GetActorValue(*a_info)) },
				{ "base", number(a_player.GetBaseActorValue(*a_info)) },
				{ "permanent", number(a_player.GetPermanentActorValue(*a_info)) },
			};
		}

		json ReadState(std::string_view a_runtimeVersion)
		{
			auto out = MainThread::RunAndWait([runtime = std::string(a_runtimeVersion)]() -> json {
				const auto lifecycle = Lifecycle::GetSnapshot();
				const auto live = ReadLivePlayer();
				json       menus = lifecycle.openMenus;
				json       state{
					{ "plugin", "devbench" },
					{ "version", DEVBENCH_VERSION_STRING },
					{ "game", "Fallout 4" },
					{ "runtime", runtime },
					{ "playerLoaded", live.readiness.loaded },
					{ "unavailableReason", live.readiness.loaded ? json(nullptr) : json(live.readiness.reason) },
					{ "menus", json{
								   { "mainMenu", live.inMainMenu },
								   { "loadingMenu", live.inLoadingMenu },
								   { "trackingReady", lifecycle.menuTrackingReady },
								   { "open", std::move(menus) },
							   } },
					{ "lifecycle", json{
									   { "gameDataReady", lifecycle.gameDataReady },
									   { "gameLoaded", lifecycle.gameLoaded },
									   { "loadInProgress", lifecycle.loadInProgress || live.inLoadingMenu },
									   { "postLoadSucceeded", lifecycle.postLoadSucceeded ? json(*lifecycle.postLoadSucceeded) : json(nullptr) },
									   { "lastEvent", lifecycle.lastEvent.empty() ? json(nullptr) : json(lifecycle.lastEvent) },
								   } },
					{ "frame", game::CurrentFrame() },
				};
				return state;
			});
			out.update(InstanceIdentity());
			return out;
		}

		json ReadPlayer()
		{
			return MainThread::RunAndWait([]() -> json {
				const auto live = ReadLivePlayer();
				if (!live.readiness.loaded)
					return UnavailablePlayer(live);

				json out{
					{ "playerLoaded", true },
					{ "name", nullptr },
					{ "level", live.player->GetLevel() },
					{ "gold", live.player->GetGoldAmount() },
					{ "race", inspection::SerializeForm(live.player->race) },
				};
				if (const auto name = live.npc->GetFullName(); name && HasVisibleText(name))
					out["name"] = name;
				switch (live.player->GetSex())
				{
					case RE::SEX::kMale:
						out["sex"] = "male";
						break;
					case RE::SEX::kFemale:
						out["sex"] = "female";
						break;
					default:
						out["sex"] = "none";
						break;
				}

				if (const auto values = RE::ActorValue::GetSingleton())
				{
					out["actorValues"] = json{
						{ "health", ActorValue(*live.player, values->health) },
						{ "actionPoints", ActorValue(*live.player, values->actionPoints) },
						{ "rads", ActorValue(*live.player, values->rads) },
						{ "carryWeight", ActorValue(*live.player, values->carryWeight) },
						{ "strength", ActorValue(*live.player, values->strength) },
						{ "perception", ActorValue(*live.player, values->perception) },
						{ "endurance", ActorValue(*live.player, values->endurance) },
						{ "charisma", ActorValue(*live.player, values->charisma) },
						{ "intelligence", ActorValue(*live.player, values->intelligence) },
						{ "agility", ActorValue(*live.player, values->agility) },
						{ "luck", ActorValue(*live.player, values->luck) },
					};
				}
				else
				{
					out["actorValues"] = nullptr;
				}

				std::vector<RE::TESBoundObject*> equippedForms;
				if (const auto inventory = live.player->inventoryList)
				{
					const RE::BSAutoReadLock lock(inventory->rwLock);
					for (const auto& item : inventory->data)
					{
						if (!item.object)
							continue;
						bool isEquipped = false;
						for (auto stack = item.stackData; stack; stack = stack->nextStack)
							isEquipped = isEquipped || stack->IsEquipped();
						if (isEquipped)
							equippedForms.push_back(item.object);
					}
				}
				json equipped = json::array();
				for (const auto* form : equippedForms)
					equipped.push_back(inspection::SerializeForm(form));
				out["equipped"] = json{
					{ "items", std::move(equipped) },
					{ "projection", "FO4 inventory stack equip flags; no Skyrim left/right slot projection" },
				};
				return out;
			});
		}

		json ReadScene()
		{
			return MainThread::RunAndWait([]() -> json {
				const auto live = ReadLivePlayer();
				if (!live.readiness.loaded)
					return UnavailablePlayer(live);

				const auto cell = live.player->GetParentCell();
				const auto location = live.player->GetCurrentLocation();
				const auto worldspace = cell && !cell->IsInterior() ? cell->worldSpace : nullptr;
				const auto position = live.player->GetPosition();
				json       out{
					{ "playerLoaded", true },
					{ "cell", inspection::SerializeForm(cell) },
					{ "worldspace", inspection::SerializeForm(worldspace) },
					{ "location", inspection::SerializeForm(location) },
					{ "currentScene", inspection::SerializeForm(live.player->GetCurrentScene()) },
					{ "sceneActionActive", static_cast<bool>(live.player->sceneActionActive) },
					{ "position", json{ { "x", position.x }, { "y", position.y }, { "z", position.z } } },
					{ "gameHour", nullptr },
					{ "daysPassed", nullptr },
					{ "weather", nullptr },
				};
				if (cell)
					out["cell"]["interior"] = cell->IsInterior();
				if (const auto calendar = RE::Calendar::GetSingleton())
				{
					if (calendar->gameHour)
						out["gameHour"] = calendar->gameHour->GetValue();
					if (calendar->gameDaysPassed)
						out["daysPassed"] = calendar->gameDaysPassed->GetValue();
				}
				if (const auto sky = RE::Sky::GetSingleton(); sky && sky->currentWeather)
					out["weather"] = inspection::SerializeForm(sky->currentWeather);
				return out;
			});
		}

		json ReadMods()
		{
			if (!Lifecycle::GetSnapshot().gameDataReady)
				throw ToolError(503, "inspect mods is unavailable until F4SE gameDataReady");

			return MainThread::RunAndWait([]() -> json {
				if (!Lifecycle::GetSnapshot().gameDataReady)
					throw ToolError(503, "inspect mods is unavailable until F4SE gameDataReady");
				const auto data = RE::TESDataHandler::GetSingleton();
				if (!data)
					throw ToolError(503, "TESDataHandler unavailable");

				json full = json::array();
				for (const auto file : data->compiledFileCollection.files)
				{
					if (!file)
						continue;
					full.push_back(json{
						{ "name", std::string(file->GetFilename()) },
						{ "index", file->GetCompileIndex() },
						{ "loadIndex", file->GetCompileIndex() },
						{ "loadIndexHex", std::format("{:02X}", file->GetCompileIndex()) },
						{ "light", false },
					});
				}

				json light = json::array();
				for (const auto file : data->compiledFileCollection.smallFiles)
				{
					if (!file)
						continue;
					light.push_back(json{
						{ "name", std::string(file->GetFilename()) },
						{ "index", file->GetSmallFileCompileIndex() },
						{ "lightIndex", file->GetSmallFileCompileIndex() },
						{ "lightIndexHex", std::format("{:03X}", file->GetSmallFileCompileIndex()) },
						{ "loadIndex", file->GetCompileIndex() },
						{ "light", true },
					});
				}

				const auto fullCount = full.size();
				const auto lightCount = light.size();
				return json{
					{ "gameDataReady", true },
					{ "count", fullCount },
					{ "lightCount", lightCount },
					{ "total", fullCount + lightCount },
					{ "plugins", std::move(full) },
					{ "lightPlugins", std::move(light) },
				};
			});
		}
	}

	tools::InspectBackend MakeInspectBackend(std::string a_runtimeVersion)
	{
		return tools::InspectBackend{
			.health = [runtime = a_runtimeVersion] {
				const auto lifecycle = Lifecycle::GetSnapshot();
				json       out{
						  { "ok", true },
						  { "frame", game::CurrentFrame() },
						  { "lastTaskFrame", MainThread::LastCompletedFrame() },
						  { "pendingTasks", MainThread::PendingTasks() },
						  { "lastLifecycle", lifecycle.lastEvent.empty() ? json(nullptr) : json(lifecycle.lastEvent) },
				};
				out.update(Identity(runtime));
				return out; },
			.state = [runtime = a_runtimeVersion] { return ReadState(runtime); },
			.player = &ReadPlayer,
			.scene = &ReadScene,
			.mods = &ReadMods,
			.vm = &inspection::ReadVm,
			.inventory = &inspection::ReadInventory,
			.quests = &inspection::ReadQuests,
			.effects = &inspection::ReadEffects,
			.refs = &inspection::ReadReferences,
			.registrants = &inspection::ReadRegistrants,
		};
	}
}

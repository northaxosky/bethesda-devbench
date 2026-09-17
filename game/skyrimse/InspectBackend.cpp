#include "InspectBackend.h"

#include "GameState.h"
#include "Lifecycle.h"
#include "MainThread.h"
#include "Server.h"
#include "Version.h"
#include "inspection/Form.h"
#include "inspection/Readers.h"

#include <cmath>

namespace dvb::skyrimse
{
	namespace
	{
		json Identity(std::string_view a_runtimeVersion)
		{
			json identity{
				{ "game", "Skyrim" },
				{ "runtime", a_runtimeVersion },
			};
			identity.update(InstanceIdentity());
			return identity;
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
			const auto lifecycle = Lifecycle::GetSnapshot();
			const auto ui = RE::UI::GetSingleton();
			const bool inMainMenu =
				ui ? ui->IsMenuOpen(RE::MainMenu::MENU_NAME) : lifecycle.inMainMenu;
			const bool inLoadingMenu =
				ui ? ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) : lifecycle.inLoadingMenu;
			tools::PlayerReadinessInput input{
				.gameDataReady = lifecycle.gameDataReady,
				.inMainMenu = inMainMenu,
				.inLoadingMenu = inLoadingMenu,
				.loadInProgress = lifecycle.loadInProgress,
				.loadFailed = lifecycle.postLoadSucceeded == false,
			};
			if (!input.gameDataReady || !lifecycle.gameLoaded || input.inMainMenu ||
				input.inLoadingMenu || input.loadInProgress || input.loadFailed)
				return {
					nullptr,
					nullptr,
					inMainMenu,
					inLoadingMenu,
					tools::AssessPlayerReadiness(input),
				};

			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto npc = player ? player->GetActorBase() : nullptr;
			input.hasPlayer = player != nullptr;
			input.hasNpc = npc != nullptr;
			input.has3D = player && player->Get3D() != nullptr;
			return {
				player,
				npc,
				inMainMenu,
				inLoadingMenu,
				tools::AssessPlayerReadiness(input),
			};
		}

		json UnavailablePlayer(const LivePlayer& a_live)
		{
			return json{
				{ "playerLoaded", false },
				{ "unavailableReason", a_live.readiness.reason },
			};
		}

		json Number(float a_value)
		{
			return std::isfinite(a_value) ? json(a_value) : json(nullptr);
		}

		json ActorValue(const RE::PlayerCharacter& a_player, RE::ActorValue a_value)
		{
			const auto owner = a_player.AsActorValueOwner();
			if (!owner)
				return nullptr;
			return json{
				{ "current", Number(owner->GetActorValue(a_value)) },
				{ "base", Number(owner->GetBaseActorValue(a_value)) },
				{ "permanent", Number(owner->GetPermanentActorValue(a_value)) },
			};
		}

		json ReadState(std::string_view a_runtimeVersion)
		{
			auto out =
				MainThread::RunAndWait([runtime = std::string(a_runtimeVersion)]() -> json {
					const auto lifecycle = Lifecycle::GetSnapshot();
					const auto live = ReadLivePlayer();
					json       menus = lifecycle.openMenus;
					return json{
						{ "plugin", "devbench" },
						{ "version", DEVBENCH_VERSION_STRING },
						{ "game", "Skyrim" },
						{ "runtime", runtime },
						{ "playerLoaded", live.readiness.loaded },
						{ "unavailableReason",
							live.readiness.loaded ?
								json(nullptr) :
								json(live.readiness.reason) },
						{ "menus",
							json{
								{ "mainMenu", live.inMainMenu },
								{ "loadingMenu", live.inLoadingMenu },
								{ "trackingReady", lifecycle.menuTrackingReady },
								{ "open", std::move(menus) },
							} },
						{ "lifecycle",
							json{
								{ "gameDataReady", lifecycle.gameDataReady },
								{ "gameLoaded", lifecycle.gameLoaded },
								{ "loadInProgress",
									lifecycle.loadInProgress || live.inLoadingMenu },
								{ "postLoadSucceeded",
									lifecycle.postLoadSucceeded ?
										json(*lifecycle.postLoadSucceeded) :
										json(nullptr) },
								{ "lastEvent",
									lifecycle.lastEvent.empty() ?
										json(nullptr) :
										json(lifecycle.lastEvent) },
							} },
						{ "frame", game::CurrentFrame() },
					};
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
					{ "race", inspection::SerializeForm(live.player->GetRace()) },
				};
				if (const auto name = live.player->GetName(); name && *name)
					out["name"] = std::string(name);
				switch (live.npc->GetSex())
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

				out["actorValues"] = json{
					{ "health", ActorValue(*live.player, RE::ActorValue::kHealth) },
					{ "magicka", ActorValue(*live.player, RE::ActorValue::kMagicka) },
					{ "stamina", ActorValue(*live.player, RE::ActorValue::kStamina) },
					{ "carryWeight",
						ActorValue(*live.player, RE::ActorValue::kCarryWeight) },
				};
				out["equipped"] = json{
					{ "right",
						inspection::SerializeForm(
							live.player->GetEquippedObject(false)) },
					{ "left",
						inspection::SerializeForm(
							live.player->GetEquippedObject(true)) },
					{ "ammo",
						inspection::SerializeForm(live.player->GetCurrentAmmo()) },
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
				const auto position = live.player->GetPosition();
				json out{
					{ "playerLoaded", true },
					{ "cell", inspection::SerializeForm(cell) },
					{ "worldspace",
						inspection::SerializeForm(live.player->GetWorldspace()) },
					{ "location",
						inspection::SerializeForm(
							live.player->GetCurrentLocation()) },
					{ "currentScene",
						inspection::SerializeForm(
							live.player->GetCurrentScene()) },
					{ "position",
						json{
							{ "x", position.x },
							{ "y", position.y },
							{ "z", position.z },
						} },
					{ "gameHour", nullptr },
					{ "daysPassed", nullptr },
					{ "weather", nullptr },
				};
				if (cell)
					out["cell"]["interior"] = cell->IsInteriorCell();
				if (const auto calendar = RE::Calendar::GetSingleton())
				{
					if (calendar->gameHour)
						out["gameHour"] = calendar->gameHour->value;
					if (calendar->gameDaysPassed)
						out["daysPassed"] = calendar->gameDaysPassed->value;
				}
				if (const auto sky = RE::Sky::GetSingleton();
					sky && sky->currentWeather)
					out["weather"] =
						inspection::SerializeForm(sky->currentWeather);
				return out;
			});
		}

		json ReadMods()
		{
			if (!Lifecycle::GetSnapshot().gameDataReady)
				throw ToolError(
					503, "inspect mods is unavailable until SKSE dataLoaded");

			return MainThread::RunAndWait([]() -> json {
				if (!Lifecycle::GetSnapshot().gameDataReady)
					throw ToolError(
						503, "inspect mods is unavailable until SKSE dataLoaded");
				const auto data = RE::TESDataHandler::GetSingleton();
				if (!data)
					throw ToolError(503, "TESDataHandler unavailable");

				json full = json::array();
				const auto fullCount = data->GetLoadedModCount();
				const auto fullFiles = data->GetLoadedMods();
				for (std::uint16_t index = 0; index < fullCount; ++index)
				{
					const auto file = fullFiles ? fullFiles[index] : nullptr;
					if (!file)
						continue;
					full.push_back(json{
						{ "name", std::string(file->GetFilename()) },
						{ "index", file->GetCompileIndex() },
						{ "loadIndex", file->GetCompileIndex() },
						{ "loadIndexHex",
							std::format("{:02X}", file->GetCompileIndex()) },
						{ "light", false },
					});
				}

				json light = json::array();
				const auto lightCount = data->GetLoadedLightModCount();
				const auto lightFiles = data->GetLoadedLightMods();
				for (std::uint32_t index = 0; index < lightCount; ++index)
				{
					const auto file = lightFiles ? lightFiles[index] : nullptr;
					if (!file)
						continue;
					light.push_back(json{
						{ "name", std::string(file->GetFilename()) },
						{ "index", file->GetSmallFileCompileIndex() },
						{ "lightIndex", file->GetSmallFileCompileIndex() },
						{ "lightIndexHex",
							std::format(
								"{:03X}", file->GetSmallFileCompileIndex()) },
						{ "loadIndex", file->GetCompileIndex() },
						{ "light", true },
					});
				}

				return json{
					{ "gameDataReady", true },
					{ "count", full.size() },
					{ "lightCount", light.size() },
					{ "total", full.size() + light.size() },
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
				json out{
					{ "ok", true },
					{ "frame", game::CurrentFrame() },
					{ "lastTaskFrame", MainThread::LastCompletedFrame() },
					{ "pendingTasks", MainThread::PendingTasks() },
					{ "lastLifecycle",
						lifecycle.lastEvent.empty() ?
							json(nullptr) :
							json(lifecycle.lastEvent) },
				};
				out.update(Identity(runtime));
				return out;
			},
			.state = [runtime = a_runtimeVersion] {
				return ReadState(runtime);
			},
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

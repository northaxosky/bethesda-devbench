#include "Readers.h"

#include "Form.h"
#include "MainThread.h"
#include "ToolExtensions.h"
#include "game/fallout4/Lifecycle.h"
#include "xse/HostApi.h"

#include <map>

namespace dvb::fallout4::inspection
{
	namespace
	{
		using Request = tools::inspection::Request;
		using ResultWindow = tools::inspection::ResultWindow;

		void RequireStableWorld(std::string_view a_kind)
		{
			const auto lifecycle = Lifecycle::GetSnapshot();
			if (!lifecycle.gameDataReady)
				throw ToolError(503, std::format("inspect {}: game data is not ready", a_kind));
			if (lifecycle.inMainMenu)
				throw ToolError(503, std::format("inspect {}: unavailable in the main menu", a_kind));
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				throw ToolError(503, std::format("inspect {}: unavailable while a load is in progress", a_kind));
			if (lifecycle.postLoadSucceeded == false)
				throw ToolError(503, std::format("inspect {}: the last load failed", a_kind));
		}

		bool HasQuestFlag(const RE::TESQuest& a_quest, RE::QuestFlag a_flag)
		{
			return (a_quest.data.flags & static_cast<std::uint16_t>(a_flag)) != 0;
		}

		std::string ObjectiveState(RE::QUEST_OBJECTIVE_STATE a_state)
		{
			switch (a_state)
			{
				case RE::QUEST_OBJECTIVE_STATE::kDisplayed:
					return "displayed";
				case RE::QUEST_OBJECTIVE_STATE::kCompleted:
				case RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed:
					return "completed";
				case RE::QUEST_OBJECTIVE_STATE::kFailed:
				case RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed:
					return "failed";
				default:
					return "dormant";
			}
		}

		bool IsJournalObjective(RE::QUEST_OBJECTIVE_STATE a_state)
		{
			// FO4's journal accepts the displayed variants. The plain completed/failed
			// states are intentionally absent from the Pip-Boy journal.
			return a_state == RE::QUEST_OBJECTIVE_STATE::kDisplayed ||
			       a_state == RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed ||
			       a_state == RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed;
		}

		std::string ExpandObjective(
			const RE::BGSQuestObjective& a_objective,
			const RE::TESQuest&          a_quest,
			std::uint32_t                a_instanceId)
		{
			if (a_objective.displayText.QEmpty())
				return {};
			RE::BSString text;
			if (!text.Set(a_objective.displayText.QString(), a_objective.displayText.QLength()))
				return {};
			RE::BGSQuestInstanceText::ParseString(&text, &a_quest, a_instanceId);
			return text.data() ? std::string(text.data(), text.size()) : std::string{};
		}

		RE::TESObjectREFR* ResolveReferenceTarget(
			std::string_view   a_identifier,
			RE::TESObjectREFR* a_default,
			std::string_view   a_kind)
		{
			if (a_identifier.empty())
			{
				if (!a_default)
					throw ToolError(503, std::format("inspect {}: player unavailable", a_kind));
				return a_default;
			}

			auto* form = ResolveForm(a_identifier);
			if (!form)
				throw ToolError(404, std::format("inspect {}: form '{}' not found", a_kind, a_identifier));
			auto* reference = form->As<RE::TESObjectREFR>();
			if (!reference)
				throw ToolError(
					422, std::format("inspect {}: form '{}' is not a placed reference", a_kind, a_identifier));
			return reference;
		}

		RE::Actor* ResolveActorTarget(
			std::string_view a_identifier,
			RE::Actor*       a_default,
			std::string_view a_kind)
		{
			auto* reference = ResolveReferenceTarget(a_identifier, a_default, a_kind);
			auto* actor = reference->As<RE::Actor>();
			if (!actor)
				throw ToolError(
					422, std::format("inspect {}: form '{}' is not an actor", a_kind, a_identifier));
			return actor;
		}

		json ActorValue(const RE::Actor& a_actor, const RE::ActorValueInfo* a_info)
		{
			if (!a_info)
				return nullptr;
			auto number = [](float a_value) -> json {
				return std::isfinite(a_value) ? json(a_value) : json(nullptr);
			};
			return json{
				{ "current", number(a_actor.GetActorValue(*a_info)) },
				{ "base", number(a_actor.GetBaseActorValue(*a_info)) },
				{ "permanent", number(a_actor.GetPermanentActorValue(*a_info)) },
			};
		}

		json SerializeReference(RE::TESObjectREFR* a_reference)
		{
			if (!a_reference)
				return nullptr;

			json out = SerializeForm(a_reference);
			out["base"] = SerializeForm(a_reference->GetObjectReference());
			const auto position = a_reference->GetPosition();
			out["position"] = json{
				{ "x", position.x },
				{ "y", position.y },
				{ "z", position.z },
			};

			if (auto* actor = a_reference->As<RE::Actor>())
			{
				json actorState{
					{ "level", actor->GetLevel() },
				};
				if (const auto values = RE::ActorValue::GetSingleton())
					actorState["health"] = ActorValue(*actor, values->health);
				if (const auto player = RE::PlayerCharacter::GetSingleton(); player && actor != player)
					actorState["hostileToPlayer"] = actor->GetHostileToActor(player);
				out["actor"] = std::move(actorState);
			}
			return out;
		}

		bool MatchesType(const RE::TESForm* a_form, std::string_view a_filter)
		{
			if (!a_form)
				return false;
			const auto type = a_form->GetFormTypeString();
			return type && tools::inspection::MatchesFormType(type, a_filter);
		}
	}

	json ReadVm(const Request&)
	{
		return MainThread::RunAndWait([]() -> json {
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm)
				return json{ { "available", false } };

			std::size_t   loadedTypes = 0;
			std::size_t   attachedScripts = 0;
			std::size_t   arrays = 0;
			std::size_t   structs = 0;
			std::size_t   runningStacks = 0;
			std::uint32_t frozenStacks = 0;
			{
				RE::BSAutoLock lock(vm->typeInfoLock);
				loadedTypes = vm->objectTypeMap.size();
			}
			{
				RE::BSAutoLock lock(vm->attachedScriptsLock);
				attachedScripts = vm->attachedScripts.size();
			}
			{
				RE::BSAutoLock lock(vm->arraysLock);
				arrays = vm->arrays.size();
			}
			{
				RE::BSAutoLock lock(vm->structsLock);
				structs = vm->allStructs.size();
			}
			{
				RE::BSAutoLock lock(vm->runningStacksLock);
				runningStacks = vm->allRunningStacks.size();
			}
			{
				RE::BSAutoLock lock(vm->frozenStacksLock);
				frozenStacks = vm->frozenStacksCount;
			}
			return json{
				{ "available", true },
				{ "initialized", vm->initialized },
				{ "loadedTypes", loadedTypes },
				{ "attachedScripts", attachedScripts },
				{ "arrays", arrays },
				{ "structs", structs },
				{ "runningStacks", runningStacks },
				{ "frozenStacks", frozenStacks },
				{ "overstressed", vm->overstressed },
			};
		});
	}

	json ReadInventory(const Request& a_request)
	{
		return MainThread::RunAndWait([request = a_request]() -> json {
			RequireStableWorld("inventory");
			auto* owner = ResolveReferenceTarget(
				request.formId, RE::PlayerCharacter::GetSingleton(), "inventory");
			json         ownerIdentity = SerializeForm(owner);
			ResultWindow items(request.limit);
			auto*        inventory = owner->inventoryList;
			if (!inventory)
			{
				items.MarkComplete();
				auto out = items.Build("items");
				out["owner"] = std::move(ownerIdentity);
				out["inventoryAvailable"] = false;
				return out;
			}

			struct ItemSnapshot
			{
				RE::TESBoundObject* object = nullptr;
				std::uint64_t       count = 0;
				std::size_t         stackCount = 0;
				bool                equipped = false;
			};

			std::vector<ItemSnapshot> snapshots;
			bool                      complete = true;
			{
				const RE::BSAutoReadLock lock(inventory->rwLock);
				for (const auto& item : inventory->data)
				{
					if (!item.object || !MatchesType(item.object, request.formType))
						continue;

					std::uint64_t count = 0;
					std::size_t   stackCount = 0;
					bool          equipped = false;
					for (auto stack = item.stackData; stack; stack = stack->nextStack)
					{
						count += stack->GetCount();
						++stackCount;
						equipped = equipped || stack->IsEquipped();
					}
					if (count == 0)
						continue;

					snapshots.push_back(ItemSnapshot{
						.object = item.object,
						.count = count,
						.stackCount = stackCount,
						.equipped = equipped,
					});
					if (snapshots.size() > request.limit)
					{
						complete = false;
						break;
					}
				}
			}
			for (const auto& snapshot : snapshots)
			{
				json value = SerializeForm(snapshot.object);
				value["count"] = snapshot.count;
				value["stackCount"] = snapshot.stackCount;
				value["equipped"] = snapshot.equipped;
				value["value"] = RE::TESValueForm::GetFormValue(snapshot.object, nullptr);
				const auto weight = RE::TESWeightForm::GetFormWeight(snapshot.object, nullptr);
				value["weight"] = std::isfinite(weight) ? json(weight) : json(nullptr);
				items.Observe(std::move(value));
			}
			if (complete)
				items.MarkComplete();
			auto out = items.Build("items");
			out["owner"] = std::move(ownerIdentity);
			out["inventoryAvailable"] = true;
			return out;
		});
	}

	json ReadQuests(const Request& a_request)
	{
		return MainThread::RunAndWait([request = a_request]() -> json {
			RequireStableWorld("quests");
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				throw ToolError(503, "inspect quests is unavailable until a player is loaded");

			struct QuestInstance
			{
				RE::TESQuest* quest = nullptr;
				std::uint32_t instanceId = 0;
				json          objectives = json::array();
			};

			std::map<std::pair<RE::TESFormID, std::uint32_t>, QuestInstance> instances;
			for (const auto& entry : player->objectives)
			{
				const auto* objective = entry.objective;
				auto*       quest = objective ? objective->ownerQuest : nullptr;
				const auto  state = entry.enstanceState.get();
				if (!quest || !IsJournalObjective(state))
					continue;

				const auto key = std::pair{ quest->GetFormID(), entry.instanceID };
				auto&      instance = instances[key];
				instance.quest = quest;
				instance.instanceId = entry.instanceID;
				instance.objectives.push_back(json{
					{ "index", objective->index },
					{ "text", ExpandObjective(*objective, *quest, entry.instanceID) },
					{ "state", ObjectiveState(state) },
				});
			}

			ResultWindow quests(request.limit);
			for (auto& [key, instance] : instances)
			{
				(void)key;
				auto* quest = instance.quest;
				if (!quest)
					continue;

				json value = SerializeForm(quest);
				value["stage"] = quest->currentStage;
				value["instanceId"] = instance.instanceId;
				value["type"] = static_cast<std::int32_t>(quest->data.questType);
				value["priority"] = quest->data.priority;
				value["enabled"] = HasQuestFlag(*quest, RE::QuestFlag::kEnabled);
				value["active"] = quest->GetActive() && quest->currentInstanceID == instance.instanceId;
				value["completed"] = HasQuestFlag(*quest, RE::QuestFlag::kCompleted);
				value["failed"] = HasQuestFlag(*quest, RE::QuestFlag::kFailed);
				value["objectives"] = std::move(instance.objectives);
				quests.Observe(std::move(value));
			}
			quests.MarkComplete();
			return quests.Build("quests");
		});
	}

	json ReadEffects(const Request& a_request)
	{
		return MainThread::RunAndWait([request = a_request]() -> json {
			RequireStableWorld("effects");
			auto* actor = ResolveActorTarget(
				request.formId, RE::PlayerCharacter::GetSingleton(), "effects");
			ResultWindow                                       effects(request.limit);
			bool                                               complete = true;
			std::vector<RE::BSTSmartPointer<RE::ActiveEffect>> snapshots;
			if (auto* target = actor->GetMagicTarget(); target)
			{
				if (auto* list = target->GetActiveEffectList(); list)
				{
					for (const auto& effectPointer : list->data)
					{
						if (!effectPointer)
							continue;
						snapshots.push_back(effectPointer);
						if (snapshots.size() > request.limit)
						{
							complete = false;
							break;
						}
					}
				}
			}
			for (const auto& effectPointer : snapshots)
			{
				auto* effect = effectPointer.get();
				json  value{
					{ "spell", SerializeForm(effect->spell) },
					{ "effect", SerializeForm(effect->effect ? effect->effect->effectSetting : nullptr) },
					{ "source", SerializeForm(effect->source) },
					{ "magnitude", std::isfinite(effect->magnitude) ? json(effect->magnitude) : json(nullptr) },
					{ "duration", std::isfinite(effect->duration) ? json(effect->duration) : json(nullptr) },
					{ "elapsed", std::isfinite(effect->elapsedSeconds) ? json(effect->elapsedSeconds) : json(nullptr) },
					{ "inactive", effect->flags.any(RE::ActiveEffect::Flags::kInactive) },
					{ "dispelled", effect->flags.any(RE::ActiveEffect::Flags::kDispelled) },
					{ "wornOff", effect->flags.any(RE::ActiveEffect::Flags::kWornOff) },
				};
				const auto caster = effect->caster.get();
				value["caster"] = SerializeForm(caster.get());
				effects.Observe(std::move(value));
			}
			if (complete)
				effects.MarkComplete();
			auto out = effects.Build("activeEffects");
			out["target"] = SerializeForm(actor);
			return out;
		});
	}

	json ReadReferences(const Request& a_request)
	{
		return MainThread::RunAndWait([request = a_request]() -> json {
			if (!request.formId.empty())
			{
				if (!Lifecycle::GetSnapshot().gameDataReady)
					throw ToolError(503, "inspect refs: game data is not ready");
				auto* form = ResolveForm(request.formId);
				if (!form)
					throw ToolError(404, std::format("inspect refs: form '{}' not found", request.formId));
				auto* reference = form->As<RE::TESObjectREFR>();
				if (reference)
					RequireStableWorld("refs");
				return json{
					{ "source", "formId" },
					{ "count", 1 },
					{ "countExact", true },
					{ "returned", 1 },
					{ "truncated", false },
					{ "refs", json::array({ reference ? SerializeReference(reference) : SerializeForm(form) }) },
				};
			}

			if (request.selected)
			{
				RequireStableWorld("refs");
				const auto handle = RE::Console::GetCurrentPickREFR();
				const auto reference = handle.get();
				return json{
					{ "source", "selected" },
					{ "count", reference ? 1 : 0 },
					{ "countExact", true },
					{ "returned", reference ? 1 : 0 },
					{ "truncated", false },
					{ "refs", reference ? json::array({ SerializeReference(reference.get()) }) : json::array() },
				};
			}

			RequireStableWorld("refs");
			auto* world = RE::TES::GetSingleton();
			if (!world)
				throw ToolError(503, "inspect refs is unavailable without a loaded world");
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (request.radius > 0.0 && !player)
				throw ToolError(503, "inspect refs radius requires a loaded player");

			ResultWindow refs(request.limit);
			bool         complete = true;
			auto         visit = [&](RE::TESObjectREFR* a_reference) {
				if (!a_reference || a_reference->GetFormID() == 0)
					return RE::BSContainer::ForEachResult::kContinue;
				if (!request.formType.empty() &&
					!MatchesType(a_reference, request.formType) &&
					!MatchesType(a_reference->GetObjectReference(), request.formType))
					return RE::BSContainer::ForEachResult::kContinue;
				if (!refs.Observe(SerializeReference(a_reference)))
				{
					complete = false;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			};
			if (request.radius > 0.0)
				world->ForEachReferenceInRange(player, static_cast<float>(request.radius), visit);
			else
				world->ForEachReference(visit);
			if (complete)
				refs.MarkComplete();
			auto out = refs.Build("refs");
			out["source"] = request.radius > 0.0 ? "radius" : "loadedGrid";
			return out;
		});
	}

	json ReadRegistrants(const Request&)
	{
		json consumers = json::array();
		for (const auto& consumer : HostApi::Consumers())
			consumers.push_back(json{
				{ "name", consumer.name },
				{ "atEpoch", consumer.atEpoch },
				{ "atFrame", consumer.atFrame },
			});

		json registrations = json::array();
		for (const auto& registration : HostApi::Registrations())
			registrations.push_back(json{
				{ "kind", registration.kind },
				{ "name", registration.name },
				{ "atEpoch", registration.atEpoch },
				{ "atFrame", registration.atFrame },
				{ "replaced", registration.replaced },
			});

		json capabilities = json::object();
		for (const std::string_view base : { "capture", "inspect", "menu" })
		{
			json keys = json::array();
			for (const auto& key : ToolExtensions::Keys(base))
				keys.push_back(key);
			capabilities[base] = std::move(keys);
		}
		return json{
			{ "consumers", std::move(consumers) },
			{ "registrations", std::move(registrations) },
			{ "capabilities", std::move(capabilities) },
		};
	}
}

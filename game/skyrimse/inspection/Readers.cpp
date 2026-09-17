#include "Readers.h"

#include "Form.h"
#include "MainThread.h"
#include "ToolExtensions.h"
#include "game/skyrimse/Lifecycle.h"
#include "xse/HostApi.h"

#include <cmath>

namespace dvb::skyrimse::inspection
{
	namespace
	{
		using Request = tools::inspection::Request;
		using ResultWindow = tools::inspection::ResultWindow;

		void RequireStableWorld(std::string_view a_kind)
		{
			const auto lifecycle = Lifecycle::GetSnapshot();
			if (!lifecycle.gameDataReady)
				throw ToolError(
					503, std::format("inspect {}: game data is not ready", a_kind));
			if (!lifecycle.gameLoaded || lifecycle.inMainMenu)
				throw ToolError(
					503, std::format("inspect {}: unavailable without a loaded world", a_kind));
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				throw ToolError(
					503, std::format("inspect {}: unavailable while a load is in progress", a_kind));
			if (lifecycle.postLoadSucceeded == false)
				throw ToolError(
					503, std::format("inspect {}: the last load failed", a_kind));
		}

		std::string_view SkyrimFormTypeFilter(std::string_view a_filter)
		{
			if (a_filter == "scroll")
				return "scrl";
			if (a_filter == "soulgem")
				return "slgm";
			return a_filter;
		}

		bool MatchesType(const RE::TESForm* a_form, std::string_view a_filter)
		{
			if (!a_form)
				return false;
			return tools::inspection::MatchesFormType(
				RE::FormTypeToString(a_form->GetFormType()),
				SkyrimFormTypeFilter(a_filter));
		}

		RE::TESObjectREFR* ResolveReferenceTarget(
			std::string_view   a_identifier,
			RE::TESObjectREFR* a_default,
			std::string_view   a_kind)
		{
			if (a_identifier.empty())
			{
				if (!a_default)
					throw ToolError(
						503, std::format("inspect {}: player unavailable", a_kind));
				return a_default;
			}

			auto* form = ResolveForm(a_identifier);
			if (!form)
				throw ToolError(
					404, std::format("inspect {}: form '{}' not found", a_kind, a_identifier));
			auto* reference = form->As<RE::TESObjectREFR>();
			if (!reference)
				throw ToolError(
					422,
					std::format(
						"inspect {}: form '{}' is not a placed reference",
						a_kind, a_identifier));
			return reference;
		}

		RE::Actor* ResolveActorTarget(
			std::string_view a_identifier,
			RE::Actor*       a_default,
			std::string_view a_kind)
		{
			auto* reference =
				ResolveReferenceTarget(a_identifier, a_default, a_kind);
			auto* actor = reference->As<RE::Actor>();
			if (!actor)
				throw ToolError(
					422,
					std::format(
						"inspect {}: form '{}' is not an actor",
						a_kind, a_identifier));
			return actor;
		}

		json Number(float a_value)
		{
			return std::isfinite(a_value) ? json(a_value) : json(nullptr);
		}

		json ActorValue(const RE::Actor& a_actor, RE::ActorValue a_value)
		{
			const auto owner = a_actor.AsActorValueOwner();
			if (!owner)
				return nullptr;
			return json{
				{ "current", Number(owner->GetActorValue(a_value)) },
				{ "base", Number(owner->GetBaseActorValue(a_value)) },
				{ "permanent", Number(owner->GetPermanentActorValue(a_value)) },
			};
		}

		json SerializeReference(RE::TESObjectREFR* a_reference)
		{
			if (!a_reference)
				return nullptr;

			json out = SerializeForm(a_reference);
			out["base"] = SerializeForm(a_reference->GetBaseObject());
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
					{ "health", ActorValue(*actor, RE::ActorValue::kHealth) },
					{ "playerTeammate", actor->IsPlayerTeammate() },
				};
				if (const auto player = RE::PlayerCharacter::GetSingleton();
					player && actor != player)
					actorState["hostileToPlayer"] =
						actor->IsHostileToActor(player);
				else
					actorState["hostileToPlayer"] = nullptr;
				out["actor"] = std::move(actorState);
			}
			return out;
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
	}

	json ReadVm(const Request&)
	{
		return MainThread::RunAndWait([]() -> json {
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm)
				return json{ { "available", false } };

			std::size_t loadedTypes = 0;
			std::size_t attachedScripts = 0;
			std::size_t arrays = 0;
			std::size_t runningStacks = 0;
			std::uint32_t frozenStacks = 0;
			{
				RE::BSSpinLockGuard lock(vm->typeInfoLock);
				loadedTypes = vm->objectTypeMap.size();
			}
			{
				RE::BSSpinLockGuard lock(vm->attachedScriptsLock);
				attachedScripts = vm->attachedScripts.size();
			}
			{
				RE::BSSpinLockGuard lock(vm->arraysLock);
				arrays = vm->arrays.size();
			}
			{
				RE::BSSpinLockGuard lock(vm->runningStacksLock);
				runningStacks = vm->allRunningStacks.size();
			}
			{
				RE::BSSpinLockGuard lock(vm->frozenStacksLock);
				frozenStacks = vm->frozenStacksCount;
			}
			return json{
				{ "available", true },
				{ "initialized", vm->initialized },
				{ "loadedTypes", loadedTypes },
				{ "attachedScripts", attachedScripts },
				{ "arrays", arrays },
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
			auto inventory = owner->GetInventory();

			ResultWindow items(request.limit);
			for (auto& [object, entry] : inventory)
			{
				if (!object || entry.first <= 0 || !MatchesType(object, request.formType))
					continue;

				json value = SerializeForm(object);
				value["count"] = entry.first;
				value["value"] = object->GetGoldValue();
				value["weight"] = Number(object->GetWeight());
				value["equipped"] = entry.second ? entry.second->IsWorn() : false;
				if (!items.Observe(std::move(value)))
					break;
			}
			if (!items.Truncated())
				items.MarkComplete();
			auto out = items.Build("items");
			out["owner"] = SerializeForm(owner);
			out["inventoryAvailable"] = true;
			return out;
		});
	}

	json ReadQuests(const Request& a_request)
	{
		return MainThread::RunAndWait([request = a_request]() -> json {
			RequireStableWorld("quests");
			auto* data = RE::TESDataHandler::GetSingleton();
			if (!data)
				throw ToolError(503, "TESDataHandler unavailable");

			ResultWindow quests(request.limit);
			for (auto* quest : data->GetFormArray<RE::TESQuest>())
			{
				if (!quest || !(quest->IsRunning() || quest->IsCompleted()) ||
					quest->GetType() == RE::QUEST_DATA::Type::kNone)
					continue;

				json objectives = json::array();
				for (auto* objective : quest->objectives)
				{
					if (!objective ||
						objective->state.get() == RE::QUEST_OBJECTIVE_STATE::kDormant)
						continue;
					objectives.push_back(json{
						{ "index", objective->index },
						{ "text",
							objective->displayText.c_str() ?
								json(std::string(objective->displayText.c_str())) :
								json(std::string{}) },
						{ "state", ObjectiveState(objective->state.get()) },
					});
				}
				if (quest->GetCurrentStageID() == 0 && objectives.empty() &&
					!quest->IsCompleted())
					continue;

				json value = SerializeForm(quest);
				value["stage"] = quest->GetCurrentStageID();
				value["type"] = static_cast<std::int32_t>(quest->GetType());
				value["active"] = quest->IsActive();
				value["completed"] = quest->IsCompleted();
				value["objectives"] = std::move(objectives);
				if (!quests.Observe(std::move(value)))
					break;
			}
			if (!quests.Truncated())
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
			ResultWindow effects(request.limit);

			auto append = [&](RE::ActiveEffect* a_effect) {
				if (!a_effect)
					return true;
				json value{
					{ "spell", SerializeForm(a_effect->spell) },
					{ "effect", SerializeForm(a_effect->GetBaseObject()) },
					{ "source", SerializeForm(a_effect->source) },
					{ "caster", SerializeForm(a_effect->GetCasterActor().get()) },
					{ "magnitude", Number(a_effect->magnitude) },
					{ "duration", Number(a_effect->duration) },
					{ "elapsed", Number(a_effect->elapsedSeconds) },
					{ "inactive", a_effect->flags.any(RE::ActiveEffect::Flag::kInactive) },
					{ "dispelled", a_effect->flags.any(RE::ActiveEffect::Flag::kDispelled) },
				};
				return effects.Observe(std::move(value));
			};

			if (auto* target = actor->AsMagicTarget())
			{
				if (REL::Module::IsVR())
				{
#ifdef ENABLE_SKYRIM_VR
					target->VisitActiveEffects([&](RE::ActiveEffect* a_effect) {
						return append(a_effect) ?
						           RE::BSContainer::ForEachResult::kContinue :
						           RE::BSContainer::ForEachResult::kStop;
					});
#else
					throw ToolError(
						503,
						"inspect effects: this build lacks the Skyrim VR active-effect visitor");
#endif
				}
				else if (auto* list = target->GetActiveEffectList())
				{
					for (auto* effect : *list)
					{
						if (!append(effect))
							break;
					}
				}
			}
			if (!effects.Truncated())
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
					throw ToolError(
						404,
						std::format(
							"inspect refs: form '{}' not found", request.formId));
				auto* reference = form->As<RE::TESObjectREFR>();
				if (reference)
					RequireStableWorld("refs");
				return json{
					{ "source", "formId" },
					{ "count", 1 },
					{ "countExact", true },
					{ "returned", 1 },
					{ "truncated", false },
					{ "refs",
						json::array({
							reference ? SerializeReference(reference) : SerializeForm(form),
						}) },
				};
			}

			if (request.selected)
			{
				RequireStableWorld("refs");
				const auto reference = RE::Console::GetSelectedRef();
				return json{
					{ "source", "selected" },
					{ "count", reference ? 1 : 0 },
					{ "countExact", true },
					{ "returned", reference ? 1 : 0 },
					{ "truncated", false },
					{ "refs",
						reference ?
							json::array({ SerializeReference(reference.get()) }) :
							json::array() },
				};
			}

			RequireStableWorld("refs");
			auto* world = RE::TES::GetSingleton();
			if (!world)
				throw ToolError(
					503, "inspect refs is unavailable without a loaded world");
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (request.radius > 0.0 && !player)
				throw ToolError(
					503, "inspect refs radius requires a loaded player");

			ResultWindow refs(request.limit);
			auto visit = [&](RE::TESObjectREFR* a_reference) {
				if (!a_reference || a_reference->GetFormID() == 0)
					return RE::BSContainer::ForEachResult::kContinue;
				if (!request.formType.empty() &&
					!MatchesType(a_reference, request.formType) &&
					!MatchesType(a_reference->GetBaseObject(), request.formType))
					return RE::BSContainer::ForEachResult::kContinue;
				return refs.Observe(SerializeReference(a_reference)) ?
				           RE::BSContainer::ForEachResult::kContinue :
				           RE::BSContainer::ForEachResult::kStop;
			};
			if (request.radius > 0.0)
				world->ForEachReferenceInRange(
					player, static_cast<float>(request.radius), visit);
			else
				world->ForEachReference(visit);
			if (!refs.Truncated())
				refs.MarkComplete();
			auto out = refs.Build("refs");
			out["source"] =
				request.radius > 0.0 ? "radius" : "loadedGrid";
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

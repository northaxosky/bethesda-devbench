#include "game/fallout4/recording/RecordingBackend.h"

#include "GameState.h"
#include "MainThread.h"
#include "game/fallout4/Lifecycle.h"
#include "game/fallout4/inspection/Form.h"

#include <numbers>

namespace dvb::fallout4
{
	namespace
	{
		constexpr double kRadiansToDegrees =
			180.0 / std::numbers::pi_v<double>;

		std::string FormIdentifier(const json& a_identity)
		{
			if (!a_identity.is_object())
				return {};
			if (const auto editorId = a_identity.find("editorId");
				editorId != a_identity.end() && editorId->is_string() &&
				!editorId->get_ref<const std::string&>().empty())
				return editorId->get<std::string>();
			if (const auto formId = a_identity.find("formIdHex");
				formId != a_identity.end() && formId->is_string())
				return formId->get<std::string>();
			return {};
		}

		json Snapshot()
		{
			return MainThread::RunAndWait([]() -> json {
				const auto lifecycle = Lifecycle::GetSnapshot();
				auto*      player = RE::PlayerCharacter::GetSingleton();
				if (!lifecycle.gameDataReady || lifecycle.inMainMenu ||
					lifecycle.inLoadingMenu || lifecycle.loadInProgress ||
					lifecycle.postLoadSucceeded == false || !player ||
					!player->Get3D())
					return json{
						{ "playerLoaded", false },
						{ "frame", game::CurrentFrame() },
					};

				auto* cell = player->GetParentCell();
				auto* worldspace =
					cell && !cell->IsInterior() ? cell->worldSpace : nullptr;
				const auto position = player->GetPosition();
				json scene{
					{ "interior", cell && cell->IsInterior() },
					{ "cell", inspection::SerializeForm(cell) },
					{ "worldspace", inspection::SerializeForm(worldspace) },
					{ "cellFormID", cell ? cell->GetFormID() : 0 },
					{ "worldspaceFormID",
						worldspace ? worldspace->GetFormID() : 0 },
					{ "anchor",
						json{
							{ "x", position.x },
							{ "y", position.y },
							{ "z", position.z },
							{ "yaw",
								static_cast<double>(player->data.angle.z) *
									kRadiansToDegrees },
							{ "pitch",
								static_cast<double>(player->data.angle.x) *
									kRadiansToDegrees },
						} },
				};
				if (const auto calendar = RE::Calendar::GetSingleton();
					calendar && calendar->gameHour)
					scene["gameHour"] = calendar->gameHour->GetValue();
				if (const auto sky = RE::Sky::GetSingleton();
					sky && sky->currentWeather)
					scene["weather"] =
						inspection::SerializeForm(sky->currentWeather);

				return json{
					{ "playerLoaded", true },
					{ "frame", game::CurrentFrame() },
					{ "pose",
						json::array({
							position.x,
							position.y,
							position.z,
							static_cast<double>(player->data.angle.z) *
								kRadiansToDegrees,
							static_cast<double>(player->data.angle.x) *
								kRadiansToDegrees,
						}) },
					{ "scene", std::move(scene) },
				};
			});
		}

		tools::recording::RecordingTransitionReceipt Transition(
			const json& a_target)
		{
			if (!a_target.value("interior", false))
				throw ToolError(
					501,
					"exterior recording transitions must use the permission-checked console COW seam");
			const auto lifecycle = Lifecycle::GetSnapshot();
			if (!lifecycle.gameDataReady)
				throw ToolError(
					503,
					"recorded transitions are unavailable until gameDataReady");
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				throw ToolError(
					409, "a game load is already in progress");
			const auto actionCursor = lifecycle.eventCursor;

			const auto result = MainThread::RunAndWait([a_target]() -> json {
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player)
					throw ToolError(
						503,
						"recorded transition requires PlayerCharacter");
				using CenterOnCell_t =
					bool (*)(RE::PlayerCharacter*, const char*, RE::TESObjectCELL*);
				static REL::Relocation<CenterOnCell_t> centerOnCell{
					REL::ID(2232904)
				};
				const auto configuredEditorCell =
					a_target.value("editorCell", std::string{});
				RE::TESObjectCELL* cell = nullptr;
				const char*       name = nullptr;
				std::string       identifier;
				if (!configuredEditorCell.empty())
				{
					name = configuredEditorCell.c_str();
					identifier = configuredEditorCell;
				}
				else
				{
					const auto identity =
						a_target.value("cell", json(nullptr));
					identifier = FormIdentifier(identity);
					if (identifier.empty())
						throw ToolError(
							422,
							"recorded interior transition has no stable CELL identity");
					auto* form = inspection::ResolveForm(identifier);
					if (!form || !form->Is(RE::ENUM_FORM_ID::kCELL))
						throw ToolError(
							404,
							std::format(
								"recorded CELL '{}' is unavailable",
								identifier));
					cell = form->As<RE::TESObjectCELL>();
					if (!cell)
						throw ToolError(
							409,
							"recorded transition identity is not a TESObjectCELL");
				}

				if (!centerOnCell(player, name, cell))
					throw ToolError(
						409,
						"Fallout 4 rejected the typed CELL transition");
				return json{
					{ "mode",
						configuredEditorCell.empty() ?
							"cell" :
							"configuredEditorCell" },
					{ "target", identifier },
				};
			});
			return {
				.queued = true,
				.actionCursor = actionCursor,
				.mode = result.value("mode", std::string("unknown")),
			};
		}
	}

	tools::recording::RecordingBackend MakeRecordingBackend()
	{
		return {
			.snapshot = &Snapshot,
			.transition = &Transition,
		};
	}
}

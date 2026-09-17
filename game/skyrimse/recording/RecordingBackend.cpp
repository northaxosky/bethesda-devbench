#include "RecordingBackend.h"

#include "EventBus.h"
#include "GameState.h"
#include "MainThread.h"
#include "RecordingActivity.h"
#include "game/skyrimse/Lifecycle.h"
#include "game/skyrimse/inspection/Form.h"
#include "game/skyrimse/vr/VRInput.h"

#include <RE/Skyrim.h>

#include <mutex>
#include <numbers>

namespace dvb::skyrimse::recording
{
	namespace
	{
		constexpr double kRadiansToDegrees =
			180.0 / std::numbers::pi_v<double>;

		std::mutex g_activityMutex;
		EventBus*  g_activityEvents = nullptr;

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

		std::string InputDeviceName(RE::INPUT_DEVICE a_device)
		{
			switch (static_cast<int>(a_device))
			{
				case 0:
					return "keyboard";
				case 1:
					return "mouse";
				case 2:
					return "gamepad";
				case 3:
					return REL::Module::IsVR() ? "vivePrimary" : "virtualKeyboard";
				case 4:
					return "viveSecondary";
				case 5:
					return "oculusPrimary";
				case 6:
					return "oculusSecondary";
				case 7:
					return "wmrPrimary";
				case 8:
					return "wmrSecondary";
				case 9:
					return "vrVirtualKeyboard";
				default:
					return static_cast<int>(a_device) < 0 ? "none" : "unknown";
			}
		}

		std::string InputEventTypeName(int a_type)
		{
			switch (a_type)
			{
				case 0:
					return "button";
				case 1:
					return "mouseMove";
				case 2:
					return "char";
				case 3:
					return "thumbstick";
				case 4:
					return "deviceConnect";
				case 5:
					return "kinect";
				case 6:
					return REL::Module::IsVR() ? "vrTouchpadPosition" : "sixaxis";
				case 7:
					return REL::Module::IsVR() ? "vrTouchpadSwipe" : "motionGesture";
				case 8:
					return "amiibo";
				default:
					return "unknown";
			}
		}

		json SerializeInputEvent(const RE::InputEvent& a_event)
		{
			const int type = static_cast<int>(a_event.GetEventType());
			json      out{
				{ "kind", "input" },
				{ "eventType", InputEventTypeName(type) },
				{ "eventTypeCode", type },
				{ "device", InputDeviceName(a_event.GetDevice()) },
				{ "deviceCode", static_cast<int>(a_event.GetDevice()) },
			};
			if (const auto id = a_event.AsIDEvent())
			{
				out["idCode"] = id->GetIDCode();
				if (const char* user = id->userEvent.c_str(); user && *user)
					out["userEvent"] = user;
			}
			if (const auto button = a_event.AsButtonEvent())
			{
				out["value"] = button->Value();
				out["heldSeconds"] = button->HeldDuration();
				out["state"] = button->IsDown() ? "down" :
				               button->IsHeld() ? "held" :
				               button->IsUp()   ? "up" :
				                                  "changed";
				if (REL::Module::IsVR())
					if (const auto wand = button->AsVRWandEvent())
						out["wandIndex"] = wand->unkVR28;
			}
			else if (const auto mouse = a_event.AsMouseMoveEvent())
			{
				out["x"] = mouse->mouseInputX;
				out["y"] = mouse->mouseInputY;
			}
			else if (const auto character = a_event.AsCharEvent())
			{
				out["keyCode"] = character->keyCode;
			}
			else if (const auto stick = a_event.AsThumbstickEvent())
			{
				out["x"] = stick->xValue;
				out["y"] = stick->yValue;
			}
			else if (type == 4)
			{
				out["connected"] =
					static_cast<const RE::DeviceConnectEvent&>(a_event).connected;
			}
			else if (REL::Module::IsVR() && type == 6)
			{
				const auto& touch =
					static_cast<const RE::VrWandTouchpadPositionEvent&>(a_event);
				out["wandIndex"] = touch.unkVR28;
				out["raw"] = json::array({ touch.unk30, touch.unk38, touch.unk40 });
			}
			else if (REL::Module::IsVR() && type == 7)
			{
				const auto& swipe =
					static_cast<const RE::VrWandTouchpadSwipeEvent&>(a_event);
				out["wandIndex"] = swipe.unkVR28;
				out["raw"] = json::array({ swipe.unk30, swipe.unk38 });
			}
			return out;
		}

		json Snapshot()
		{
			return MainThread::RunAndWait([]() -> json {
				const auto lifecycle = Lifecycle::GetSnapshot();
				auto*      player = RE::PlayerCharacter::GetSingleton();
				if (!lifecycle.gameDataReady || lifecycle.inMainMenu ||
					lifecycle.inLoadingMenu || lifecycle.loadInProgress ||
					lifecycle.postLoadSucceeded == false || !player || !player->Get3D())
					return json{
						{ "playerLoaded", false },
						{ "frame", game::CurrentFrame() },
						{ "tracking", vrinput::ObserveVRTrackedSet() },
					};

				auto*      cell = player->GetParentCell();
				auto*      worldspace = player->GetWorldspace();
				const auto position = player->GetPosition();
				json       scene{
					{ "interior", cell && cell->IsInteriorCell() },
					{ "cell", inspection::SerializeForm(cell) },
					{ "worldspace", inspection::SerializeForm(worldspace) },
					{ "cellFormID", cell ? cell->GetFormID() : 0 },
					{ "worldspaceFormID", worldspace ? worldspace->GetFormID() : 0 },
					{ "anchor",
						json{
							{ "x", position.x },
							{ "y", position.y },
							{ "z", position.z },
							{ "yaw", static_cast<double>(player->GetAngleZ()) * kRadiansToDegrees },
							{ "pitch", static_cast<double>(player->GetAngleX()) * kRadiansToDegrees },
						} },
				};
				if (const auto calendar = RE::Calendar::GetSingleton())
					scene["gameHour"] = calendar->GetHour();
				if (const auto sky = RE::Sky::GetSingleton(); sky && sky->currentWeather)
					scene["weather"] = inspection::SerializeForm(sky->currentWeather);

				json out{
					{ "playerLoaded", true },
					{ "frame", game::CurrentFrame() },
					{ "pose",
						json::array({
							position.x,
							position.y,
							position.z,
							static_cast<double>(player->GetAngleZ()) * kRadiansToDegrees,
							static_cast<double>(player->GetAngleX()) * kRadiansToDegrees,
						}) },
					{ "scene", std::move(scene) },
					{ "tracking", vrinput::ObserveVRTrackedSet() },
				};
				if (const auto camera = RE::PlayerCamera::GetSingleton();
					camera && camera->cameraRoot)
				{
					const auto&  transform = camera->cameraRoot->world;
					RE::NiPoint3 euler{};
					out["camera"] = json{
						{ "x", transform.translate.x },
						{ "y", transform.translate.y },
						{ "z", transform.translate.z },
					};
					if (transform.rotate.ToEulerAnglesXYZ(euler))
					{
						out["camera"]["pitch"] = euler.x;
						out["camera"]["yaw"] = euler.z;
					}
					if (camera->IsInFirstPerson())
						out["camera"]["pov"] = "first";
					else if (camera->IsInThirdPerson())
						out["camera"]["pov"] = "third";
					else if (camera->currentState &&
							 camera->currentState->id == RE::CameraState::kAutoVanity)
						out["camera"]["pov"] = "vanity";
				}
				return out;
			});
		}

		tools::recording::RecordingTransitionReceipt Transition(const json& a_target)
		{
			if (!a_target.value("interior", false))
				throw ToolError(
					501,
					"exterior recording transitions must use the permission-checked console COW seam");
			const auto lifecycle = Lifecycle::GetSnapshot();
			if (!lifecycle.gameDataReady)
				throw ToolError(
					503, "recorded transitions are unavailable until gameDataReady");
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				throw ToolError(409, "a game load is already in progress");
			const auto actionCursor = lifecycle.eventCursor;

			const auto result = MainThread::RunAndWait([a_target]() -> json {
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player)
					throw ToolError(503, "recorded transition requires PlayerCharacter");
				const auto configuredEditorCell =
					a_target.value("editorCell", std::string{});
				std::string identifier;
				bool        applied = false;
				if (!configuredEditorCell.empty())
				{
					identifier = configuredEditorCell;
					applied = player->CenterOnCell(configuredEditorCell.c_str());
				}
				else
				{
					const auto identity = a_target.value("cell", json(nullptr));
					identifier = FormIdentifier(identity);
					if (identifier.empty())
						throw ToolError(
							422, "recorded interior transition has no stable CELL identity");
					const auto form = inspection::ResolveForm(identifier);
					const auto cell = form ? form->As<RE::TESObjectCELL>() : nullptr;
					if (!cell)
						throw ToolError(404,
							std::format("recorded CELL '{}' is unavailable", identifier));
					applied = player->CenterOnCell(cell);
				}
				if (!applied)
					throw ToolError(409, "Skyrim rejected the typed CELL transition");
				return json{
					{ "mode", configuredEditorCell.empty() ? "cell" : "configuredEditorCell" },
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
		tools::recording::RecordingBackend backend{
			.gameId = REL::Module::IsVR() ? "vr" : "se",
			.gameName = REL::Module::IsVR() ? "Skyrim VR" : "Skyrim Special Edition",
			.compatibleGameIds = { "se", "vr", "skyrim", "skyrimse",
				"skyrim special edition", "skyrim vr" },
			.compatibleRuntimeVariants =
				REL::Module::IsVR() ? std::vector<std::string>{ "vr" } :
									  std::vector<std::string>{ "se", "ae" },
			.recordedOnVR = REL::Module::IsVR(),
			.snapshot = &Snapshot,
			.transition = &Transition,
			.activityCaptureContract = &ActivityCaptureContract,
			.buildTrackedInputReplay = &BuildVRTrackedSetReplay,
		};
		if (REL::Module::IsVR())
			backend.trackingSnapshot = &vrinput::ObserveVRTrackedSet;
		return backend;
	}

	void AttachActivityEvents(EventBus& a_events)
	{
		const std::lock_guard lock{ g_activityMutex };
		g_activityEvents = std::addressof(a_events);
	}

	void DetachActivityEvents() noexcept
	{
		const std::lock_guard lock{ g_activityMutex };
		g_activityEvents = nullptr;
	}

	void NoteInputEvents(RE::InputEvent* const* a_events)
	{
		if (!a_events)
			return;
		EventBus* events = nullptr;
		{
			const std::lock_guard lock{ g_activityMutex };
			events = g_activityEvents;
		}
		if (!events)
			return;
		for (const auto* event = *a_events; event; event = event->next)
			events->Publish("input.activity", SerializeInputEvent(*event));
	}
}

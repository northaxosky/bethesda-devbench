#include "InputBackend.h"

#include "GameState.h"
#include "MainThread.h"
#include "game/skyrimse/recording/RecordingBackend.h"
#include "game/skyrimse/vr/VRInput.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <RE/B/BSInputEventQueue.h>
#include <RE/B/BSWin32KeyboardDevice.h>

namespace dvb::skyrimse
{
	namespace
	{
		using tools::input::ButtonCommand;
		using tools::input::DispatchResult;
		using tools::input::DispatchStatus;
		using tools::input::DispatchSubmission;

		DispatchResult ApplyButton(const ButtonCommand& a_command)
		{
			if (!a_command.gate || !a_command.gate->TryBegin())
				return { DispatchStatus::kCancelled, -1, false,
					"keyboard command was cancelled before native dispatch" };

			struct FinishGate
			{
				std::shared_ptr<tools::input::DispatchGate> gate;
				~FinishGate() { gate->Finish(); }
			} finish{ a_command.gate };

			const auto devices = RE::BSInputDeviceManager::GetSingleton();
			const auto keyboard = devices ? devices->GetKeyboard() : nullptr;
			if (!keyboard)
				return { DispatchStatus::kUnavailable, -1, false,
					"Skyrim keyboard device is unavailable" };

			const bool physicalDown = keyboard->IsPressed(a_command.key.scancode);
			if (a_command.down && physicalDown)
				return { DispatchStatus::kPhysicalConflict, game::CurrentFrame(), true,
					std::format(
						"physical key '{}' is already down; synthetic down was not injected",
						a_command.key.name) };
			if (!a_command.down && physicalDown)
				return { DispatchStatus::kApplied, game::CurrentFrame(), true,
					"synthetic lease released without injecting up because the physical key is down" };

			auto* queue = RE::BSInputEventQueue::GetSingleton();
			if (!queue)
				return { DispatchStatus::kUnavailable, -1, false,
					"Skyrim BSInputEventQueue is unavailable" };
			if (queue->buttonEventCount >= RE::BSInputEventQueue::MAX_BUTTON_EVENTS)
				return { DispatchStatus::kQueueFull, game::CurrentFrame(), false,
					"Skyrim keyboard event pool is full; native dispatch was not appended" };

			queue->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, 0, a_command.key.scancode,
				a_command.down ? 1.0F : 0.0F, a_command.down ? 0.0F : a_command.heldSecs);
			return { DispatchStatus::kApplied, game::CurrentFrame(), false, {} };
		}

		DispatchSubmission DispatchButton(ButtonCommand a_command)
		{
			const auto completion = a_command.complete;
			try
			{
				const auto value = MainThread::RunAndWait(
					[command = std::move(a_command)]() mutable -> json {
						DispatchResult result;
						try
						{
							result = ApplyButton(command);
						}
						catch (const std::exception& a_exception)
						{
							result = { DispatchStatus::kFailed, -1, false,
								std::format("native keyboard dispatch failed: {}", a_exception.what()) };
						}
						catch (...)
						{
							result = { DispatchStatus::kFailed, -1, false,
								"native keyboard dispatch failed with an unknown exception" };
						}
						if (command.complete)
							command.complete(result);
						return json{
							{ "status", static_cast<int>(result.status) },
							{ "frame", result.frame },
							{ "physicalKeyDown", result.physicalKeyDown },
							{ "message", result.message },
						};
					},
					std::chrono::milliseconds(1000));

				return { false,
					DispatchResult{
						static_cast<DispatchStatus>(
							value.value("status", static_cast<int>(DispatchStatus::kFailed))),
						value.value("frame", -1),
						value.value("physicalKeyDown", false),
						value.value("message", std::string{}),
					} };
			}
			catch (const ToolError& a_error)
			{
				if (a_error.code == 504)
					return { true, std::nullopt };
				DispatchResult result{
					a_error.code == 503 ? DispatchStatus::kUnavailable : DispatchStatus::kFailed,
					-1, false, a_error.what()
				};
				if (completion)
					completion(result);
				return { false, result };
			}
			catch (const std::exception& a_exception)
			{
				DispatchResult result{ DispatchStatus::kFailed, -1, false,
					std::format("could not queue native keyboard dispatch: {}", a_exception.what()) };
				if (completion)
					completion(result);
				return { false, result };
			}
		}
	}

	tools::input::InputBackend MakeInputBackend()
	{
		return tools::input::InputBackend{
			.gameName = "Skyrim Special Edition",
			.extenderName = "SKSE",
			.injection = "Skyrim.BSInputEventQueue",
			.available = true,
			.dispatch = &DispatchButton,
			.nativeDevices = { "vrTrackedSet" },
			.nativeCapabilities = [] { return json{ { "vrTrackedSet", vrinput::VRInputCapabilities() } }; },
			.handleNative = &vrinput::HandleVRInput,
			.attachActivity = [](EventBus& a_events) {
				vrinput::AttachVRInputEvents(a_events);
				recording::AttachActivityEvents(a_events); },
			.setNativeReady = &vrinput::SetVRInputReady,
			.releaseNativeForLifecycle = [](std::string a_reason) { vrinput::ReleaseVRInputForLifecycle(a_reason.c_str()); },
			.shutdownNative = [] {
				recording::DetachActivityEvents();
				vrinput::ShutdownVRInput(); },
		};
	}

}

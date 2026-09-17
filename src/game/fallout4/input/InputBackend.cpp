#include "InputBackend.h"

#include "GameState.h"
#include "MainThread.h"

#include <atomic>
#include <cstddef>

namespace dvb::fallout4
{
	namespace
	{
		using tools::input::ButtonCommand;
		using tools::input::DispatchResult;
		using tools::input::DispatchStatus;
		using tools::input::DispatchSubmission;

		constexpr REL::Version kSupportedRuntime{ 1, 11, 240, 0 };
		constexpr std::size_t  kKeyboardCurrentStateOffset = 0x70;
		constexpr std::size_t  kQueueGenerationOffset = 0x1490;

		// AE 1.11.240, Fallout4.exe SHA-256
		// FDCEF37AC1230AF6D0B0050EB2142B139EF3A867B37B9211FB6EDFCC646072F8:
		// - ID 4801677 / RVA 0x3273AE0 is a BSInputEventQueue* global. The queue
		//   constructor (ID 2268399 / RVA 0x1672A30) writes rcx into that slot.
		// - ID 2268423 / RVA 0x1673C00 is AddButtonEvent(queue, device, deviceID,
		//   idCode, value, heldSecs). It takes a 0x40 ButtonEvent from the queue's
		//   preconstructed 30-entry pool, fills +08/+0C/+30/+38/+3C, then calls
		//   ID 2268431 / RVA 0x1674610.
		// - the append routine links through InputEvent::next at +18 and increments
		//   the queue generation at +0x1490 while holding the queue lock.
		// These source-confidence bindings intentionally call the producer instead of
		// allocating an event, appending blindly, or invoking receivers manually.
		using AddButtonEvent_t =
			void(void*, std::int32_t, std::int32_t, std::uint32_t, float, float);

		bool RuntimeSupported() noexcept
		{
			static const bool supported = []() noexcept {
				try
				{
					const auto runtime = REX::FModule::GetExecutingModule().GetFileVersion();
					if (!REX::FModule::IsRuntimeAE() || runtime != kSupportedRuntime)
					{
						logs::error(
							"devbench: native keyboard input disabled for runtime {}; only "
							"Fallout 4 AE 1.11.240 is supported",
							runtime.string("."));
						return false;
					}
					return true;
				}
				catch (const std::exception& a_exception)
				{
					logs::error(
						"devbench: native keyboard runtime validation failed: {}", a_exception.what());
					return false;
				}
				catch (...)
				{
					logs::error("{}", "devbench: native keyboard runtime validation failed");
					return false;
				}
			}();
			return supported;
		}

		DispatchResult ApplyButton(const ButtonCommand& a_command)
		{
			if (!a_command.gate || !a_command.gate->TryBegin())
			{
				return {
					DispatchStatus::kCancelled, -1, false,
					"keyboard command was cancelled before native dispatch"
				};
			}

			struct FinishGate
			{
				std::shared_ptr<tools::input::DispatchGate> gate;
				~FinishGate() { gate->Finish(); }
			} finish{ a_command.gate };

			if (!RuntimeSupported())
			{
				return {
					DispatchStatus::kUnavailable, -1, false,
					"native keyboard input is unavailable outside Fallout 4 AE 1.11.240"
				};
			}

			const auto devices = RE::BSInputDeviceManager::GetSingleton();
			if (!devices)
			{
				return {
					DispatchStatus::kUnavailable, -1, false,
					"Fallout 4 BSInputDeviceManager is unavailable"
				};
			}
			const auto keyboard =
				devices->devices[std::to_underlying(RE::INPUT_DEVICE::kKeyboard)];
			if (!keyboard)
			{
				return {
					DispatchStatus::kUnavailable, -1, false,
					"Fallout 4 keyboard device is unavailable"
				};
			}

			// Keyboard poll ID 2268454 / RVA 0x1674FE0 compares the 256-byte current
			// state at BSInputDevice+0x70 with the previous state at +0x170. Its helper
			// (ID 2268476 / RVA 0x1675FE0) emits value 1/0 and accumulated held seconds.
			// Read that byte only; never overwrite hardware state to manufacture input.
			const auto physicalState = reinterpret_cast<const std::uint8_t*>(keyboard) +
			                           kKeyboardCurrentStateOffset + a_command.key.scancode;
			const bool physicalDown = *physicalState != 0;
			if (a_command.down && physicalDown)
			{
				return {
					DispatchStatus::kPhysicalConflict, game::CurrentFrame(), true,
					std::format(
						"physical key '{}' is already down; synthetic down was not injected",
						a_command.key.name)
				};
			}
			if (!a_command.down && physicalDown)
			{
				// A physical press took over a synthetic hold. Do not inject an up that
				// would contradict the hardware state; the physical release will emit it.
				return {
					DispatchStatus::kApplied, game::CurrentFrame(), true,
					"synthetic lease released without injecting up because the physical key is down"
				};
			}

			static REL::Relocation<void**>           queueGlobal{ REL::ID(4801677) };
			static REL::Relocation<AddButtonEvent_t> addButton{ REL::ID(2268423) };
			auto*                                    queue = *queueGlobal;
			if (!queue)
			{
				return {
					DispatchStatus::kUnavailable, -1, false,
					"Fallout 4 BSInputEventQueue is unavailable"
				};
			}

			auto& generation = *reinterpret_cast<std::uint32_t*>(
				static_cast<std::byte*>(queue) + kQueueGenerationOffset);
			const auto before = std::atomic_ref<std::uint32_t>(generation).load(std::memory_order_acquire);
			addButton(queue, static_cast<std::int32_t>(keyboard->deviceType.get()), keyboard->deviceID,
				a_command.key.scancode, a_command.down ? 1.0F : 0.0F,
				a_command.down ? 0.0F : a_command.heldSecs);
			const auto after = std::atomic_ref<std::uint32_t>(generation).load(std::memory_order_acquire);

			// AddButtonEvent has no return value. Its proven success acknowledgement is
			// the append routine's wrapping 32-bit generation increment. A full pool with
			// no recyclable queued ButtonEvent returns without changing the generation.
			if (after == before)
			{
				return {
					DispatchStatus::kQueueFull, game::CurrentFrame(), false,
					"Fallout 4 keyboard event pool is full; native dispatch was not appended"
				};
			}
			if (after != before + 1U)
			{
				// Do not retry an uncertain mutation: another producer changed the
				// generation during the observation window, so our event may already be
				// present. Preserve/retire the lease as if applied and surface uncertainty.
				return {
					DispatchStatus::kUnknown, game::CurrentFrame(), false,
					"Fallout 4 keyboard queue changed concurrently; native dispatch outcome "
					"is uncertain and will not be retried"
				};
			}

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

				return {
					false,
					DispatchResult{
						static_cast<DispatchStatus>(value.value("status",
							static_cast<int>(DispatchStatus::kFailed))),
						value.value("frame", -1),
						value.value("physicalKeyDown", false),
						value.value("message", std::string{}),
					},
				};
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

	tools::input::KeyboardBackend MakeInputBackend()
	{
		return tools::input::KeyboardBackend{
			.injection = "Fallout4.BSInputEventQueue",
			.available = true,
			.dispatch = &DispatchButton,
		};
	}
}

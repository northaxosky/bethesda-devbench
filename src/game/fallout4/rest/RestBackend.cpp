#include "RestBackend.h"

#include "game/fallout4/inspection/Form.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

namespace dvb::fallout4
{
	namespace
	{
		using namespace std::chrono_literals;
		using tools::rest::RestCommand;
		using tools::rest::RestKind;
		using tools::rest::RestResult;
		using tools::rest::RestResultStatus;
		using tools::rest::RestSubmission;

		constexpr REL::Version     kSupportedRuntime{ 1, 11, 240, 0 };
		constexpr auto             kPollInterval = 25ms;
		constexpr auto             kMenuOpenTimeout = 5s;
		constexpr std::uint32_t    kCanSleepFurnitureFlag = 0x80000000u;
		constexpr std::string_view kMenuName = "SleepWaitMenu";

		bool RuntimeSupported() noexcept
		{
			static const bool supported = []() noexcept {
				try
				{
					const auto runtime = REX::FModule::GetExecutingModule().GetFileVersion();
					if (!REX::FModule::IsRuntimeAE() || runtime != kSupportedRuntime)
					{
						logs::error(
							"devbench: native wait/sleep disabled for runtime {}; only Fallout "
							"4 AE 1.11.240 is supported",
							runtime.string("."));
						return false;
					}
					return true;
				}
				catch (const std::exception& a_exception)
				{
					logs::error(
						"devbench: native wait/sleep runtime validation failed: {}",
						a_exception.what());
					return false;
				}
				catch (...)
				{
					logs::error(
						"{}", "devbench: native wait/sleep runtime validation failed");
					return false;
				}
			}();
			return supported;
		}

		std::optional<double> CurrentGameHours()
		{
			const auto calendar = RE::Calendar::GetSingleton();
			if (!calendar || !calendar->gameDaysPassed)
				return std::nullopt;
			return static_cast<double>(calendar->gameDaysPassed->value) * 24.0;
		}

		Scaleform::Ptr<RE::IMenu> GetSleepWaitMenu()
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui)
				return nullptr;
			return ui->GetMenu(RE::BSFixedString(kMenuName));
		}

		void CloseSleepWaitMenu(RE::IMenu& a_menu)
		{
			const RE::BSFixedString cancel{ "Cancel" };
			(void)a_menu.OnButtonEventRelease(cancel);
		}

		std::optional<int> IntegerValue(const Scaleform::GFx::Value& a_value)
		{
			if (a_value.IsInt())
				return a_value.GetInt();
			if (a_value.IsUInt())
			{
				const auto value = a_value.GetUInt();
				if (value <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
					return static_cast<int>(value);
			}
			if (a_value.IsNumber())
			{
				const auto value = a_value.GetNumber();
				if (std::isfinite(value) && std::trunc(value) == value &&
					value >= static_cast<double>(std::numeric_limits<int>::min()) &&
					value <= static_cast<double>(std::numeric_limits<int>::max()))
					return static_cast<int>(value);
			}
			return std::nullopt;
		}

		struct NativeOperation
		{
			explicit NativeOperation(RestCommand a_command) :
				command(std::move(a_command))
			{}

			RestCommand                           command;
			bool                                  accepted = false;
			std::chrono::steady_clock::time_point menuDeadline;
			std::optional<double>                 startGameHours;
		};

		class NativeBackendState final :
			public std::enable_shared_from_this<NativeBackendState>
		{
		public:
			NativeBackendState() :
				worker_([this](std::stop_token a_stop) { PollLoop(a_stop); })
			{}

			~NativeBackendState()
			{
				Shutdown();
			}

			RestSubmission Dispatch(RestCommand a_command)
			{
				auto* task = F4SE::GetTaskInterface();
				if (!task)
					return { false,
						RestResult{
							RestResultStatus::kUnavailable, false, std::nullopt,
							"F4SE TaskInterface is unavailable" } };

				auto operation = std::make_shared<NativeOperation>(std::move(a_command));
				{
					const std::lock_guard lock{ mutex_ };
					if (stopping_)
						return { false,
							RestResult{
								RestResultStatus::kUnavailable, false, std::nullopt,
								"native wait/sleep backend is shutting down" } };
					if (active_)
						return { false,
							RestResult{
								RestResultStatus::kFailed, false, std::nullopt,
								"native wait/sleep backend already has an active operation" } };
					active_ = operation;
				}

				const std::weak_ptr weak = shared_from_this();
				try
				{
					task->AddTask([weak, operation] {
						if (const auto state = weak.lock())
							state->BeginOnMain(operation);
					});
				}
				catch (const std::exception& a_exception)
				{
					Complete(operation,
						{ RestResultStatus::kUnavailable, false, std::nullopt,
							std::format(
								"could not queue native wait/sleep: {}", a_exception.what()) });
					return { true, std::nullopt };
				}
				return { true, std::nullopt };
			}

			void Shutdown() noexcept
			{
				std::shared_ptr<NativeOperation> operation;
				{
					const std::lock_guard lock{ mutex_ };
					if (stopping_)
						return;
					stopping_ = true;
					operation = active_;
				}
				if (operation && operation->command.gate)
					(void)operation->command.gate->Cancel();
				cv_.notify_all();
				worker_.request_stop();
				if (worker_.joinable())
					worker_.join();
			}

		private:
			std::optional<RestResult> BeginNative(NativeOperation& a_operation)
			{
				if (!a_operation.command.gate ||
					!a_operation.command.gate->TryBegin())
					return RestResult{
						RestResultStatus::kCancelled, false, std::nullopt,
						"wait/sleep dispatch was cancelled before native execution"
					};

				struct FinishGate
				{
					std::shared_ptr<tools::rest::DispatchGate> gate;
					~FinishGate() { gate->Finish(); }
				} finish{ a_operation.command.gate };

				if (!a_operation.command.markDispatching ||
					!a_operation.command.markDispatching())
					return RestResult{
						RestResultStatus::kCancelled, false, std::nullopt,
						"shared game-operation reservation was no longer dispatchable"
					};

				if (!RuntimeSupported())
					return RestResult{
						RestResultStatus::kUnavailable, false, std::nullopt,
						"native wait/sleep is unavailable outside Fallout 4 AE 1.11.240"
					};

				const auto player = RE::PlayerCharacter::GetSingleton();
				if (!player)
					return RestResult{
						RestResultStatus::kUnavailable, false, std::nullopt,
						"PlayerCharacter is unavailable"
					};
				const auto existingMenu = GetSleepWaitMenu();
				if (player->sleepSeconds != 0 ||
					(existingMenu && existingMenu->OnStack()))
					return RestResult{
						RestResultStatus::kRefused, false, std::nullopt,
						"Fallout 4 already has an active sleep/wait menu or operation"
					};

				RE::TESObjectREFR* target = nullptr;
				if (a_operation.command.kind == RestKind::kSleep)
				{
					auto* form =
						inspection::ResolveForm(a_operation.command.target.value_or(""));
					if (!form)
						return RestResult{
							RestResultStatus::kRefused, false, std::nullopt,
							std::format(
								"sleep target '{}' was not found",
								a_operation.command.target.value_or(""))
						};
					target = form->As<RE::TESObjectREFR>();
					if (!target)
						return RestResult{
							RestResultStatus::kRefused, false, std::nullopt,
							"sleep target is not a placed reference"
						};
					auto* const furniture =
						target->GetObjectReference() ?
							target->GetObjectReference()->As<RE::TESFurniture>() :
							nullptr;
					// TESFurniture::CanSleepOn at AE RVA 0x44C210 is exactly this record
					// flag test. Keeping the check local avoids inventing a CommonLib API.
					if (!furniture ||
						(furniture->furnFlags & kCanSleepFurnitureFlag) == 0)
						return RestResult{
							RestResultStatus::kRefused, false, std::nullopt,
							"sleep target is not a bed-enabled furniture reference"
						};
				}

				RE::ObjectRefHandle handle;
				if (target)
				{
					// The real furniture activation path first calls
					// PlayerCharacter::CanPassTime with the bed reference, then opens
					// SleepWaitMenu. Repeat that bed-specific gate for sleep; wait relies
					// on ToggleOpenSleepWaitMenu's own null-reference gate, matching
					// SitWaitMenu exactly and avoiding a duplicate gate call.
					using CanPassTime_t =
						bool(RE::PlayerCharacter*, RE::TESObjectREFR*, bool, bool);
					static REL::Relocation<CanPassTime_t> canPassTime{
						REL::ID(2232915)
					};
					if (!canPassTime(player, target, true, false))
						return RestResult{
							RestResultStatus::kRefused, false, std::nullopt,
							"Fallout 4 refused the bed-specific sleep gate; see the in-game "
							"HUD message"
						};

					handle = target->GetHandle();
					if (!handle)
						return RestResult{
							RestResultStatus::kRefused, false, std::nullopt,
							"sleep target does not have a valid reference handle"
						};
				}

				// AE 1.11.240 source proof: ID 2249519 / RVA 0x107CC60 is
				// SleepWaitMenu::ToggleOpenSleepWaitMenu(ObjectRefHandle*, bool sleep).
				// It calls PlayerCharacter::CanPassTime(player, nullptr, true, false)
				// before queueing the menu and its {handle, sleep} payload.
				using ToggleSleepWait_t = void(RE::ObjectRefHandle*, bool);
				static REL::Relocation<ToggleSleepWait_t> toggle{ REL::ID(2249519) };
				a_operation.startGameHours = CurrentGameHours();
				a_operation.menuDeadline =
					std::chrono::steady_clock::now() + kMenuOpenTimeout;
				toggle(std::addressof(handle),
					a_operation.command.kind == RestKind::kSleep);
				if (!a_operation.command.markQueued ||
					!a_operation.command.markQueued())
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"shared game-operation tracker rejected the queued wait/sleep state"
					};
				return std::nullopt;
			}

			std::optional<RestResult> PollNative(NativeOperation& a_operation)
			{
				const auto player = RE::PlayerCharacter::GetSingleton();
				if (!player)
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"PlayerCharacter became unavailable during wait/sleep"
					};

				auto menu = GetSleepWaitMenu();
				if (!a_operation.accepted)
				{
					if (!menu || !menu->OnStack() || !menu->menuObj.IsObject())
					{
						if (std::chrono::steady_clock::now() >=
							a_operation.menuDeadline)
							return RestResult{
								RestResultStatus::kRefused, false, std::nullopt,
								"Fallout 4 did not open SleepWaitMenu; the native gate or "
								"current UI state refused the request"
							};
						return std::nullopt;
					}

					// The shipped 1.11.240 SleepWaitMenu.swf initializes iHours=8 and
					// iMaxHours=24. Its hours setter stores iHours, while ModifyHours
					// delegates to ChangeHour, which clamps iHours to 0..iMaxHours.
					// Set the proven native-read property, then run the loaded SWF's own
					// zero-delta clamp so a replacer can impose a stricter maximum.
					const Scaleform::GFx::Value requested{ a_operation.command.hours };
					if (!menu->menuObj.SetMember("hours", requested))
					{
						CloseSleepWaitMenu(*menu);
						return RestResult{
							RestResultStatus::kFailed, false, std::nullopt,
							"SleepWaitMenu rejected its proven 'hours' property"
						};
					}
					const Scaleform::GFx::Value zero{ std::int32_t{ 0 } };
					if (!menu->menuObj.Invoke("ModifyHours", nullptr, &zero, 1))
					{
						CloseSleepWaitMenu(*menu);
						return RestResult{
							RestResultStatus::kFailed, false, std::nullopt,
							"SleepWaitMenu rejected its proven ModifyHours clamp"
						};
					}

					Scaleform::GFx::Value clamped;
					if (!menu->menuObj.GetMember("hours", std::addressof(clamped)))
					{
						CloseSleepWaitMenu(*menu);
						return RestResult{
							RestResultStatus::kFailed, false, std::nullopt,
							"SleepWaitMenu did not expose its proven 'hours' property"
						};
					}
					const auto hours = IntegerValue(clamped);
					if (!hours || *hours != a_operation.command.hours)
					{
						CloseSleepWaitMenu(*menu);
						return RestResult{
							RestResultStatus::kRefused, false, std::nullopt,
							std::format(
								"loaded SleepWaitMenu restricted requested hours {} to {}",
								a_operation.command.hours,
								hours ? std::to_string(*hours) : std::string("a non-integer value"))
						};
					}

					// ID 2249522 / AE RVA 0x107CE80 is SleepWaitMenu::OnAccept.
					// The mapped AS callback named "Accept" contains only a call to this
					// native callback. OnAccept reads menuObj.hours, starts the native
					// sleep/wait path, marks the menu waiting, and resets its fade timer.
					using OnAccept_t = void(RE::IMenu*);
					static REL::Relocation<OnAccept_t> onAccept{ REL::ID(2249522) };
					onAccept(menu.get());
					if (player->sleepSeconds == 0)
					{
						CloseSleepWaitMenu(*menu);
						return RestResult{
							RestResultStatus::kFailed, false, std::nullopt,
							"SleepWaitMenu acceptance did not start native time passage"
						};
					}
					a_operation.accepted = true;
					return std::nullopt;
				}

				if (player->sleepSeconds != 0 || (menu && menu->OnStack()))
					return std::nullopt;

				std::optional<double> elapsed;
				if (a_operation.startGameHours)
				{
					if (const auto current = CurrentGameHours())
						elapsed = std::max(0.0, *current - *a_operation.startGameHours);
				}
				const bool interrupted =
					elapsed && *elapsed + 0.01 < a_operation.command.hours;
				return RestResult{
					RestResultStatus::kCompleted,
					interrupted,
					elapsed,
					interrupted ?
						"native wait/sleep finished before all requested hours elapsed" :
						"native wait/sleep finish path completed",
					json{
						{ "nativeCompleted", true },
						{ "autosavePolicy", "native" },
						{ "runtimeValidated", RuntimeSupported() },
					}
				};
			}

			void BeginOnMain(const std::shared_ptr<NativeOperation>& a_operation)
			{
				std::optional<RestResult> result;
				try
				{
					result = BeginNative(*a_operation);
				}
				catch (const std::exception& a_exception)
				{
					result = RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						std::format("native wait/sleep start failed: {}", a_exception.what())
					};
				}
				catch (...)
				{
					result = RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"native wait/sleep start failed with an unknown exception"
					};
				}
				if (result)
					Complete(a_operation, std::move(*result));
				else
					RequestPoll(a_operation);
			}

			void PollOnMain(const std::shared_ptr<NativeOperation>& a_operation)
			{
				std::optional<RestResult> result;
				try
				{
					result = PollNative(*a_operation);
				}
				catch (const std::exception& a_exception)
				{
					result = RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						std::format("native wait/sleep polling failed: {}", a_exception.what())
					};
				}
				catch (...)
				{
					result = RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"native wait/sleep polling failed with an unknown exception"
					};
				}

				{
					const std::lock_guard lock{ mutex_ };
					pollQueued_ = false;
				}
				if (result)
					Complete(a_operation, std::move(*result));
				else
					RequestPoll(a_operation);
			}

			void RequestPoll(const std::shared_ptr<NativeOperation>& a_operation)
			{
				{
					const std::lock_guard lock{ mutex_ };
					if (stopping_ || active_ != a_operation)
						return;
					pollRequested_ = true;
					nextPoll_ = std::chrono::steady_clock::now() + kPollInterval;
				}
				cv_.notify_all();
			}

			void Complete(
				const std::shared_ptr<NativeOperation>& a_operation, RestResult a_result)
			{
				std::function<void(RestResult)> callback;
				{
					const std::lock_guard lock{ mutex_ };
					if (active_ != a_operation)
						return;
					callback = a_operation->command.complete;
					active_.reset();
					pollRequested_ = false;
					pollQueued_ = false;
				}
				cv_.notify_all();
				if (callback)
					callback(std::move(a_result));
			}

			void PollLoop(std::stop_token a_stop)
			{
				std::unique_lock lock{ mutex_ };
				while (!a_stop.stop_requested())
				{
					cv_.wait(lock, [&] {
						return a_stop.stop_requested() || stopping_ ||
						       (active_ && pollRequested_ && !pollQueued_);
					});
					if (a_stop.stop_requested() || stopping_)
						return;
					const auto due = nextPoll_;
					if (cv_.wait_until(lock, due, [&] {
							return a_stop.stop_requested() || stopping_ ||
						           !active_ || !pollRequested_ || pollQueued_;
						}))
						continue;
					if (!active_ || !pollRequested_ || pollQueued_)
						continue;

					auto operation = active_;
					pollRequested_ = false;
					pollQueued_ = true;
					lock.unlock();

					auto* task = F4SE::GetTaskInterface();
					if (!task)
					{
						Complete(operation,
							{ RestResultStatus::kUnavailable, false, std::nullopt,
								"F4SE TaskInterface became unavailable during wait/sleep" });
					}
					else
					{
						const std::weak_ptr weak = shared_from_this();
						try
						{
							task->AddTask([weak, operation] {
								if (const auto state = weak.lock())
									state->PollOnMain(operation);
							});
						}
						catch (const std::exception& a_exception)
						{
							Complete(operation,
								{ RestResultStatus::kUnavailable, false, std::nullopt,
									std::format(
										"could not queue wait/sleep poll: {}",
										a_exception.what()) });
						}
					}
					lock.lock();
				}
			}

			std::mutex                            mutex_;
			std::condition_variable               cv_;
			std::shared_ptr<NativeOperation>      active_;
			std::chrono::steady_clock::time_point nextPoll_;
			bool                                  pollRequested_ = false;
			bool                                  pollQueued_ = false;
			bool                                  stopping_ = false;
			std::jthread                          worker_;
		};
	}

	tools::rest::RestBackend MakeRestBackend()
	{
		auto state = std::make_shared<NativeBackendState>();
		return {
			.implementation = "Fallout4.SleepWaitMenu",
			.available = RuntimeSupported(),
			.runtimeValidated = RuntimeSupported(),
			.dispatch = [state](RestCommand a_command) { return state->Dispatch(std::move(a_command)); },
			.shutdown = [state] { state->Shutdown(); },
		};
	}
}

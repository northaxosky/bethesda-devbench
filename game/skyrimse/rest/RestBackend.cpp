#include "RestBackend.h"

#include "game/skyrimse/Lifecycle.h"

#include <RE/C/Calendar.h>
#include <RE/G/GFxValue.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/SleepWaitMenu.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESFurniture.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/U/UI.h>
#include <SKSE/API.h>
#include <SKSE/Interfaces.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace dvb::skyrimse
{
	namespace
	{
		using namespace std::chrono_literals;
		using tools::rest::RestCommand;
		using tools::rest::RestKind;
		using tools::rest::RestResult;
		using tools::rest::RestResultStatus;
		using tools::rest::RestSubmission;

		constexpr auto kPollInterval = 25ms;
		constexpr auto kMenuOpenTimeout = 5s;
		constexpr auto kNativeStartTimeout = 30s;

		struct ReadinessFailure
		{
			RestResultStatus status = RestResultStatus::kRefused;
			std::string      message;
		};

		std::optional<ReadinessFailure> CheckReadiness()
		{
			const auto lifecycle = Lifecycle::GetSnapshot();
			if (!lifecycle.gameDataReady)
				return ReadinessFailure{
					.status = RestResultStatus::kUnavailable,
					.message =
						"native wait/sleep requires initialized game data",
				};
			if (lifecycle.inMainMenu || !lifecycle.gameLoaded)
				return ReadinessFailure{
					.message =
						"native wait/sleep requires a loaded game outside the main menu",
				};
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				return ReadinessFailure{
					.message =
						"native wait/sleep is unavailable while a load is in progress",
				};
			return std::nullopt;
		}

		std::optional<double> CurrentGameHours()
		{
			const auto* calendar = RE::Calendar::GetSingleton();
			if (!calendar)
				return std::nullopt;
			const double hours = calendar->GetHoursPassed();
			return std::isfinite(hours) ? std::optional(hours) : std::nullopt;
		}

		RE::GPtr<RE::SleepWaitMenu> GetSleepWaitMenu()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui ? ui->GetMenu<RE::SleepWaitMenu>() : nullptr;
		}

		bool CancelSleepWaitMenu(RE::SleepWaitMenu& a_menu)
		{
			auto& root = a_menu.GetRuntimeData().root;
			return (root.IsObject() || root.IsDisplayObject()) &&
			       root.Invoke("onCancelPress");
		}

		std::optional<int> IntegerValue(const RE::GFxValue& a_value)
		{
			if (!a_value.IsNumber())
				return std::nullopt;
			const double value = a_value.GetNumber();
			if (!std::isfinite(value) || std::trunc(value) != value ||
				value < static_cast<double>(std::numeric_limits<int>::min()) ||
				value > static_cast<double>(std::numeric_limits<int>::max()))
				return std::nullopt;
			return static_cast<int>(value);
		}

		RE::TESForm* ResolveHex(std::string_view a_text)
		{
			if (a_text.empty())
				return nullptr;
			std::uint64_t value = 0;
			const auto [end, error] =
				std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
			if (error != std::errc{} || end != a_text.data() + a_text.size() ||
				value > std::numeric_limits<RE::FormID>::max())
				return nullptr;
			return RE::TESForm::LookupByID(static_cast<RE::FormID>(value));
		}

		RE::TESForm* ResolveForm(std::string_view a_identifier)
		{
			if (a_identifier.empty())
				return nullptr;
			if (a_identifier.starts_with("0x") || a_identifier.starts_with("0X"))
				return ResolveHex(a_identifier.substr(2));
			if (auto* form = RE::TESForm::LookupByEditorID(a_identifier))
				return form;
			return ResolveHex(a_identifier);
		}

		struct NativeOperation
		{
			explicit NativeOperation(RestCommand a_command) :
				command(std::move(a_command))
			{}

			RestCommand                           command;
			bool                                  menuAccepted = false;
			bool                                  nativeStarted = false;
			std::chrono::steady_clock::time_point menuDeadline;
			std::chrono::steady_clock::time_point nativeStartDeadline;
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
				auto* tasks = SKSE::GetTaskInterface();
				if (!tasks)
					return {
						false,
						RestResult{
							RestResultStatus::kUnavailable, false, std::nullopt,
							"SKSE TaskInterface is unavailable" },
					};

				auto operation =
					std::make_shared<NativeOperation>(std::move(a_command));
				{
					const std::lock_guard lock{ mutex_ };
					if (stopping_)
						return {
							false,
							RestResult{
								RestResultStatus::kUnavailable, false,
								std::nullopt,
								"native wait/sleep backend is shutting down" },
						};
					if (active_)
						return {
							false,
							RestResult{
								RestResultStatus::kFailed, false, std::nullopt,
								"native wait/sleep backend already has an active operation" },
						};
					active_ = operation;
				}

				const std::weak_ptr weak = shared_from_this();
				try
				{
					tasks->AddTask([weak, operation] {
						if (const auto state = weak.lock())
							state->BeginOnMain(operation);
					});
				}
				catch (const std::exception& a_exception)
				{
					Complete(operation,
						{ RestResultStatus::kUnavailable, false, std::nullopt,
							std::format(
								"could not queue native wait/sleep: {}",
								a_exception.what()) });
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

				if (const auto failure = CheckReadiness())
					return RestResult{
						failure->status, false, std::nullopt, failure->message
					};

				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* ui = RE::UI::GetSingleton();
				if (!player || !ui)
					return RestResult{
						RestResultStatus::kUnavailable, false, std::nullopt,
						"Skyrim player or UI is unavailable"
					};
				if (player->GetPlayerRuntimeData().sleepSeconds != 0 ||
					ui->IsMenuOpen(RE::SleepWaitMenu::MENU_NAME))
					return RestResult{
						RestResultStatus::kRefused, false, std::nullopt,
						"Skyrim already has an active sleep/wait menu or operation"
					};

				RE::TESObjectREFR* target = nullptr;
				if (a_operation.command.kind == RestKind::kSleep)
				{
					auto* form =
						ResolveForm(a_operation.command.target.value_or(""));
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
					auto* furniture = target->GetBaseObject() ?
					                      target->GetBaseObject()->As<RE::TESFurniture>() :
					                      nullptr;
					if (!furniture ||
						!furniture->furnFlags.all(
							RE::TESFurniture::ActiveMarker::kCanSleep))
						return RestResult{
							RestResultStatus::kRefused, false, std::nullopt,
							"sleep target is not bed-enabled furniture"
						};
				}

				if (!player->CanSleepWait(target))
					return RestResult{
						RestResultStatus::kRefused, false, std::nullopt,
						a_operation.command.kind == RestKind::kSleep ?
							"Skyrim refused the bed-specific sleep gate; "
							"see the in-game HUD message" :
							"Skyrim refused the native wait gate; "
							"see the in-game HUD message"
					};

				a_operation.menuDeadline =
					std::chrono::steady_clock::now() + kMenuOpenTimeout;
				if (!a_operation.command.markQueued ||
					!a_operation.command.markQueued())
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"shared game-operation tracker rejected the queued wait/sleep state"
					};
				RE::SleepWaitMenu::ToggleOpenMenu(
					a_operation.command.kind == RestKind::kSleep);
				return std::nullopt;
			}

			std::optional<double> ElapsedGameHours(
				const NativeOperation& a_operation) const
			{
				if (!a_operation.startGameHours)
					return std::nullopt;
				if (const auto current = CurrentGameHours())
					return std::max(0.0, *current - *a_operation.startGameHours);
				return std::nullopt;
			}

			RestResult CompletedResult(const NativeOperation& a_operation) const
			{
				const auto elapsed = ElapsedGameHours(a_operation);
				const bool interrupted =
					elapsed && *elapsed + 0.01 < a_operation.command.hours;
				return RestResult{
					RestResultStatus::kCompleted,
					interrupted,
					elapsed,
					interrupted ?
						"native wait/sleep closed before all requested hours elapsed" :
						"native wait/sleep menu and time-passage state both closed",
					json{
						{ "nativeCompleted", true },
						{ "autosavePolicy", "native" },
						{ "runtimeValidated", false },
						{ "menuPath", "Sleep/Wait Menu.onOKPress -> GameDelegate.OK" },
					}
				};
			}

			std::optional<RestResult> ConfigureAndAccept(
				NativeOperation& a_operation, RE::SleepWaitMenu& a_menu)
			{
				auto& root = a_menu.GetRuntimeData().root;
				if (!root.IsObject() && !root.IsDisplayObject())
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"loaded Sleep/Wait Menu did not expose its root ActionScript object"
					};

				RE::GFxValue slider;
				if (!root.GetMember("HoursSlider", &slider) ||
					(!slider.IsObject() && !slider.IsDisplayObject()))
				{
					(void)CancelSleepWaitMenu(a_menu);
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"loaded Sleep/Wait Menu did not expose HoursSlider"
					};
				}
				if (!slider.SetMember(
						"value", RE::GFxValue(a_operation.command.hours)) ||
					!root.Invoke("sliderChange"))
				{
					(void)CancelSleepWaitMenu(a_menu);
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"loaded Sleep/Wait Menu rejected its native slider update"
					};
				}

				RE::GFxValue clamped;
				if (!root.Invoke("getSliderValue", &clamped))
				{
					(void)CancelSleepWaitMenu(a_menu);
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"loaded Sleep/Wait Menu did not expose getSliderValue"
					};
				}
				const auto hours = IntegerValue(clamped);
				if (!hours || *hours != a_operation.command.hours)
				{
					(void)CancelSleepWaitMenu(a_menu);
					return RestResult{
						RestResultStatus::kRefused, false, std::nullopt,
						std::format(
							"loaded Sleep/Wait Menu restricted requested hours {} to {}",
							a_operation.command.hours,
							hours ? std::to_string(*hours) :
									std::string("a non-integer value"))
					};
				}

				// Vanilla and SkyUI-compatible menus route onOKPress through the
				// loaded movie's GameDelegate "OK" callback. That callback owns the
				// native StartWaiting/StartSleeping, update, finish, and autosave
				// policy; this adapter never advances ticks or changes the calendar.
				a_operation.startGameHours = CurrentGameHours();
				if (!root.Invoke("onOKPress"))
				{
					(void)CancelSleepWaitMenu(a_menu);
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"loaded Sleep/Wait Menu rejected its onOKPress acceptance path"
					};
				}
				a_operation.menuAccepted = true;
				a_operation.nativeStartDeadline =
					std::chrono::steady_clock::now() + kNativeStartTimeout;
				const auto& runtime = a_menu.GetRuntimeData();
				a_operation.nativeStarted =
					runtime.isActive ||
					RE::PlayerCharacter::GetSingleton()
							->GetPlayerRuntimeData()
							.sleepSeconds != 0;
				return std::nullopt;
			}

			std::optional<RestResult> PollNative(NativeOperation& a_operation)
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player)
					return RestResult{
						RestResultStatus::kFailed, false, std::nullopt,
						"PlayerCharacter became unavailable during wait/sleep"
					};

				auto menu = GetSleepWaitMenu();
				if (!a_operation.menuAccepted)
				{
					if (!menu || !menu->OnStack())
					{
						if (std::chrono::steady_clock::now() >=
							a_operation.menuDeadline)
							return RestResult{
								RestResultStatus::kRefused, false, std::nullopt,
								"Skyrim did not open Sleep/Wait Menu; the native gate or "
								"current UI state refused the request"
							};
						return std::nullopt;
					}
					return ConfigureAndAccept(a_operation, *menu);
				}

				const bool menuOpen = menu && menu->OnStack();
				const bool timePassage =
					player->GetPlayerRuntimeData().sleepSeconds != 0 ||
					(menuOpen && menu->GetRuntimeData().isActive);
				a_operation.nativeStarted =
					a_operation.nativeStarted || timePassage;

				if (!a_operation.nativeStarted)
				{
					if (!menuOpen)
						return CompletedResult(a_operation);
					if (std::chrono::steady_clock::now() >=
						a_operation.nativeStartDeadline)
					{
						(void)CancelSleepWaitMenu(*menu);
						return RestResult{
							RestResultStatus::kFailed, false,
							ElapsedGameHours(a_operation),
							"Sleep/Wait Menu accepted OK but native time passage did not start"
						};
					}
					return std::nullopt;
				}

				if (timePassage || menuOpen)
					return std::nullopt;
				return CompletedResult(a_operation);
			}

			void BeginOnMain(
				const std::shared_ptr<NativeOperation>& a_operation)
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
						std::format(
							"native wait/sleep start failed: {}",
							a_exception.what())
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

			void PollOnMain(
				const std::shared_ptr<NativeOperation>& a_operation)
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
						std::format(
							"native wait/sleep polling failed: {}",
							a_exception.what())
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

			void RequestPoll(
				const std::shared_ptr<NativeOperation>& a_operation)
			{
				{
					const std::lock_guard lock{ mutex_ };
					if (stopping_ || active_ != a_operation)
						return;
					pollRequested_ = true;
					nextPoll_ =
						std::chrono::steady_clock::now() + kPollInterval;
				}
				cv_.notify_all();
			}

			void Complete(
				const std::shared_ptr<NativeOperation>& a_operation,
				RestResult                              a_result)
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

					auto* tasks = SKSE::GetTaskInterface();
					if (!tasks)
					{
						Complete(operation,
							{ RestResultStatus::kUnavailable, false, std::nullopt,
								"SKSE TaskInterface became unavailable during wait/sleep" });
					}
					else
					{
						const std::weak_ptr weak = shared_from_this();
						try
						{
							tasks->AddTask([weak, operation] {
								if (const auto state = weak.lock())
									state->PollOnMain(operation);
							});
						}
						catch (const std::exception& a_exception)
						{
							Complete(operation,
								{ RestResultStatus::kUnavailable, false,
									std::nullopt,
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
			.implementation = "Skyrim.SleepWaitMenu.GameDelegate.OK",
			.available = true,
			// Compile-time CommonLib relocation coverage is not supervised runtime
			// evidence. Keep this false until SE, AE, and VR are each exercised.
			.runtimeValidated = false,
			.dispatch =
				[state](RestCommand a_command) {
					return state->Dispatch(std::move(a_command));
				},
			.shutdown = [state] { state->Shutdown(); },
		};
	}
}

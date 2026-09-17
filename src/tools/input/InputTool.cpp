#include "InputTool.h"

#include "EventBus.h"
#include "tools/ToolPermissions.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace dvb::tools::input
{
	namespace
	{
		using namespace std::chrono;

		constexpr auto kReleaseRetryDelay = milliseconds(100);
		constexpr auto kShutdownCleanupBudget = milliseconds(1500);

		std::int64_t NowMs()
		{
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}

		void RequireObject(const json& a_args)
		{
			if (!a_args.is_object())
				throw ToolError(400, "input arguments must be an object");
		}

		int BoundedInteger(
			const json& a_object, std::string_view a_name, int a_default, int a_min, int a_max)
		{
			const auto it = a_object.find(a_name);
			if (it == a_object.end())
				return a_default;
			if (!it->is_number_integer())
				throw ToolError(400, std::format("'{}' must be an integer", a_name));
			const auto value = it->get<std::int64_t>();
			if (value < a_min || value > a_max)
				throw ToolError(
					400, std::format("'{}' must be within {}..{}", a_name, a_min, a_max));
			return static_cast<int>(value);
		}

		bool BooleanArgument(const json& a_object, std::string_view a_name, bool a_default)
		{
			const auto it = a_object.find(a_name);
			if (it == a_object.end())
				return a_default;
			if (!it->is_boolean())
				throw ToolError(400, std::format("'{}' must be a boolean", a_name));
			return it->get<bool>();
		}

		KeyboardKey ParseKey(const json& a_args)
		{
			const auto it = a_args.find("key");
			if (it == a_args.end())
				throw ToolError(
					400, "keyboard action requires 'key' (a documented name or DirectInput scancode)");

			std::optional<KeyboardKey> key;
			if (it->is_string())
				key = ResolveKeyboardKey(it->get<std::string>());
			else if (it->is_number_integer())
				key = ResolveKeyboardKey(BoundedInteger(
					a_args, "key", 0, 0, std::numeric_limits<std::uint16_t>::max()));
			else
				throw ToolError(400, "'key' must be a string name or integer DirectInput scancode");

			if (!key)
				throw ToolError(400,
					std::format("unknown/invalid keyboard key {}; use action='capabilities' for "
								"names and scan codes",
						it->dump()));
			return *key;
		}

		std::string ResolveOwner(const json& a_args, const ToolContext& a_context)
		{
			std::string owner;
			if (const auto it = a_args.find("owner"); it != a_args.end())
			{
				if (!it->is_string())
					throw ToolError(400, "'owner' must be a string");
				owner = it->get<std::string>();
			}
			else if (!a_context.clientId.empty())
			{
				owner = "mcp:" + a_context.clientId;
			}
			else
			{
				owner = "rest:anonymous";
			}
			if (owner.empty() || owner.size() > 128)
				throw ToolError(400, "'owner' must contain 1..128 characters");
			return owner;
		}

		json KeyJson(const KeyboardKey& a_key)
		{
			return json{ { "key", a_key.name }, { "scancode", a_key.scancode } };
		}

		json ContractJson()
		{
			return json{
				{ "name", "devbench.input" },
				{ "version", json{ { "major", 2 }, { "minor", 0 } } },
			};
		}

		std::string_view StatusName(DispatchStatus a_status)
		{
			switch (a_status)
			{
				case DispatchStatus::kApplied:
					return "applied";
				case DispatchStatus::kUnknown:
					return "unknown";
				case DispatchStatus::kCancelled:
					return "cancelled";
				case DispatchStatus::kPhysicalConflict:
					return "physicalConflict";
				case DispatchStatus::kQueueFull:
					return "queueFull";
				case DispatchStatus::kUnavailable:
					return "unavailable";
				case DispatchStatus::kFailed:
					return "failed";
			}
			return "failed";
		}

		bool DispatchSucceeded(const DispatchResult& a_result)
		{
			return a_result.status == DispatchStatus::kApplied ||
			       a_result.status == DispatchStatus::kUnknown;
		}

		[[noreturn]] void ThrowDispatchError(const DispatchResult& a_result)
		{
			const auto message = a_result.message.empty() ?
			                         std::format("Fallout 4 keyboard dispatch failed ({})", StatusName(a_result.status)) :
			                         a_result.message;
			switch (a_result.status)
			{
				case DispatchStatus::kPhysicalConflict:
					throw ToolError(409, message);
				case DispatchStatus::kQueueFull:
				case DispatchStatus::kUnavailable:
					throw ToolError(503, message);
				case DispatchStatus::kCancelled:
					throw ToolError(409, message);
				default:
					throw ToolError(500, message);
			}
		}
	}

	bool DispatchGate::TryBegin() noexcept
	{
		auto expected = State::kQueued;
		return state_.compare_exchange_strong(
			expected, State::kRunning, std::memory_order_acq_rel, std::memory_order_acquire);
	}

	bool DispatchGate::Cancel() noexcept
	{
		auto expected = State::kQueued;
		return state_.compare_exchange_strong(
			expected, State::kCancelled, std::memory_order_acq_rel, std::memory_order_acquire);
	}

	void DispatchGate::Finish() noexcept
	{
		auto expected = State::kRunning;
		(void)state_.compare_exchange_strong(
			expected, State::kFinished, std::memory_order_release, std::memory_order_relaxed);
	}

	struct InputService::State : std::enable_shared_from_this<InputService::State>
	{
		enum class LeasePhase
		{
			kDownPending,
			kPressed,
			kReleasePending,
			kReleaseRetry
		};

		struct ActiveLease
		{
			KeyboardLease                 lease;
			LeasePhase                    phase = LeasePhase::kDownPending;
			std::shared_ptr<DispatchGate> downGate;
			std::shared_ptr<DispatchGate> releaseGate;
			std::string                   releaseReason;
			std::int64_t                  retryAtMs = 0;
		};

		EventBus*                                      events;
		const bool                                     allowControlActions;
		KeyboardBackend                                backend;
		std::atomic<bool>                              ready{ false };
		std::mutex                                     mutex;
		std::condition_variable                        cv;
		KeyboardLeaseTable                             leases;
		std::unordered_map<std::uint16_t, ActiveLease> active;
		std::thread                                    watchdog;
		bool                                           accepting = true;
		bool                                           lifecyclePending = false;
		std::string                                    lifecycleReason;
		bool                                           workerStop = false;
		bool                                           shutdownStarted = false;

		State(EventBus& a_events, bool a_allowControlActions, KeyboardBackend a_backend) :
			events(std::addressof(a_events)),
			allowControlActions(a_allowControlActions),
			backend(std::move(a_backend))
		{}

		void Start()
		{
			const auto self = shared_from_this();
			watchdog = std::thread([self] { self->WatchdogLoop(); });
		}

		void RequireReady()
		{
			if (!backend.available)
				throw ToolError(503,
					"Fallout 4 keyboard injection is unavailable for this runtime; only AE 1.11.240 "
					"is supported");
			if (!ready.load(std::memory_order_acquire))
				throw ToolError(503,
					"keyboard input is not ready; F4SE kInputLoaded has not completed");
			const std::lock_guard lock{ mutex };
			if (!accepting)
				throw ToolError(503, "keyboard input service is shutting down");
		}

		void Publish(std::string_view a_action, const KeyboardLease& a_lease,
			std::string_view a_reason, const DispatchResult* a_result, bool a_pending)
		{
			EventBus* bus = nullptr;
			{
				const std::lock_guard lock{ mutex };
				bus = events;
			}
			if (!bus)
				return;

			json payload{
				{ "contractVersion", kKeyboardContractVersion },
				{ "device", "keyboard" },
				{ "action", a_action },
				{ "owner", a_lease.owner },
				{ "generation", a_lease.generation },
				{ "reason", a_reason },
				{ "pending", a_pending },
			};
			payload.update(KeyJson(a_lease.key));
			if (a_result)
			{
				payload["dispatch"] = StatusName(a_result->status);
				payload["frame"] = a_result->frame;
				payload["physicalKeyDown"] = a_result->physicalKeyDown;
				if (!a_result->message.empty())
					payload["message"] = a_result->message;
			}
			bus->Publish("input.keyboard", std::move(payload));
		}

		DispatchSubmission Dispatch(ButtonCommand a_command)
		{
			if (!backend.dispatch)
			{
				DispatchResult result{
					DispatchStatus::kUnavailable, -1, false,
					"Fallout 4 keyboard dispatcher is unavailable"
				};
				if (a_command.complete)
					a_command.complete(result);
				return { false, result };
			}

			const auto completed = std::make_shared<std::atomic<bool>>(false);
			auto       completion = std::move(a_command.complete);
			a_command.complete = [completed, completion = std::move(completion)](
									 DispatchResult a_result) mutable {
				if (!completed->exchange(true, std::memory_order_acq_rel) && completion)
					completion(std::move(a_result));
			};
			const auto notify = a_command.complete;

			try
			{
				auto submission = backend.dispatch(std::move(a_command));
				if (submission.result)
					notify(*submission.result);
				return submission;
			}
			catch (const std::exception& a_exception)
			{
				DispatchResult result{
					DispatchStatus::kFailed, -1, false,
					std::format("Fallout 4 keyboard dispatcher threw: {}", a_exception.what())
				};
				notify(result);
				return { false, result };
			}
			catch (...)
			{
				DispatchResult result{
					DispatchStatus::kFailed, -1, false,
					"Fallout 4 keyboard dispatcher threw an unknown exception"
				};
				notify(result);
				return { false, result };
			}
		}

		void OnDownComplete(std::uint16_t a_scancode, std::uint64_t a_generation,
			const std::shared_ptr<DispatchGate>& a_gate, DispatchResult a_result)
		{
			std::optional<KeyboardLease>  lease;
			std::shared_ptr<DispatchGate> releaseGate;
			{
				const std::lock_guard lock{ mutex };
				const auto            it = active.find(a_scancode);
				if (it == active.end() || it->second.lease.generation != a_generation ||
					it->second.downGate != a_gate)
					return;

				lease = it->second.lease;
				it->second.downGate.reset();
				if (DispatchSucceeded(a_result))
				{
					if (it->second.phase == LeasePhase::kDownPending)
						it->second.phase = LeasePhase::kPressed;
				}
				else
				{
					releaseGate = it->second.releaseGate;
					leases.RemoveExact(a_scancode, a_generation);
					active.erase(it);
				}
			}
			if (releaseGate)
				releaseGate->Cancel();
			cv.notify_all();
			if (lease)
				Publish(DispatchSucceeded(a_result) ? "down" : "downRejected", *lease,
					DispatchSucceeded(a_result) ? "request" : "nativeDispatch", &a_result, false);
		}

		void OnReleaseComplete(std::uint16_t a_scancode, std::uint64_t a_generation,
			const std::shared_ptr<DispatchGate>& a_gate, DispatchResult a_result)
		{
			std::optional<KeyboardLease> lease;
			std::string                  reason;
			{
				const std::lock_guard lock{ mutex };
				const auto            it = active.find(a_scancode);
				if (it == active.end() || it->second.lease.generation != a_generation ||
					it->second.releaseGate != a_gate)
					return;

				lease = it->second.lease;
				reason = it->second.releaseReason;
				it->second.releaseGate.reset();
				if (DispatchSucceeded(a_result))
				{
					leases.RemoveExact(a_scancode, a_generation);
					active.erase(it);
				}
				else if (a_result.status == DispatchStatus::kCancelled)
				{
					// A cancelled release is obsolete only when the corresponding lease was
					// already removed (for example, its pending down failed). Otherwise retry.
					it->second.phase = LeasePhase::kReleaseRetry;
					it->second.retryAtMs = NowMs() + kReleaseRetryDelay.count();
				}
				else
				{
					it->second.phase = LeasePhase::kReleaseRetry;
					it->second.retryAtMs = NowMs() + kReleaseRetryDelay.count();
				}
			}
			cv.notify_all();
			if (lease)
				Publish(DispatchSucceeded(a_result) ? "up" : "releaseRetry", *lease,
					reason.empty() ? "request" : reason, &a_result, false);
		}

		DispatchSubmission SubmitDown(const KeyboardLease& a_lease)
		{
			auto gate = std::make_shared<DispatchGate>();
			{
				const std::lock_guard lock{ mutex };
				const auto            it = active.find(a_lease.key.scancode);
				if (it == active.end() || it->second.lease.generation != a_lease.generation)
				{
					return { false,
						DispatchResult{ DispatchStatus::kCancelled, -1, false,
							"keyboard lease became obsolete before dispatch" } };
				}
				it->second.downGate = gate;
			}

			const std::weak_ptr weak = shared_from_this();
			return Dispatch(ButtonCommand{
				.key = a_lease.key,
				.down = true,
				.heldSecs = 0.0F,
				.gate = gate,
				.complete = [weak, scancode = a_lease.key.scancode,
								generation = a_lease.generation, gate](DispatchResult a_result) {
					if (const auto self = weak.lock())
						self->OnDownComplete(scancode, generation, gate, std::move(a_result));
				},
			});
		}

		struct ReleaseResult
		{
			bool                          releasedWithoutEvent = false;
			bool                          alreadyPending = false;
			std::optional<DispatchResult> immediate;
			bool                          pending = false;
		};

		ReleaseResult ReleaseGeneration(
			std::uint16_t a_scancode, std::uint64_t a_generation, std::string a_reason)
		{
			KeyboardLease                 lease;
			std::shared_ptr<DispatchGate> releaseGate;
			{
				const std::lock_guard lock{ mutex };
				const auto            it = active.find(a_scancode);
				if (it == active.end() || it->second.lease.generation != a_generation)
					return { .releasedWithoutEvent = true };

				if (it->second.phase == LeasePhase::kReleasePending)
					return { .alreadyPending = true, .pending = true };

				// If the native down has not begun, cancelling its gate proves no input event
				// can be appended later. No compensating up is needed and the lease can retire.
				if (it->second.phase == LeasePhase::kDownPending && it->second.downGate &&
					it->second.downGate->Cancel())
				{
					lease = it->second.lease;
					leases.RemoveExact(a_scancode, a_generation);
					active.erase(it);
				}
				else
				{
					lease = it->second.lease;
					releaseGate = std::make_shared<DispatchGate>();
					it->second.releaseGate = releaseGate;
					it->second.releaseReason = std::move(a_reason);
					it->second.phase = LeasePhase::kReleasePending;
				}
			}

			if (!releaseGate)
			{
				cv.notify_all();
				const DispatchResult cancelled{
					DispatchStatus::kCancelled, -1, false,
					"pending synthetic key-down was cancelled before native dispatch"
				};
				Publish("up", lease, "cancelledPendingDown", &cancelled, false);
				return { .releasedWithoutEvent = true };
			}

			const float heldSecs = std::max(
				0.001F, static_cast<float>(NowMs() - lease.pressedAtMs) / 1000.0F);
			const std::weak_ptr weak = shared_from_this();
			auto                submission = Dispatch(ButtonCommand{
				.key = lease.key,
				.down = false,
				.heldSecs = heldSecs,
				.gate = releaseGate,
				.complete = [weak, scancode = lease.key.scancode,
								generation = lease.generation, releaseGate](DispatchResult a_result) {
					if (const auto self = weak.lock())
						self->OnReleaseComplete(
							scancode, generation, releaseGate, std::move(a_result));
				},
			});
			return {
				.immediate = std::move(submission.result),
				.pending = submission.pending,
			};
		}

		json Capabilities() const
		{
			json keys = json::array();
			for (const auto& key : KeyboardKeyCatalog())
				keys.push_back(KeyJson(key));
			return json{
				{ "contract", ContractJson() },
				{ "capabilities",
					json{
						{ "keyboard",
							json{
								{ "version", kKeyboardContractVersion },
								{ "available", backend.available },
								{ "ready", backend.available && ready.load(std::memory_order_acquire) },
								{ "injection", backend.injection },
								{ "encoding", "DirectInputScanCode" },
								{ "clock", "wall" },
								{ "actions",
									json::array(
										{ "status", "down", "up", "tap", "sequence", "releaseAll" }) },
								{ "defaultTapMs", kDefaultTapMs },
								{ "defaultMaxHoldMs", kDefaultMaxHoldMs },
								{ "maximumMaxHoldMs", kMaximumMaxHoldMs },
								{ "maximumHeldKeys", kMaximumHeldKeys },
								{ "maximumSequenceEvents", kMaximumSequenceEvents },
								{ "maximumSequenceMs", kMaximumSequenceMs },
								{ "keys", std::move(keys) },
							} },
					} },
			};
		}

		json Status()
		{
			const auto now = NowMs();
			json       held = json::array();
			{
				const std::lock_guard lock{ mutex };
				for (const auto& lease : leases.Snapshot())
				{
					json item = KeyJson(lease.key);
					item["owner"] = lease.owner;
					item["generation"] = lease.generation;
					item["heldForMs"] = std::max<std::int64_t>(0, now - lease.pressedAtMs);
					item["remainingMs"] = std::max<std::int64_t>(0, lease.expiresAtMs - now);
					if (const auto it = active.find(lease.key.scancode); it != active.end())
					{
						switch (it->second.phase)
						{
							case LeasePhase::kDownPending:
								item["state"] = "downPending";
								break;
							case LeasePhase::kPressed:
								item["state"] = "pressed";
								break;
							case LeasePhase::kReleasePending:
								item["state"] = "releasePending";
								break;
							case LeasePhase::kReleaseRetry:
								item["state"] = "releaseRetry";
								break;
						}
					}
					held.push_back(std::move(item));
				}
			}
			return json{
				{ "contract", ContractJson() },
				{ "device", "keyboard" },
				{ "ready", backend.available && ready.load(std::memory_order_acquire) },
				{ "held", std::move(held) },
			};
		}

		json Down(const KeyboardKey& a_key, const std::string& a_owner, int a_maxHoldMs,
			bool a_requireFresh = false)
		{
			RequireReady();
			KeyboardAcquireResult acquired;
			{
				const std::lock_guard lock{ mutex };
				if (leases.Size() >= kMaximumHeldKeys && !leases.Find(a_key.scancode))
					throw ToolError(409,
						std::format("keyboard synthetic hold limit reached ({}); release a key or "
									"call releaseAll",
							kMaximumHeldKeys));
				acquired = leases.Acquire(a_key, a_owner, NowMs(), a_maxHoldMs);
				if (acquired.status == KeyboardAcquireStatus::kConflict)
					throw ToolError(409,
						std::format(
							"key '{}' is held by owner '{}'", a_key.name, acquired.lease.owner));
				if (acquired.status == KeyboardAcquireStatus::kAlreadyOwned)
				{
					if (a_requireFresh)
						throw ToolError(409,
							std::format("key '{}' is already held by this owner; a tap/sequence "
										"will not release an earlier hold",
								a_key.name));
					json result{
						{ "contract", ContractJson() },
						{ "device", "keyboard" },
						{ "action", "down" },
						{ "owner", acquired.lease.owner },
						{ "generation", acquired.lease.generation },
						{ "accepted", true },
						{ "alreadyHeld", true },
					};
					result.update(KeyJson(acquired.lease.key));
					return result;
				}
				active.emplace(a_key.scancode,
					ActiveLease{ .lease = acquired.lease, .phase = LeasePhase::kDownPending });
			}

			auto submission = SubmitDown(acquired.lease);
			cv.notify_all();
			if (submission.result && !DispatchSucceeded(*submission.result))
				ThrowDispatchError(*submission.result);

			json result{
				{ "contract", ContractJson() },
				{ "device", "keyboard" },
				{ "action", "down" },
				{ "owner", acquired.lease.owner },
				{ "generation", acquired.lease.generation },
				{ "accepted", true },
				{ "pending", submission.pending },
				{ "frame", submission.result ? submission.result->frame : -1 },
				{ "uncertain",
					submission.result && submission.result->status == DispatchStatus::kUnknown },
			};
			result.update(KeyJson(acquired.lease.key));
			Publish("down", acquired.lease, "request", submission.result ? &*submission.result : nullptr,
				submission.pending);
			return result;
		}

		json Up(const KeyboardKey& a_key, const std::string& a_owner, bool a_force,
			std::string a_reason = "request")
		{
			RequireReady();
			KeyboardLease lease;
			{
				const std::lock_guard lock{ mutex };
				const auto            current = leases.Find(a_key.scancode);
				if (!current)
				{
					json result{
						{ "contract", ContractJson() },
						{ "device", "keyboard" },
						{ "action", "up" },
						{ "released", false },
						{ "notHeld", true },
					};
					result.update(KeyJson(a_key));
					return result;
				}
				if (!a_force && current->owner != a_owner)
					throw ToolError(409,
						std::format("key '{}' is held by owner '{}' (not '{}')", a_key.name,
							current->owner, a_owner));
				lease = *current;
			}

			auto release = ReleaseGeneration(a_key.scancode, lease.generation, std::move(a_reason));
			if (release.immediate && !DispatchSucceeded(*release.immediate))
				ThrowDispatchError(*release.immediate);

			json result{
				{ "contract", ContractJson() },
				{ "device", "keyboard" },
				{ "action", "up" },
				{ "owner", lease.owner },
				{ "generation", lease.generation },
				{ "accepted", true },
				{ "pending", release.pending || release.alreadyPending },
				{ "released", release.releasedWithoutEvent ||
								  (release.immediate && DispatchSucceeded(*release.immediate)) },
				{ "frame", release.immediate ? release.immediate->frame : -1 },
				{ "uncertain",
					release.immediate &&
						release.immediate->status == DispatchStatus::kUnknown },
			};
			result.update(KeyJson(lease.key));
			return result;
		}

		json Tap(const KeyboardKey& a_key, const std::string& a_owner, int a_durationMs)
		{
			json down =
				Down(a_key, a_owner, std::min(kMaximumMaxHoldMs, a_durationMs + 2000), true);
			std::this_thread::sleep_for(milliseconds(a_durationMs));
			json up = Up(a_key, a_owner, false);
			json result{
				{ "contract", ContractJson() },
				{ "device", "keyboard" },
				{ "action", "tap" },
				{ "durationMs", a_durationMs },
				{ "down", std::move(down) },
				{ "up", std::move(up) },
			};
			result.update(KeyJson(a_key));
			return result;
		}

		struct SequenceEvent
		{
			std::string                action;
			std::optional<KeyboardKey> key;
			int                        durationMs = 0;
			int                        afterMs = 0;
		};

		std::pair<std::vector<SequenceEvent>, int> ParseSequence(const json& a_args)
		{
			const auto it = a_args.find("events");
			if (it == a_args.end() || !it->is_array())
				throw ToolError(400, "action='sequence' requires an 'events' array");
			if (it->empty() || it->size() > kMaximumSequenceEvents)
				throw ToolError(
					400, std::format("'events' must contain 1..{} entries", kMaximumSequenceEvents));

			std::vector<SequenceEvent>        parsed;
			std::unordered_set<std::uint16_t> balanced;
			int                               totalMs = 0;
			for (const auto& value : *it)
			{
				if (!value.is_object())
					throw ToolError(400, "each sequence event must be an object");
				SequenceEvent event;
				event.action = value.value("action", std::string("tap"));
				event.afterMs = BoundedInteger(value, "afterMs", 0, 0, 10000);
				totalMs += event.afterMs;
				if (event.action == "wait")
				{
					event.durationMs = BoundedInteger(value, "durationMs", 0, 0, 10000);
					totalMs += event.durationMs;
				}
				else
				{
					event.key = ParseKey(value);
					if (event.action == "tap")
					{
						if (balanced.contains(event.key->scancode))
							throw ToolError(400,
								std::format("sequence taps key '{}' while it is held by an earlier "
											"sequence down",
									event.key->name));
						event.durationMs =
							BoundedInteger(value, "durationMs", kDefaultTapMs, 10, 5000);
						totalMs += event.durationMs;
					}
					else if (event.action == "down")
					{
						if (!balanced.insert(event.key->scancode).second)
							throw ToolError(400,
								std::format("sequence presses key '{}' twice without an intervening "
											"up",
									event.key->name));
					}
					else if (event.action == "up")
					{
						if (!balanced.erase(event.key->scancode))
							throw ToolError(400,
								std::format("sequence releases key '{}' without a matching sequence "
											"down",
									event.key->name));
					}
					else
					{
						throw ToolError(400,
							std::format("unknown sequence event action '{}' "
										"(tap|down|up|wait)",
								event.action));
					}
				}
				parsed.push_back(std::move(event));
			}
			if (!balanced.empty())
				throw ToolError(400,
					"sequence contains an unbalanced down; add a matching up or use action='down' "
					"for a bounded persistent hold");
			if (totalMs > kMaximumSequenceMs)
				throw ToolError(400,
					std::format(
						"sequence duration {}ms exceeds the {}ms limit", totalMs, kMaximumSequenceMs));
			return { std::move(parsed), totalMs };
		}

		json Sequence(const json& a_args, const std::string& a_owner)
		{
			RequireReady();
			auto [eventsToRun, totalMs] = ParseSequence(a_args);
			json                       results = json::array();
			std::vector<KeyboardLease> opened;
			try
			{
				for (const auto& event : eventsToRun)
				{
					json result;
					if (event.action == "wait")
					{
						std::this_thread::sleep_for(milliseconds(event.durationMs));
						result = json{ { "action", "wait" }, { "durationMs", event.durationMs } };
					}
					else if (event.action == "tap")
					{
						result = Tap(*event.key, a_owner, event.durationMs);
					}
					else if (event.action == "down")
					{
						result =
							Down(*event.key, a_owner, std::min(kMaximumMaxHoldMs, totalMs + 2000), true);
						opened.push_back(KeyboardLease{
							*event.key, a_owner, result.value("generation", 0ULL), 0, 0 });
					}
					else
					{
						result = Up(*event.key, a_owner, false);
						std::erase_if(opened, [&](const auto& a_lease) {
							return a_lease.key.scancode == event.key->scancode;
						});
					}
					results.push_back(std::move(result));
					if (event.afterMs)
						std::this_thread::sleep_for(milliseconds(event.afterMs));
				}
			}
			catch (...)
			{
				for (const auto& lease : opened)
				{
					try
					{
						(void)ReleaseGeneration(
							lease.key.scancode, lease.generation, "sequenceFailure");
					}
					catch (const std::exception& a_exception)
					{
						logs::warn(
							"devbench: sequence cleanup could not release '{}' generation {}: {}",
							lease.key.name, lease.generation, a_exception.what());
					}
				}
				throw;
			}

			return json{
				{ "contract", ContractJson() },
				{ "device", "keyboard" },
				{ "action", "sequence" },
				{ "owner", a_owner },
				{ "durationMs", totalMs },
				{ "eventsRun", results.size() },
				{ "results", std::move(results) },
			};
		}

		json ReleaseAll(const std::string& a_owner, bool a_all, std::string a_reason = "request")
		{
			RequireReady();
			std::vector<KeyboardLease> selected;
			{
				const std::lock_guard lock{ mutex };
				for (const auto& lease : leases.Snapshot())
				{
					if (a_all || lease.owner == a_owner)
						selected.push_back(lease);
				}
			}

			json released = json::array();
			json pending = json::array();
			json failed = json::array();
			for (const auto& lease : selected)
			{
				try
				{
					auto result =
						ReleaseGeneration(lease.key.scancode, lease.generation, a_reason);
					if (result.immediate && !DispatchSucceeded(*result.immediate))
					{
						json item = KeyJson(lease.key);
						item["error"] = result.immediate->message;
						failed.push_back(std::move(item));
					}
					else if (result.pending || result.alreadyPending)
					{
						pending.push_back(KeyJson(lease.key));
					}
					else
					{
						released.push_back(KeyJson(lease.key));
					}
				}
				catch (const std::exception& a_exception)
				{
					json item = KeyJson(lease.key);
					item["error"] = a_exception.what();
					failed.push_back(std::move(item));
				}
			}
			return json{
				{ "contract", ContractJson() },
				{ "device", "keyboard" },
				{ "action", "releaseAll" },
				{ "owner", a_all ? "*" : a_owner },
				{ "released", std::move(released) },
				{ "pending", std::move(pending) },
				{ "failed", std::move(failed) },
			};
		}

		void CancelQueuedDownsLocked(std::vector<KeyboardLease>& a_cancelled)
		{
			for (auto it = active.begin(); it != active.end();)
			{
				if (it->second.phase == LeasePhase::kDownPending && it->second.downGate &&
					it->second.downGate->Cancel())
				{
					a_cancelled.push_back(it->second.lease);
					leases.RemoveExact(it->first, it->second.lease.generation);
					it = active.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		void RequestLifecycleRelease(std::string a_reason) noexcept
		{
			std::vector<KeyboardLease> cancelled;
			try
			{
				{
					const std::lock_guard lock{ mutex };
					CancelQueuedDownsLocked(cancelled);
					lifecycleReason = std::move(a_reason);
					lifecyclePending = true;
				}
				cv.notify_all();
				const DispatchResult result{
					DispatchStatus::kCancelled, -1, false,
					"pending synthetic key-down was cancelled by a lifecycle boundary"
				};
				for (const auto& lease : cancelled)
					Publish("up", lease, "lifecycleCancelledPendingDown", &result, false);
			}
			catch (const std::exception& a_exception)
			{
				logs::error(
					"devbench: keyboard lifecycle cleanup request failed: {}", a_exception.what());
			}
			catch (...)
			{
				logs::error("{}", "devbench: keyboard lifecycle cleanup request failed");
			}
		}

		void WatchdogLoop() noexcept
		{
			for (;;)
			{
				std::vector<std::pair<KeyboardLease, std::string>> selected;
				{
					std::unique_lock lock{ mutex };
					if (workerStop)
						return;
					const auto now = NowMs();
					if (lifecyclePending)
					{
						for (const auto& lease : leases.Snapshot())
							selected.emplace_back(lease, lifecycleReason);
						lifecyclePending = false;
					}
					else
					{
						for (const auto& lease : leases.Expired(now))
							selected.emplace_back(lease, "leaseExpired");
						for (const auto& [scancode, activeLease] : active)
						{
							(void)scancode;
							if (activeLease.phase == LeasePhase::kReleaseRetry &&
								activeLease.retryAtMs <= now &&
								std::ranges::none_of(selected, [&](const auto& a_item) {
									return a_item.first.generation == activeLease.lease.generation;
								}))
								selected.emplace_back(activeLease.lease,
									activeLease.releaseReason.empty() ? "releaseRetry" :
																		activeLease.releaseReason);
						}
					}

					if (selected.empty())
					{
						std::int64_t wakeAt = now + 1000;
						for (const auto& lease : leases.Snapshot())
							wakeAt = std::min(wakeAt, lease.expiresAtMs);
						for (const auto& [scancode, activeLease] : active)
						{
							(void)scancode;
							if (activeLease.phase == LeasePhase::kReleaseRetry)
								wakeAt = std::min(wakeAt, activeLease.retryAtMs);
						}
						cv.wait_for(lock, milliseconds(std::max<std::int64_t>(1, wakeAt - now)));
						continue;
					}
				}

				for (const auto& [lease, reason] : selected)
				{
					try
					{
						(void)ReleaseGeneration(lease.key.scancode, lease.generation, reason);
					}
					catch (const std::exception& a_exception)
					{
						logs::warn(
							"devbench: automatic keyboard release remains pending for '{}' "
							"generation {}: {}",
							lease.key.name, lease.generation, a_exception.what());
					}
				}
			}
		}

		void Shutdown() noexcept
		{
			try
			{
				{
					const std::lock_guard lock{ mutex };
					if (shutdownStarted)
						return;
					shutdownStarted = true;
					accepting = false;
					ready.store(false, std::memory_order_release);
				}
				RequestLifecycleRelease("shutdown");

				const auto deadline = steady_clock::now() + kShutdownCleanupBudget;
				{
					std::unique_lock lock{ mutex };
					cv.wait_until(lock, deadline, [&] { return leases.Size() == 0; });
					std::vector<KeyboardLease> cancelled;
					CancelQueuedDownsLocked(cancelled);
					workerStop = true;
				}
				cv.notify_all();
				if (watchdog.joinable())
					watchdog.join();

				std::size_t remaining = 0;
				{
					const std::lock_guard lock{ mutex };
					remaining = leases.Size();
					events = nullptr;
				}
				if (remaining)
					logs::warn(
						"devbench: keyboard service shutdown left {} native release(s) pending",
						remaining);
			}
			catch (const std::exception& a_exception)
			{
				logs::error("devbench: keyboard service shutdown failed: {}", a_exception.what());
				{
					const std::lock_guard lock{ mutex };
					workerStop = true;
					events = nullptr;
				}
				cv.notify_all();
				if (watchdog.joinable())
					watchdog.join();
			}
			catch (...)
			{
				logs::error("{}", "devbench: keyboard service shutdown failed");
				{
					const std::lock_guard lock{ mutex };
					workerStop = true;
					events = nullptr;
				}
				cv.notify_all();
				if (watchdog.joinable())
					watchdog.join();
			}
		}
	};

	InputService::InputService(
		EventBus& a_events, bool a_allowControlActions, KeyboardBackend a_backend) :
		state_(std::make_shared<State>(a_events, a_allowControlActions, std::move(a_backend)))
	{
		state_->Start();
	}

	InputService::~InputService()
	{
		Shutdown();
	}

	json InputService::Handle(const json& a_args, const ToolContext& a_context)
	{
		try
		{
			RequireObject(a_args);
			const auto action = a_args.value("action", std::string("capabilities"));
			if (action == "capabilities")
				return state_->Capabilities();

			if (const auto it = a_args.find("device"); it != a_args.end() && !it->is_string())
				throw ToolError(400, "'device' must be a string");
			const auto device = a_args.value("device", std::string("keyboard"));
			if (device != "keyboard")
				throw ToolError(400, "input contract v1 supports only device='keyboard'");
			if (action == "status")
				return state_->Status();

			RequireToolPermission(
				state_->allowControlActions, ToolPermission::kControlActions);
			if (!a_context.internal && BooleanArgument(a_args, "force", false))
				throw ToolError(403, "cross-owner keyboard release is reserved for internal cleanup");
			if (!a_context.internal && BooleanArgument(a_args, "all", false))
				throw ToolError(
					403, "all-owner keyboard cleanup is reserved for internal lifecycle/replay paths");

			const auto owner = ResolveOwner(a_args, a_context);
			if (action == "down")
				return state_->Down(ParseKey(a_args), owner,
					BoundedInteger(
						a_args, "maxHoldMs", kDefaultMaxHoldMs, 100, kMaximumMaxHoldMs));
			if (action == "up")
				return state_->Up(ParseKey(a_args), owner,
					a_context.internal && BooleanArgument(a_args, "force", false));
			if (action == "tap")
				return state_->Tap(ParseKey(a_args), owner,
					BoundedInteger(a_args, "durationMs", kDefaultTapMs, 10, 5000));
			if (action == "sequence")
				return state_->Sequence(a_args, owner);
			if (action == "releaseAll")
				return state_->ReleaseAll(owner,
					a_context.internal && BooleanArgument(a_args, "all", false));
			throw ToolError(400,
				std::format("unknown input action '{}' "
							"(capabilities|status|down|up|tap|sequence|releaseAll)",
					action));
		}
		catch (const json::exception& a_exception)
		{
			throw ToolError(
				400, std::format("invalid keyboard input JSON: {}", a_exception.what()));
		}
	}

	void InputService::SetReady(bool a_ready) noexcept
	{
		state_->ready.store(a_ready, std::memory_order_release);
	}

	void InputService::ReleaseForLifecycle(std::string a_reason) noexcept
	{
		state_->RequestLifecycleRelease(std::move(a_reason));
	}

	void InputService::Shutdown() noexcept
	{
		if (state_)
			state_->Shutdown();
	}

	ToolDescriptor BuildInputDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "input";
		descriptor.description =
			"FO4-only synthetic keyboard input through Fallout 4's BSInputEventQueue. "
			"action='capabilities' (default) reports readiness, limits, injection path, and "
			"the complete DirectInput scan-code catalog. Mutations require "
			"allowControlActions=true. 'down' creates a bounded owner lease; same-owner down "
			"is idempotent and another owner receives 409. 'up' releases only that owner's "
			"lease. 'tap' and 'sequence' use wall-clock timing; sequence is fully prevalidated "
			"and balanced before dispatch. 'releaseAll' is owner-scoped externally. Queued "
			"native work is generation-gated so cancelled, expired, or lifecycle-invalidated "
			"downs cannot execute later. Releases remain leased and retry until native "
			"acknowledgement. Physical keyboard state is observed but never modified. This "
			"contract does not expose VR tracked input or claim render-frame pacing.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties",
				json{
					{ "action",
						json{
							{ "type", "string" },
							{ "enum",
								json::array({ "capabilities", "status", "down", "up", "tap",
									"sequence", "releaseAll" }) },
							{ "default", "capabilities" },
						} },
					{ "device",
						json{
							{ "type", "string" },
							{ "enum", json::array({ "keyboard" }) },
						} },
					{ "key",
						json{
							{ "oneOf",
								json::array({
									json{ { "type", "string" } },
									json{
										{ "type", "integer" },
										{ "minimum", 1 },
										{ "maximum", 255 },
									},
								}) },
						} },
					{ "owner",
						json{
							{ "type", "string" },
							{ "minLength", 1 },
							{ "maxLength", 128 },
						} },
					{ "durationMs",
						json{
							{ "type", "integer" },
							{ "minimum", 10 },
							{ "maximum", 5000 },
						} },
					{ "maxHoldMs",
						json{
							{ "type", "integer" },
							{ "minimum", 100 },
							{ "maximum", kMaximumMaxHoldMs },
						} },
					{ "force", json{ { "type", "boolean" } } },
					{ "all", json{ { "type", "boolean" } } },
					{ "events",
						json{
							{ "type", "array" },
							{ "minItems", 1 },
							{ "maxItems", kMaximumSequenceEvents },
							{ "items", json{ { "type", "object" } } },
						} },
				} },
			{ "additionalProperties", false },
		};
		return descriptor;
	}

	std::shared_ptr<InputService> RegisterInputTool(
		ToolRegistry& a_registry, EventBus& a_events, bool a_allowControlActions,
		KeyboardBackend a_backend)
	{
		auto service =
			std::make_shared<InputService>(a_events, a_allowControlActions, std::move(a_backend));
		const std::weak_ptr weak = service;
		a_registry.Register(BuildInputDescriptor(),
			[weak](const json& a_args, const ToolContext& a_context) {
				const auto service = weak.lock();
				if (!service)
					throw ToolError(503, "keyboard input service is unavailable");
				return service->Handle(a_args, a_context);
			});
		return service;
	}
}

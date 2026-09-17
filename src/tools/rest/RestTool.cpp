#include "RestTool.h"

#include "tools/ToolPermissions.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace dvb::tools::rest
{
	namespace
	{
		std::string_view KindName(RestKind a_kind)
		{
			return a_kind == RestKind::kSleep ? "sleep" : "wait";
		}
	}

	std::string_view RestResultStatusName(RestResultStatus a_status) noexcept
	{
		switch (a_status)
		{
			case RestResultStatus::kCompleted:
				return "completed";
			case RestResultStatus::kRefused:
				return "refused";
			case RestResultStatus::kCancelled:
				return "cancelled";
			case RestResultStatus::kUnavailable:
				return "unavailable";
			case RestResultStatus::kFailed:
				return "failed";
		}
		return "failed";
	}

	bool RestResultSucceeded(const RestResult& a_result) noexcept
	{
		return a_result.status == RestResultStatus::kCompleted &&
		       !a_result.interrupted;
	}

	json RestResultToJson(const RestResult& a_result)
	{
		json out{
			{ "outcome", RestResultStatusName(a_result.status) },
			{ "interrupted", a_result.interrupted },
			{ "elapsedGameHours",
				a_result.elapsedGameHours ? json(*a_result.elapsedGameHours) : json(nullptr) },
			{ "message", a_result.message.empty() ? json(nullptr) : json(a_result.message) },
		};
		for (const auto& [key, value] : a_result.details.items())
			out[key] = value;
		return out;
	}

	namespace
	{
		void RequireObject(const json& a_args)
		{
			if (!a_args.is_object())
				throw ToolError(400, "wait/sleep arguments must be an object");
		}

		void ValidateOnly(
			const json& a_args, std::initializer_list<std::string_view> a_allowed)
		{
			for (const auto& [key, value] : a_args.items())
			{
				(void)value;
				if (std::ranges::find(a_allowed, key) == a_allowed.end())
					throw ToolError(
						400, std::format("unexpected parameter '{}' for wait/sleep", key));
			}
		}

		int ReadHours(const json& a_args)
		{
			const auto it = a_args.find("hours");
			if (it == a_args.end() || !it->is_number_integer())
				throw ToolError(400, "'hours' must be an integer");
			const auto hours = it->get<std::int64_t>();
			if (hours < kMinimumRestHours || hours > kMaximumRestHours)
				throw ToolError(400,
					std::format("'hours' must be within {}..{}",
						kMinimumRestHours, kMaximumRestHours));
			return static_cast<int>(hours);
		}

		std::optional<std::string> ReadTarget(RestKind a_kind, const json& a_args)
		{
			const auto it = a_args.find("target");
			if (a_kind == RestKind::kWait)
			{
				if (it != a_args.end())
					throw ToolError(400, "'target' is only valid for sleep");
				return std::nullopt;
			}
			if (it == a_args.end() || !it->is_string())
				throw ToolError(
					400, "sleep requires 'target' identifying a placed bed reference");
			auto target = it->get<std::string>();
			if (target.empty() || target.size() > 255 || target.find('\0') != std::string::npos)
				throw ToolError(
					400, "'target' must be a non-empty form ID/editor ID of at most 255 bytes");
			return target;
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

	struct RestService::State : std::enable_shared_from_this<RestService::State>
	{
		struct Operation
		{
			std::uint64_t                 id = 0;
			std::uint64_t                 actionCursor = 0;
			RestKind                      kind = RestKind::kWait;
			int                           hours = 0;
			std::optional<std::string>    target;
			std::string                   phase = "reserved";
			bool                          dispatched = false;
			bool                          active = true;
			bool                          uncertain = false;
			std::shared_ptr<DispatchGate> gate;
			std::optional<RestResult>     result;
		};

		const bool               allowGameActions;
		RestBackend              backend;
		RestOperationCoordinator operations;
		RestServiceConfig        config;
		std::atomic<bool>        ready{ false };
		std::mutex               mutex;
		std::condition_variable  cv;
		std::optional<Operation> operation;
		bool                     accepting = true;

		State(
			bool a_allowGameActions, RestBackend a_backend,
			RestOperationCoordinator a_operations, RestServiceConfig a_config) :
			allowGameActions(a_allowGameActions),
			backend(std::move(a_backend)),
			operations(std::move(a_operations)),
			config(a_config)
		{}

		json CapabilityJson(RestKind a_kind) const
		{
			const bool coordinated =
				operations.tryBegin && operations.markDispatching &&
				operations.markQueued && operations.finish && operations.fail &&
				operations.cancelReserved;
			return json{
				{ "tool", KindName(a_kind) },
				{ "available", backend.available && coordinated },
				{ "ready", ready.load(std::memory_order_acquire) },
				{ "implementation",
					backend.implementation.empty() ? json(nullptr) : json(backend.implementation) },
				{ "nativeRuntimeValidated", backend.runtimeValidated },
				{ "runtimeValidation",
					backend.runtimeValidated ?
						"Native wait/sleep and autosave flow verified for the backend's supported "
						"runtime; current game/mod rules and native save policy still apply." :
						"Native backend has not been runtime-validated." },
				{ "hours",
					json{
						{ "minimum", kMinimumRestHours },
						{ "maximum", kMaximumRestHours },
						{ "source", "loaded SleepWaitMenu hours property and ModifyHours clamp" },
					} },
				{ "autosaves", "nativePolicy" },
				{ "requiresBedTarget", a_kind == RestKind::kSleep },
				{ "sharedGameOperationReservation", coordinated },
			};
		}

		json OperationJson(const Operation& a_operation) const
		{
			json out{
				{ "operationId", a_operation.id },
				{ "actionCursor", a_operation.actionCursor },
				{ "kind", KindName(a_operation.kind) },
				{ "hours", a_operation.hours },
				{ "target",
					a_operation.target ? json(*a_operation.target) : json(nullptr) },
				{ "phase", a_operation.phase },
				{ "active", a_operation.active },
				{ "queued", a_operation.active },
				{ "completed",
					a_operation.result &&
							a_operation.result->status == RestResultStatus::kCompleted ?
						json(true) :
						json(false) },
				{ "uncertain", a_operation.uncertain },
			};
			if (a_operation.uncertain)
				out["note"] =
					"native dispatch began but did not finish before the listener timeout; "
					"do not retry, poll action='status'";
			if (a_operation.result)
			{
				for (const auto& [key, value] :
					RestResultToJson(*a_operation.result).items())
					out[key] = value;
			}
			return out;
		}

		json Status(RestKind a_kind)
		{
			const std::lock_guard lock{ mutex };
			return json{
				{ "capabilities", CapabilityJson(a_kind) },
				{ "operation", operation ? OperationJson(*operation) : json(nullptr) },
			};
		}

		bool MarkDispatching(std::uint64_t a_operationId)
		{
			if (!operations.markDispatching ||
				!operations.markDispatching(a_operationId))
				return false;

			const std::lock_guard lock{ mutex };
			if (operation && operation->id == a_operationId && operation->active)
			{
				operation->phase = "dispatching";
				operation->dispatched = true;
			}
			return true;
		}

		bool MarkQueued(std::uint64_t a_operationId)
		{
			if (!operations.markQueued || !operations.markQueued(a_operationId))
				return false;

			const std::lock_guard lock{ mutex };
			if (operation && operation->id == a_operationId && operation->active)
			{
				operation->phase = "queued";
				operation->dispatched = true;
			}
			return true;
		}

		void Complete(std::uint64_t a_operationId, RestResult a_result)
		{
			bool dispatched = false;
			{
				const std::lock_guard lock{ mutex };
				if (!operation || operation->id != a_operationId || !operation->active)
					return;
				dispatched = operation->dispatched;
				operation->result = a_result;
				operation->phase =
					std::string(RestResultStatusName(operation->result->status));
				operation->active = false;
				operation->uncertain = false;
			}
			if (dispatched)
			{
				if (operations.finish)
					(void)operations.finish(a_operationId, a_result);
			}
			else if (a_result.status == RestResultStatus::kCancelled)
			{
				if (operations.cancelReserved)
					(void)operations.cancelReserved(a_operationId, a_result.message);
			}
			else
			{
				if (operations.fail)
					(void)operations.fail(a_operationId, a_result.message);
			}
			cv.notify_all();
		}

		json FinishResponse(const Operation& a_operation) const
		{
			if (!a_operation.result)
				return OperationJson(a_operation);

			switch (a_operation.result->status)
			{
				case RestResultStatus::kCompleted:
				case RestResultStatus::kRefused:
					return OperationJson(a_operation);
				case RestResultStatus::kCancelled:
					throw ToolError(504,
						a_operation.result->message.empty() ?
							"wait/sleep dispatch was cancelled before native execution" :
							a_operation.result->message);
				case RestResultStatus::kUnavailable:
					throw ToolError(503,
						a_operation.result->message.empty() ?
							"native wait/sleep is unavailable" :
							a_operation.result->message);
				case RestResultStatus::kFailed:
					throw ToolError(500,
						a_operation.result->message.empty() ?
							"native wait/sleep failed" :
							a_operation.result->message);
			}
			throw ToolError(500, "native wait/sleep returned an invalid result");
		}

		json Start(RestKind a_kind, int a_hours, std::optional<std::string> a_target)
		{
			RequireToolPermission(allowGameActions, ToolPermission::kGameActions);
			if (!ready.load(std::memory_order_acquire))
				throw ToolError(503, "wait/sleep is unavailable until a stable game is loaded");
			if (!backend.available || !backend.dispatch)
				throw ToolError(503, "native wait/sleep backend is unavailable");
			if (!operations.tryBegin || !operations.markDispatching ||
				!operations.markQueued || !operations.finish ||
				!operations.fail || !operations.cancelReserved)
				throw ToolError(
					503, "shared game-operation coordination is unavailable for wait/sleep");

			{
				const std::lock_guard lock{ mutex };
				if (!accepting)
					throw ToolError(503, "wait/sleep service is shutting down");
				if (operation && operation->active)
					throw ToolError(409,
						std::format(
							"wait/sleep operation {} is still active; poll action='status'",
							operation->id));
			}

			const auto reservation = operations.tryBegin(a_kind, a_hours, a_target);
			if (!reservation || reservation->operationId == 0)
				throw ToolError(
					409, "another game mutation is active; poll game action='status'");

			const auto                    operationId = reservation->operationId;
			std::shared_ptr<DispatchGate> gate = std::make_shared<DispatchGate>();
			const auto                    commandTarget = a_target;
			{
				const std::lock_guard lock{ mutex };
				if (!accepting)
				{
					(void)operations.cancelReserved(
						operationId, "wait/sleep service shut down before dispatch");
					throw ToolError(503, "wait/sleep service is shutting down");
				}
				if (operation && operation->active)
				{
					(void)operations.cancelReserved(
						operationId, "another wait/sleep operation became active");
					throw ToolError(
						409, "another wait/sleep operation became active");
				}
				operation = Operation{
					.id = operationId,
					.actionCursor = reservation->actionCursor,
					.kind = a_kind,
					.hours = a_hours,
					.target = std::move(a_target),
					.gate = gate,
				};
			}

			const std::weak_ptr weak = shared_from_this();
			RestSubmission      submission;
			try
			{
				submission = backend.dispatch(RestCommand{
					.kind = a_kind,
					.hours = a_hours,
					.target = commandTarget,
					.operationId = operationId,
					.gate = gate,
					.markDispatching = [weak, operationId] {
						if (const auto state = weak.lock())
							return state->MarkDispatching(operationId);
						return false; },
					.markQueued = [weak, operationId] {
						if (const auto state = weak.lock())
							return state->MarkQueued(operationId);
						return false; },
					.complete = [weak, operationId](RestResult a_result) {
						if (const auto state = weak.lock())
							state->Complete(operationId, std::move(a_result)); },
				});
			}
			catch (const std::exception& a_exception)
			{
				Complete(operationId,
					{ RestResultStatus::kUnavailable, false, std::nullopt,
						std::format("could not queue native wait/sleep: {}", a_exception.what()) });
			}

			if (submission.result)
				Complete(operationId, std::move(*submission.result));
			else if (!submission.pending)
				Complete(operationId,
					{ RestResultStatus::kFailed, false, std::nullopt,
						"native wait/sleep dispatcher returned no result" });

			std::unique_lock lock{ mutex };
			const auto       terminal = [&] {
				return !accepting || !operation || operation->id != operationId ||
				       !operation->active;
			};
			if (!cv.wait_for(lock, config.listenerTimeout, terminal))
			{
				lock.unlock();
				const bool cancelled = gate->Cancel();
				lock.lock();
				if (!operation || operation->id != operationId)
					throw ToolError(500, "wait/sleep operation status was lost");
				if (!operation->active)
					return FinishResponse(*operation);
				if (cancelled)
				{
					(void)operations.cancelReserved(operationId,
						"wait/sleep listener timed out before native dispatch");
					operation->phase = "cancelled";
					operation->active = false;
					operation->result = RestResult{
						RestResultStatus::kCancelled, false, std::nullopt,
						"wait/sleep listener timed out and cancelled dispatch before native execution"
					};
					throw ToolError(504, operation->result->message);
				}
				operation->phase = "uncertain";
				operation->uncertain = true;
				return OperationJson(*operation);
			}

			if (!operation || operation->id != operationId)
				throw ToolError(500, "wait/sleep operation status was lost");
			return FinishResponse(*operation);
		}

		json Handle(RestKind a_kind, const json& a_args)
		{
			RequireObject(a_args);
			const auto actionIt = a_args.find("action");
			const auto action =
				actionIt == a_args.end() ? std::string("start") :
				actionIt->is_string()    ? actionIt->get<std::string>() :
										   throw ToolError(400, "'action' must be a string");
			if (action == "status")
			{
				ValidateOnly(a_args, { "action" });
				return Status(a_kind);
			}
			if (action != "start")
				throw ToolError(400, std::format("unknown wait/sleep action '{}'", action));

			ValidateOnly(a_args, { "action", "hours", "target" });
			return Start(a_kind, ReadHours(a_args), ReadTarget(a_kind, a_args));
		}

		void Shutdown() noexcept
		{
			std::shared_ptr<DispatchGate> gate;
			{
				const std::lock_guard lock{ mutex };
				if (!accepting)
					return;
				accepting = false;
				if (operation && operation->active)
					gate = operation->gate;
			}
			if (gate)
			{
				const bool            cancelled = gate->Cancel();
				const std::lock_guard lock{ mutex };
				if (operation && operation->active)
				{
					if (cancelled)
					{
						if (operations.cancelReserved)
							(void)operations.cancelReserved(
								operation->id,
								"wait/sleep service shut down before native dispatch");
						operation->phase = "cancelled";
						operation->active = false;
						operation->result = RestResult{
							RestResultStatus::kCancelled, false, std::nullopt,
							"wait/sleep service shut down before native dispatch"
						};
					}
					else
					{
						operation->phase = "uncertain";
						operation->uncertain = true;
					}
				}
			}
			if (backend.shutdown)
			{
				try
				{
					backend.shutdown();
				}
				catch (...)
				{}
			}
			cv.notify_all();
		}
	};

	RestService::RestService(
		bool a_allowGameActions, RestBackend a_backend,
		RestOperationCoordinator a_operations, RestServiceConfig a_config) :
		state_(std::make_shared<State>(
			a_allowGameActions, std::move(a_backend), std::move(a_operations), a_config))
	{}

	RestService::~RestService()
	{
		Shutdown();
	}

	json RestService::Handle(RestKind a_kind, const json& a_args)
	{
		return state_->Handle(a_kind, a_args);
	}

	void RestService::SetReady(bool a_ready) noexcept
	{
		state_->ready.store(a_ready, std::memory_order_release);
	}

	void RestService::Shutdown() noexcept
	{
		if (state_)
			state_->Shutdown();
	}

	ToolDescriptor BuildWaitDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "wait";
		descriptor.description =
			"Wait through the native sleep/wait menu gate and loaded UI hour clamp, "
			"normal update pumping, finish path, and native autosave policy. The host "
			"accepts up to 100000 hours, but the loaded menu decides the actual maximum. "
			"Use action='status' after an uncertain listener timeout; never retry an "
			"active operation.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties",
				json{
					{ "action",
						json{
							{ "type", "string" },
							{ "enum", json::array({ "start", "status" }) },
						} },
					{ "hours",
						json{
							{ "type", "integer" },
							{ "minimum", kMinimumRestHours },
							{ "maximum", kMaximumRestHours },
						} },
				} },
		};
		return descriptor;
	}

	ToolDescriptor BuildSleepDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "sleep";
		descriptor.description =
			"Sleep through the native sleep/wait menu path. The host accepts up to "
			"100000 hours, but the loaded menu decides the actual maximum. A placed bed "
			"reference target is required so furniture ownership, Survival/mod "
			"rules, interruptions, side effects, and native autosave policy are preserved. "
			"Use action='status' after an uncertain listener timeout.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties",
				json{
					{ "action",
						json{
							{ "type", "string" },
							{ "enum", json::array({ "start", "status" }) },
						} },
					{ "hours",
						json{
							{ "type", "integer" },
							{ "minimum", kMinimumRestHours },
							{ "maximum", kMaximumRestHours },
						} },
					{ "target",
						json{
							{ "type", "string" },
							{ "description",
								"placed bed reference form ID (hex/decimal) or editor ID" },
						} },
				} },
		};
		return descriptor;
	}

	std::shared_ptr<RestService> RegisterRestTools(
		ToolRegistry& a_registry, bool a_allowGameActions, RestBackend a_backend,
		RestOperationCoordinator a_operations,
		RestServiceConfig        a_config)
	{
		auto service = std::make_shared<RestService>(
			a_allowGameActions, std::move(a_backend), std::move(a_operations), a_config);
		const std::weak_ptr weak = service;
		a_registry.Register(BuildWaitDescriptor(),
			[weak](const json& a_args, const ToolContext&) {
				const auto service = weak.lock();
				if (!service)
					throw ToolError(503, "wait service is unavailable");
				return service->Handle(RestKind::kWait, a_args);
			});
		a_registry.Register(BuildSleepDescriptor(),
			[weak](const json& a_args, const ToolContext&) {
				const auto service = weak.lock();
				if (!service)
					throw ToolError(503, "sleep service is unavailable");
				return service->Handle(RestKind::kSleep, a_args);
			});
		return service;
	}
}

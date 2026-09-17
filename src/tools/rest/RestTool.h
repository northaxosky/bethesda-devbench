#pragma once

#include "ToolRegistry.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dvb::tools::rest
{
	inline constexpr int kMinimumRestHours = 1;
	inline constexpr int kMaximumRestHours = 100000;

	enum class RestKind
	{
		kWait,
		kSleep,
	};

	enum class RestResultStatus
	{
		kCompleted,
		kRefused,
		kCancelled,
		kUnavailable,
		kFailed,
	};

	class DispatchGate
	{
	public:
		bool TryBegin() noexcept;
		bool Cancel() noexcept;
		void Finish() noexcept;

	private:
		enum class State : std::uint8_t
		{
			kQueued,
			kRunning,
			kCancelled,
			kFinished,
		};
		std::atomic<State> state_{ State::kQueued };
	};

	struct RestResult
	{
		RestResultStatus      status = RestResultStatus::kFailed;
		bool                  interrupted = false;
		std::optional<double> elapsedGameHours;
		std::string           message;
		json                  details = json::object();
	};

	std::string_view RestResultStatusName(RestResultStatus a_status) noexcept;
	bool             RestResultSucceeded(const RestResult& a_result) noexcept;
	json             RestResultToJson(const RestResult& a_result);

	struct RestCommand
	{
		RestKind                        kind = RestKind::kWait;
		int                             hours = 0;
		std::optional<std::string>      target;
		std::uint64_t                   operationId = 0;
		std::shared_ptr<DispatchGate>   gate;
		std::function<bool()>           markDispatching;
		std::function<bool()>           markQueued;
		std::function<void(RestResult)> complete;
	};

	struct RestSubmission
	{
		bool                      pending = false;
		std::optional<RestResult> result;
	};

	struct RestBackend
	{
		std::string                                implementation;
		bool                                       available = false;
		bool                                       runtimeValidated = false;
		std::function<RestSubmission(RestCommand)> dispatch;
		std::function<void()>                      shutdown;
	};

	struct RestOperationReservation
	{
		std::uint64_t operationId = 0;
		std::uint64_t actionCursor = 0;
	};

	// Adapter for the process-wide game-operation tracker. finish must be able to
	// terminally complete an operation from Dispatching, Queued, or Started;
	// wait/sleep enters Queued while SleepWaitMenu is pumping.
	struct RestOperationCoordinator
	{
		std::function<std::optional<RestOperationReservation>(
			RestKind, int, const std::optional<std::string>&)>
															  tryBegin;
		std::function<bool(std::uint64_t)>                    markDispatching;
		std::function<bool(std::uint64_t)>                    markQueued;
		std::function<bool(std::uint64_t, const RestResult&)> finish;
		std::function<bool(std::uint64_t, std::string)>       fail;
		std::function<bool(std::uint64_t, std::string)>       cancelReserved;
	};

	struct RestServiceConfig
	{
		std::chrono::milliseconds listenerTimeout{ 30000 };
	};

	class RestService final : public std::enable_shared_from_this<RestService>
	{
	public:
		RestService(
			bool a_allowGameActions, RestBackend a_backend,
			RestOperationCoordinator a_operations,
			RestServiceConfig        a_config = {});
		~RestService();

		RestService(const RestService&) = delete;
		RestService& operator=(const RestService&) = delete;

		json Handle(RestKind a_kind, const json& a_args);
		void SetReady(bool a_ready) noexcept;
		void Shutdown() noexcept;

	private:
		struct State;
		std::shared_ptr<State> state_;
	};

	ToolDescriptor BuildWaitDescriptor();
	ToolDescriptor BuildSleepDescriptor();

	// The returned service owns operation status and the backend lifetime. Keep it
	// alive until shutdown, and call Shutdown before destroying the registry.
	std::shared_ptr<RestService> RegisterRestTools(
		ToolRegistry& a_registry, bool a_allowGameActions, RestBackend a_backend,
		RestOperationCoordinator a_operations,
		RestServiceConfig        a_config = {});
}

#pragma once

#include "KeyboardInputState.h"
#include "ToolRegistry.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace dvb
{
	class EventBus;
}

namespace dvb::tools::input
{
	inline constexpr int         kKeyboardContractVersion = 1;
	inline constexpr int         kDefaultTapMs = 50;
	inline constexpr int         kDefaultMaxHoldMs = 5000;
	inline constexpr int         kMaximumMaxHoldMs = 60000;
	inline constexpr std::size_t kMaximumHeldKeys = 8;
	inline constexpr std::size_t kMaximumSequenceEvents = 128;
	inline constexpr int         kMaximumSequenceMs = 30000;

	enum class DispatchStatus
	{
		kApplied,
		kUnknown,
		kCancelled,
		kPhysicalConflict,
		kQueueFull,
		kUnavailable,
		kFailed
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
			kFinished
		};
		std::atomic<State> state_{ State::kQueued };
	};

	struct DispatchResult
	{
		DispatchStatus status = DispatchStatus::kFailed;
		int            frame = -1;
		bool           physicalKeyDown = false;
		std::string    message;
	};

	struct ButtonCommand
	{
		KeyboardKey                         key;
		bool                                down = false;
		float                               heldSecs = 0.0F;
		std::shared_ptr<DispatchGate>       gate;
		std::function<void(DispatchResult)> complete;
	};

	struct DispatchSubmission
	{
		bool                          pending = false;
		std::optional<DispatchResult> result;
	};

	struct KeyboardBackend
	{
		std::string                                      injection;
		bool                                             available = false;
		std::function<DispatchSubmission(ButtonCommand)> dispatch;
	};

	class InputService final : public std::enable_shared_from_this<InputService>
	{
	public:
		InputService(EventBus& a_events, bool a_allowControlActions, KeyboardBackend a_backend);
		~InputService();

		InputService(const InputService&) = delete;
		InputService& operator=(const InputService&) = delete;

		json Handle(const json& a_args, const ToolContext& a_context);
		void SetReady(bool a_ready) noexcept;
		void ReleaseForLifecycle(std::string a_reason) noexcept;
		void Shutdown() noexcept;

	private:
		struct State;
		std::shared_ptr<State> state_;
	};

	ToolDescriptor BuildInputDescriptor();

	// The returned service is the lifetime root. Store it after Server so its destructor
	// runs before the registry/EventBus, and call Shutdown before Server teardown.
	std::shared_ptr<InputService> RegisterInputTool(
		ToolRegistry& a_registry, EventBus& a_events, bool a_allowControlActions,
		KeyboardBackend a_backend);
}

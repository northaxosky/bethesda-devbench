#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace dvb::waits
{
	/// Blocks callers until the engine frame counter passes a value. One shared
	/// main-thread task samples the counter each frame and re-queues itself only
	/// while waiters exist; waiters sleep on a condition variable it signals.
	class FrameAwaiter
	{
	public:
		using FrameSource = std::function<int()>;                      ///< -1 when unresolved
		using Submitter = std::function<bool(std::function<void()>)>;  ///< runs the task on the main thread

		struct Result
		{
			int  frame = -1;
			bool advanced = false;
		};

		FrameAwaiter(FrameSource a_frame, Submitter a_submit);
		~FrameAwaiter();

		FrameAwaiter(const FrameAwaiter&) = delete;
		FrameAwaiter& operator=(const FrameAwaiter&) = delete;

		/// Returns once the frame exceeds a_after (advanced) or a_wait elapses.
		/// nullopt when the frame source is unresolved; throws std::runtime_error
		/// when the main thread cannot be reached.
		std::optional<Result> Wait(int a_after, std::chrono::milliseconds a_wait);

		/// Wakes every waiter and makes later waits return immediately.
		void Shutdown();

	private:
		struct State;
		std::shared_ptr<State> state_;
	};
}

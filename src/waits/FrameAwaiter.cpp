#include "FrameAwaiter.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace dvb::waits
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Delay before re-arming a pump that stopped itself; one frame at 60 Hz.
		constexpr std::chrono::milliseconds kRearmDelay{ 16 };
	}

	struct FrameAwaiter::State : std::enable_shared_from_this<State>
	{
		FrameSource             frame;
		Submitter               submit;
		std::mutex              mutex;
		std::condition_variable wake;
		int                     observed = -1;  ///< frame seen by the latest pump run
		int                     lastRun = -1;
		int                     waiters = 0;
		bool                    pumping = false;  ///< a pump task is queued or running
		bool                    stopping = false;
		Clock::time_point       rearmAt{};

		// Caller holds the lock.
		bool Queue()
		{
			return submit([self = shared_from_this()] { self->Pump(); });
		}

		// Caller holds the lock.
		bool Arm()
		{
			lastRun = -1;
			pumping = Queue();
			return pumping;
		}

		// Runs on the main thread, once per frame while waiters exist.
		void Pump()
		{
			const int       now = frame();
			std::lock_guard lock(mutex);
			// An unchanged frame means the queue drains within one frame; park instead
			// of spinning the main thread.
			const bool sameFrame = now == lastRun;
			lastRun = now;
			if (now >= 0)
				observed = now;
			if (!sameFrame)
				wake.notify_all();
			if (!stopping && waiters > 0 && !sameFrame && Queue())
				return;
			pumping = false;
			rearmAt = Clock::now() + kRearmDelay;
			wake.notify_all();
		}
	};

	FrameAwaiter::FrameAwaiter(FrameSource a_frame, Submitter a_submit) :
		state_(std::make_shared<State>())
	{
		state_->frame = std::move(a_frame);
		state_->submit = std::move(a_submit);
	}

	FrameAwaiter::~FrameAwaiter()
	{
		Shutdown();
	}

	void FrameAwaiter::Shutdown()
	{
		{
			std::lock_guard lock(state_->mutex);
			state_->stopping = true;
		}
		state_->wake.notify_all();
	}

	std::optional<FrameAwaiter::Result> FrameAwaiter::Wait(int a_after, std::chrono::milliseconds a_wait)
	{
		auto&     s = *state_;
		const int start = s.frame();
		if (start < 0)
			return std::nullopt;
		if (start > a_after || a_wait.count() <= 0)
			return Result{ start, start > a_after };

		const auto       deadline = Clock::now() + a_wait;
		std::unique_lock lock(s.mutex);
		++s.waiters;
		struct Leave
		{
			State& s;
			~Leave() { --s.waiters; }
		} leave{ s };

		for (;;)
		{
			if (s.observed > a_after)
				return Result{ s.observed, true };
			const auto now = Clock::now();
			if (s.stopping || now >= deadline)
				break;
			auto until = deadline;
			if (!s.pumping)
			{
				if (now >= s.rearmAt)
				{
					if (!s.Arm())
						throw std::runtime_error("main thread task enqueue failed");
				}
				else
					until = std::min(deadline, s.rearmAt);
			}
			s.wake.wait_until(lock, until);
		}
		const int frame = s.frame();
		return Result{ frame, frame > a_after };
	}
}

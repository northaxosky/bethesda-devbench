#include "FrameAwaiter.h"

#include <algorithm>

namespace dvb::waits
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr std::chrono::milliseconds kFramePeriod{ 16 };
	}

	FrameAwaiter::FrameAwaiter(FrameSource a_frame) :
		frame_(std::move(a_frame))
	{}

	void FrameAwaiter::Shutdown()
	{
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
		}
		wake_.notify_all();
	}

	std::optional<FrameAwaiter::Result> FrameAwaiter::Wait(int a_after, std::chrono::milliseconds a_wait)
	{
		const auto deadline = Clock::now() + a_wait;
		int        frame = frame_();
		if (frame < 0)
			return std::nullopt;

		std::unique_lock lock(mutex_);
		// The frame counter has no event source, so sample it once per frame.
		while (frame <= a_after && !stopping_ && Clock::now() < deadline)
		{
			wake_.wait_until(lock, std::min(deadline, Clock::now() + kFramePeriod), [&] { return stopping_; });
			frame = frame_();
		}
		return Result{ frame, frame > a_after };
	}
}

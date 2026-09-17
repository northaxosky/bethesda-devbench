#include "StallWatchdog.h"

#include "Log.h"

#include <algorithm>
#include <stdexcept>

namespace dvb::tools::health
{
	FrameLiveness::FrameLiveness(std::chrono::milliseconds a_threshold) :
		threshold_(a_threshold)
	{
		if (threshold_.count() <= 0)
			throw std::invalid_argument("liveness threshold must be positive");
	}

	void FrameLiveness::Reset() noexcept
	{
		lastFrame_.reset();
		stalled_ = false;
	}

	std::optional<LivenessChange> FrameLiveness::Observe(int a_frame, Clock::time_point a_now)
	{
		if (a_frame < 0)
		{
			Reset();
			return std::nullopt;
		}
		if (!lastFrame_)
		{
			lastFrame_ = a_frame;
			lastAdvance_ = a_now;
			return std::nullopt;
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(a_now - lastAdvance_);
		if (a_frame != *lastFrame_)
		{
			const bool resumed = stalled_;
			lastFrame_ = a_frame;
			lastAdvance_ = a_now;
			stalled_ = false;
			if (resumed)
				return LivenessChange{ true, a_frame, elapsed };
		}
		else if (!stalled_ && elapsed >= threshold_)
		{
			stalled_ = true;
			return LivenessChange{ false, a_frame, elapsed };
		}
		return std::nullopt;
	}

	StallWatchdog::StallWatchdog(
		EventBus&                                 a_events,
		std::chrono::milliseconds                 a_threshold,
		std::function<int()>                      a_frame,
		std::function<std::vector<std::string>()> a_menus)
	{
		if (!a_frame || !a_menus)
			throw std::invalid_argument("liveness callbacks are required");
		FrameLiveness detector(a_threshold);
		const auto    interval = std::clamp(a_threshold / 3,
			std::chrono::milliseconds(100), std::chrono::milliseconds(1000));
		worker_ = std::jthread(
			[this, &a_events, detector, interval, frame = std::move(a_frame), menus = std::move(a_menus)](
				std::stop_token a_stop) mutable {
				while (!a_stop.stop_requested())
				{
					try
					{
						if (const auto change = detector.Observe(frame(), FrameLiveness::Clock::now()))
						{
							json payload{
								{ "frame", change->frame },
								{ "stalledForMs", change->stalledFor.count() },
							};
							if (!change->resumed)
							{
								const auto open = menus();
								if (!open.empty())
									payload["lastKnownOpenMenus"] = open;
							}
							a_events.Publish(change->resumed ? "health.resumed" : "health.stalled", std::move(payload));
						}
					}
					catch (const std::exception& a_error)
					{
						detector.Reset();
						logs::error("devbench: liveness observation failed: {}", a_error.what());
					}
					std::unique_lock lock(mutex_);
					wake_.wait_for(lock, a_stop, interval, [] { return false; });
				}
			});
	}

	StallWatchdog::~StallWatchdog()
	{
		worker_.request_stop();
		wake_.notify_all();
		if (worker_.joinable())
			worker_.join();
	}
}

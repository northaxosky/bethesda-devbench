#pragma once

#include "EventBus.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace dvb::tools::health
{
	struct LivenessChange
	{
		bool                      resumed;
		int                       frame;
		std::chrono::milliseconds stalledFor;
	};

	class FrameLiveness
	{
	public:
		using Clock = std::chrono::steady_clock;

		explicit FrameLiveness(std::chrono::milliseconds a_threshold);
		std::optional<LivenessChange> Observe(int a_frame, Clock::time_point a_now);
		void                          Reset() noexcept;

	private:
		std::chrono::milliseconds threshold_;
		std::optional<int>        lastFrame_;
		Clock::time_point         lastAdvance_{};
		bool                      stalled_ = false;
	};

	class StallWatchdog
	{
	public:
		StallWatchdog(
			EventBus&                                 a_events,
			std::chrono::milliseconds                 a_threshold,
			std::function<int()>                      a_frame,
			std::function<std::vector<std::string>()> a_menus);
		~StallWatchdog();

		StallWatchdog(const StallWatchdog&) = delete;
		StallWatchdog& operator=(const StallWatchdog&) = delete;

	private:
		std::mutex                  mutex_;
		std::condition_variable_any wake_;
		std::jthread                worker_;
	};
}

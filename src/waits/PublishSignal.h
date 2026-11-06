#pragma once

#include "EventBus.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dvb::waits
{
	class PublishSignal
	{
	public:
		using Clock = std::chrono::steady_clock;

		explicit PublishSignal(EventBus& a_events, std::vector<std::string> a_topics = {});
		~PublishSignal();

		PublishSignal(const PublishSignal&) = delete;
		PublishSignal& operator=(const PublishSignal&) = delete;

		/// Sample before observing state so a later publish still ends the Wait.
		std::uint64_t Seen();

		/// False only on timeout; a_extra runs under the signal's lock.
		bool Wait(std::uint64_t a_seen, Clock::time_point a_until, const std::function<bool()>& a_extra = {});

		/// Re-runs a_extra; call without holding a lock a_extra takes.
		void Notify();

		void Shutdown();
		bool Stopped();

	private:
		EventBus&               events_;
		EventBus::SubId         subscription_;
		std::mutex              mutex_;
		std::condition_variable wake_;
		std::uint64_t           generation_ = 0;
		bool                    stopping_ = false;
	};
}

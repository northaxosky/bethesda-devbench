#pragma once

#include "EventBus.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dvb::waits
{
	/// Blocking read over the bus ring: returns as soon as an event newer than a
	/// cursor is published, so clients never poll. Waiters are woken by a bus
	/// subscription, never by a timer slice.
	class EventAwaiter
	{
	public:
		struct Result
		{
			std::uint64_t                headSeq = 0;
			std::uint64_t                oldestSeq = 1;  ///< headSeq + 1 when the ring is empty
			std::vector<EventBus::Event> events;         ///< seq > the requested cursor
		};

		explicit EventAwaiter(EventBus& a_events);
		~EventAwaiter();

		EventAwaiter(const EventAwaiter&) = delete;
		EventAwaiter& operator=(const EventAwaiter&) = delete;

		/// Blocks until an event with seq > a_since exists, a_wait elapses, or
		/// Shutdown() is called. Zero wait is a plain snapshot.
		Result Wait(std::uint64_t a_since, std::chrono::milliseconds a_wait);

		/// Wakes every current waiter and makes later waits return immediately.
		void Shutdown();

	private:
		EventBus&               events_;
		EventBus::SubId         subscription_;
		std::mutex              mutex_;
		std::condition_variable wake_;
		std::uint64_t           published_ = 0;
		bool                    stopping_ = false;
	};
}

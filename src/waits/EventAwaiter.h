#pragma once

#include "EventBus.h"
#include "PublishSignal.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace dvb::waits
{
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

		EventAwaiter(const EventAwaiter&) = delete;
		EventAwaiter& operator=(const EventAwaiter&) = delete;

		Result Wait(std::uint64_t a_since, std::chrono::milliseconds a_wait);

		void Shutdown();

	private:
		EventBus&     events_;
		PublishSignal published_;
	};
}

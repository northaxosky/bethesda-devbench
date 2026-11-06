#include "EventAwaiter.h"

namespace dvb::waits
{
	EventAwaiter::EventAwaiter(EventBus& a_events) :
		events_(a_events), published_(a_events)
	{}

	void EventAwaiter::Shutdown()
	{
		published_.Shutdown();
	}

	EventAwaiter::Result EventAwaiter::Wait(std::uint64_t a_since, std::chrono::milliseconds a_wait)
	{
		const auto deadline = std::chrono::steady_clock::now() + a_wait;
		for (;;)
		{
			const auto seen = published_.Seen();
			if (events_.HeadSeq() > a_since)
				break;
			if (!published_.Wait(seen, deadline) || published_.Stopped())
				break;
		}

		Result result;
		auto   ring = events_.Since(0);
		result.headSeq = ring.empty() ? events_.HeadSeq() : ring.back().seq;
		result.oldestSeq = ring.empty() ? result.headSeq + 1 : ring.front().seq;
		for (auto& event : ring)
			if (event.seq > a_since)
				result.events.push_back(std::move(event));
		return result;
	}
}

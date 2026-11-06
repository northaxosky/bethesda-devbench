#include "EventAwaiter.h"

namespace dvb::waits
{
	EventAwaiter::EventAwaiter(EventBus& a_events) :
		events_(a_events)
	{
		subscription_ = events_.Subscribe([this](const EventBus::Event&) {
			{
				std::lock_guard lock(mutex_);
				++published_;
			}
			wake_.notify_all();
		});
	}

	EventAwaiter::~EventAwaiter()
	{
		Shutdown();
		events_.Unsubscribe(subscription_);
	}

	void EventAwaiter::Shutdown()
	{
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
		}
		wake_.notify_all();
	}

	EventAwaiter::Result EventAwaiter::Wait(std::uint64_t a_since, std::chrono::milliseconds a_wait)
	{
		const auto deadline = std::chrono::steady_clock::now() + a_wait;
		for (;;)
		{
			// Sampled before the head check: a publish landing after it bumps the counter.
			std::uint64_t seen;
			{
				std::lock_guard lock(mutex_);
				seen = published_;
			}
			if (events_.HeadSeq() > a_since)
				break;
			std::unique_lock lock(mutex_);
			if (!wake_.wait_until(lock, deadline, [&] { return stopping_ || published_ != seen; }))
				break;
			if (stopping_)
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

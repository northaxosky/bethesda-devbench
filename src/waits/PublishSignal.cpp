#include "PublishSignal.h"

#include <algorithm>

namespace dvb::waits
{
	PublishSignal::PublishSignal(EventBus& a_events, std::vector<std::string> a_topics) :
		events_(a_events)
	{
		subscription_ = events_.Subscribe([this, topics = std::move(a_topics)](const EventBus::Event& a_event) {
			if (!topics.empty() && std::ranges::find(topics, a_event.topic) == topics.end())
				return;
			{
				std::lock_guard lock(mutex_);
				++generation_;
			}
			wake_.notify_all();
		});
	}

	PublishSignal::~PublishSignal()
	{
		Shutdown();
		events_.Unsubscribe(subscription_);
	}

	std::uint64_t PublishSignal::Seen()
	{
		std::lock_guard lock(mutex_);
		return generation_;
	}

	bool PublishSignal::Wait(std::uint64_t a_seen, Clock::time_point a_until, const std::function<bool()>& a_extra)
	{
		std::unique_lock lock(mutex_);
		return wake_.wait_until(lock, a_until, [&] {
			return stopping_ || generation_ != a_seen || (a_extra && a_extra());
		});
	}

	void PublishSignal::Notify()
	{
		{
			std::lock_guard lock(mutex_);
		}
		wake_.notify_all();
	}

	void PublishSignal::Shutdown()
	{
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
		}
		wake_.notify_all();
	}

	bool PublishSignal::Stopped()
	{
		std::lock_guard lock(mutex_);
		return stopping_;
	}
}

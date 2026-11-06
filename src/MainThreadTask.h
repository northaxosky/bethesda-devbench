#pragma once

#include "Json.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <utility>

namespace dvb::MainThread
{
	class QueuedTask
	{
	public:
		using Clock = std::chrono::steady_clock;

		QueuedTask(std::function<json()> a_fn, Clock::time_point a_deadline) :
			fn_(std::move(a_fn)), deadline_(a_deadline)
		{}

		std::future<json> GetFuture()
		{
			return promise_.get_future();
		}

		bool Abandon()
		{
			auto expected = State::kQueued;
			return state_.compare_exchange_strong(expected, State::kAbandoned) ||
			       expected == State::kAbandoned;
		}

		void Run(Clock::time_point a_now = Clock::now())
		{
			if (a_now >= deadline_)
			{
				Abandon();
				return;
			}
			auto expected = State::kQueued;
			if (!state_.compare_exchange_strong(expected, State::kRunning))
				return;
			try
			{
				promise_.set_value(fn_());
			}
			catch (...)
			{
				try
				{
					promise_.set_exception(std::current_exception());
				}
				catch (...)
				{}
			}
		}

	private:
		enum class State
		{
			kQueued,
			kRunning,
			kAbandoned,
		};

		std::function<json()> fn_;
		Clock::time_point     deadline_;
		std::promise<json>    promise_;
		std::atomic<State>    state_{ State::kQueued };
	};
}

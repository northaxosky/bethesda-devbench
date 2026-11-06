#include "test_framework.h"

#include "waits/EventAwaiter.h"

#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;
using dvb::EventBus;
using dvb::json;
using dvb::waits::EventAwaiter;

namespace
{
	using Clock = std::chrono::steady_clock;
}

TEST_CASE("await events wakes a long waiter as soon as an event is published")
{
	EventBus     events;
	EventAwaiter awaiter(events);
	events.Publish("seed", json::object());

	const auto started = Clock::now();
	auto       waiter = std::async(std::launch::async, [&] { return awaiter.Wait(events.HeadSeq(), 20s); });
	std::this_thread::sleep_for(50ms);
	events.Publish("target", json{ { "value", 7 } });
	const auto result = waiter.get();

	CHECK(Clock::now() - started < 5s);
	CHECK(result.events.size() == 1);
	CHECK(result.events.size() == 1 && result.events[0].topic == "target");
	CHECK(result.headSeq == 2);
	CHECK(result.oldestSeq == 1);
}

TEST_CASE("await events returns the head after waitMs when nothing is published")
{
	EventBus     events;
	EventAwaiter awaiter(events);

	const auto started = Clock::now();
	const auto result = awaiter.Wait(0, 100ms);
	CHECK(Clock::now() - started >= 90ms);
	CHECK(result.events.empty());
	CHECK(result.headSeq == 0);
	CHECK(result.oldestSeq == 1);
}

TEST_CASE("await events waiters are released promptly by shutdown")
{
	EventBus     events;
	EventAwaiter awaiter(events);

	const auto started = Clock::now();
	auto       waiter = std::async(std::launch::async, [&] { return awaiter.Wait(0, 20s); });
	std::this_thread::sleep_for(50ms);
	awaiter.Shutdown();
	const auto result = waiter.get();

	CHECK(Clock::now() - started < 5s);
	CHECK(result.events.empty());
}

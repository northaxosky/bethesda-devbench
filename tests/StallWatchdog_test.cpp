#include "test_framework.h"

#include "tools/health/StallWatchdog.h"

using namespace std::chrono_literals;
using dvb::tools::health::FrameLiveness;

TEST_CASE("liveness emits one stall at the threshold and one recovery on frame progress")
{
	FrameLiveness detector(5000ms);
	const auto    start = FrameLiveness::Clock::time_point{};
	CHECK(!detector.Observe(10, start));
	CHECK(!detector.Observe(10, start + 4999ms));
	const auto stalled = detector.Observe(10, start + 5000ms);
	CHECK(stalled && !stalled->resumed);
	CHECK(stalled && stalled->stalledFor == 5000ms);
	CHECK(!detector.Observe(10, start + 6000ms));
	const auto resumed = detector.Observe(11, start + 6500ms);
	CHECK(resumed && resumed->resumed);
	CHECK(resumed && resumed->stalledFor == 6500ms);
	CHECK(!detector.Observe(12, start + 7000ms));
}

TEST_CASE("unavailable frame evidence cannot accumulate into a false stall")
{
	FrameLiveness detector(1000ms);
	const auto    start = FrameLiveness::Clock::time_point{};
	CHECK(!detector.Observe(-1, start));
	CHECK(!detector.Observe(-1, start + 1h));
	CHECK(!detector.Observe(20, start + 1h));
	CHECK(!detector.Observe(20, start + 1h + 999ms));
	CHECK(!detector.Observe(-1, start + 1h + 1000ms));
	CHECK(!detector.Observe(20, start + 2h));
	CHECK(!detector.Observe(21, start + 2h + 500ms));
	CHECK(!detector.Observe(21, start + 2h + 1499ms));
	CHECK(detector.Observe(21, start + 2h + 1500ms).has_value());
}

TEST_CASE("liveness treats a wrapping or reset generation as progress rather than a hang")
{
	FrameLiveness detector(1000ms);
	const auto    start = FrameLiveness::Clock::time_point{};
	CHECK(!detector.Observe(100, start));
	CHECK(!detector.Observe(0, start + 1500ms));
	CHECK(!detector.Observe(0, start + 2499ms));
	CHECK(detector.Observe(0, start + 2500ms).has_value());
}

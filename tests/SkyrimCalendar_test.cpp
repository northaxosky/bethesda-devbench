#include "test_framework.h"

#include "game/skyrimse/save/SkyrimCalendar.h"

#include <cmath>

namespace
{
	bool Near(double a_left, double a_right)
	{
		return std::abs(a_left - a_right) < 1e-9;
	}
}

TEST_CASE("Skyrim calendar advances across zero-based month boundary")
{
	const auto result = dvb::skyrimse::save::AdvanceSkyrimCalendar(
		{
			.year = 201,
			.month = 9,
			.day = 31,
			.hour = 23.0,
			.daysPassed = 100.0 + 23.0 / 24.0,
			.rawDaysPassed = 100.0,
		},
		2.0);
	CHECK(result.value.has_value());
	CHECK(result.value->dayDelta == 1);
	CHECK(result.value->state.year == 201);
	CHECK(result.value->state.month == 10);
	CHECK(result.value->state.day == 1);
	CHECK(Near(result.value->state.hour, 1.0));
	CHECK(Near(result.value->state.daysPassed, 101.0 + 1.0 / 24.0));
	CHECK(Near(result.value->state.rawDaysPassed, 101.0));
}

TEST_CASE("Skyrim calendar rewinds across year and preserves accumulators")
{
	const auto result = dvb::skyrimse::save::AdvanceSkyrimCalendar(
		{
			.year = 201,
			.month = 0,
			.day = 1,
			.hour = 1.0,
			.daysPassed = 100.0 + 1.0 / 24.0,
			.rawDaysPassed = 100.0,
		},
		-2.0);
	CHECK(result.value.has_value());
	CHECK(result.value->dayDelta == -1);
	CHECK(result.value->state.year == 200);
	CHECK(result.value->state.month == 11);
	CHECK(result.value->state.day == 31);
	CHECK(Near(result.value->state.hour, 23.0));
	CHECK(Near(result.value->state.daysPassed, 99.0 + 23.0 / 24.0));
	CHECK(Near(result.value->state.rawDaysPassed, 99.0));
}

TEST_CASE("Skyrim calendar applies partial-day advances to game time")
{
	const auto result = dvb::skyrimse::save::AdvanceSkyrimCalendar(
		{
			.year = 201,
			.month = 1,
			.day = 28,
			.hour = 12.0,
			.daysPassed = 50.5,
			.rawDaysPassed = 50.0,
		},
		6.0);
	CHECK(result.value.has_value());
	CHECK(result.value->dayDelta == 0);
	CHECK(result.value->state.month == 1);
	CHECK(result.value->state.day == 28);
	CHECK(Near(result.value->state.hour, 18.0));
	CHECK(Near(result.value->state.daysPassed, 50.75));
	CHECK(Near(result.value->state.rawDaysPassed, 50.0));
}

TEST_CASE("Skyrim calendar rejects invalid state and unbounded advances")
{
	auto invalid = dvb::skyrimse::save::AdvanceSkyrimCalendar(
		{ .year = 201, .month = 12, .day = 1, .hour = 12.0 }, 1.0);
	CHECK(!invalid.value.has_value());
	CHECK(invalid.error.find("date") != std::string::npos);

	CHECK(!dvb::skyrimse::save::AdvanceSkyrimCalendar(
		{ .year = 201, .month = 0, .day = 1, .hour = 12.0 }, 0.0)
			.value.has_value());
	CHECK(!dvb::skyrimse::save::AdvanceSkyrimCalendar(
		{ .year = 201, .month = 0, .day = 1, .hour = 12.0 }, 100001.0)
			.value.has_value());
}

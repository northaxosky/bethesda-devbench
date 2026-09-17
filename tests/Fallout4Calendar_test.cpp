#include "test_framework.h"

#include "tools/game/Fallout4Calendar.h"

#include <cmath>

namespace
{
	bool Near(double a_left, double a_right)
	{
		return std::abs(a_left - a_right) < 1e-9;
	}
}

TEST_CASE("Fallout 4 calendar advances across month using one-based months")
{
	const auto result = dvb::tools::game::AdvanceFallout4Calendar(
		{
			.rawYear = 287,
			.month = 10,
			.day = 31,
			.hour = 23.0,
			.daysPassed = 100.0 + 23.0 / 24.0,
			.rawDaysPassed = 100.0,
		},
		2.0);
	CHECK(result.value.has_value());
	CHECK(result.value->dayDelta == 1);
	CHECK(result.value->state.rawYear == 287);
	CHECK(dvb::tools::game::Fallout4DisplayYear(result.value->state.rawYear) == 2287);
	CHECK(result.value->state.month == 11);
	CHECK(result.value->state.day == 1);
	CHECK(Near(result.value->state.hour, 1.0));
	CHECK(Near(result.value->state.daysPassed, 101.0 + 1.0 / 24.0));
	CHECK(Near(
		result.value->state.rawDaysPassed,
		result.value->state.daysPassed - result.value->state.hour / 24.0));
}

TEST_CASE("Fallout 4 calendar rewinds across year and preserves day accumulator relation")
{
	const auto result = dvb::tools::game::AdvanceFallout4Calendar(
		{
			.rawYear = 287,
			.month = 1,
			.day = 1,
			.hour = 1.0,
			.daysPassed = 100.0 + 1.0 / 24.0,
			.rawDaysPassed = 100.0,
		},
		-2.0);
	CHECK(result.value.has_value());
	CHECK(result.value->dayDelta == -1);
	CHECK(result.value->state.rawYear == 286);
	CHECK(dvb::tools::game::Fallout4DisplayYear(result.value->state.rawYear) == 2286);
	CHECK(result.value->state.month == 12);
	CHECK(result.value->state.day == 31);
	CHECK(Near(result.value->state.hour, 23.0));
	CHECK(Near(result.value->state.daysPassed, 99.0 + 23.0 / 24.0));
	CHECK(Near(result.value->state.rawDaysPassed, 99.0));
}

TEST_CASE("Fallout 4 calendar uses the engine fixed February length")
{
	const auto result = dvb::tools::game::AdvanceFallout4Calendar(
		{
			.rawYear = 288,
			.month = 2,
			.day = 28,
			.hour = 12.0,
			.daysPassed = 50.5,
			.rawDaysPassed = 50.0,
		},
		24.0);
	CHECK(result.value.has_value());
	CHECK(result.value->state.rawYear == 288);
	CHECK(result.value->state.month == 3);
	CHECK(result.value->state.day == 1);
	CHECK(Near(result.value->state.hour, 12.0));
}

TEST_CASE("Fallout 4 calendar rejects invalid state and unbounded advances")
{
	auto invalid = dvb::tools::game::AdvanceFallout4Calendar(
		{ .rawYear = 287, .month = 0, .day = 1, .hour = 12.0 }, 1.0);
	CHECK(!invalid.value.has_value());
	CHECK(invalid.error.find("date") != std::string::npos);

	CHECK(!dvb::tools::game::AdvanceFallout4Calendar(
		{ .rawYear = 287, .month = 1, .day = 1, .hour = 12.0 }, 0.0)
			.value.has_value());
	CHECK(!dvb::tools::game::AdvanceFallout4Calendar(
		{ .rawYear = 287, .month = 1, .day = 1, .hour = 12.0 }, 100001.0)
			.value.has_value());
}

TEST_CASE("Fallout 4 display year follows native decimal prefix formatting")
{
	CHECK(dvb::tools::game::Fallout4DisplayYear(287) == 2287);
	CHECK(dvb::tools::game::Fallout4DisplayYear(99) == 299);
	CHECK(!dvb::tools::game::Fallout4DisplayYear(-1).has_value());
}

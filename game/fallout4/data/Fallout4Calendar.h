#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dvb::tools::game
{
	inline constexpr double kMaxCalendarAdvanceHours = 100000.0;

	struct Fallout4CalendarState
	{
		std::int32_t rawYear = 0;
		std::int32_t month = 1;
		std::int32_t day = 1;
		double       hour = 0.0;
		double       daysPassed = 0.0;
		double       rawDaysPassed = 0.0;
	};

	struct Fallout4CalendarAdvance
	{
		Fallout4CalendarState state;
		std::int32_t          dayDelta = 0;
	};

	struct Fallout4CalendarAdvanceResult
	{
		std::optional<Fallout4CalendarAdvance> value;
		std::string                            error;
	};

	[[nodiscard]] std::int32_t                  Fallout4DaysInMonth(std::int32_t a_month);
	[[nodiscard]] std::optional<std::int64_t>   Fallout4DisplayYear(std::int32_t a_rawYear);
	[[nodiscard]] Fallout4CalendarAdvanceResult AdvanceFallout4Calendar(
		const Fallout4CalendarState& a_state, double a_hours);
}

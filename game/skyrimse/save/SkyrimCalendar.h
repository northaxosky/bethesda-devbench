#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dvb::skyrimse::save
{
	inline constexpr double kMaxSkyrimCalendarAdvanceHours = 100000.0;

	struct SkyrimCalendarState
	{
		std::int32_t year = 0;
		std::int32_t month = 0;
		std::int32_t day = 1;
		double       hour = 0.0;
		double       daysPassed = 0.0;
		double       rawDaysPassed = 0.0;
		double       hoursPerDay = 24.0;
	};

	struct SkyrimCalendarAdvance
	{
		SkyrimCalendarState state;
		std::int32_t        dayDelta = 0;
	};

	struct SkyrimCalendarAdvanceResult
	{
		std::optional<SkyrimCalendarAdvance> value;
		std::string                          error;
	};

	[[nodiscard]] std::int32_t SkyrimDaysInMonth(std::int32_t a_month);
	[[nodiscard]] SkyrimCalendarAdvanceResult AdvanceSkyrimCalendar(
		const SkyrimCalendarState& a_state, double a_hours);
}

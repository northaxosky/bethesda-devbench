#include "Fallout4Calendar.h"

#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace dvb::tools::game
{
	namespace
	{
		// Fallout 4's Date::GetDaysInMonth table is indexed 1..12 and has no
		// leap-year branch. This differs from the zero-based Skyrim helper used
		// by upstream DevBench.
		constexpr std::array<std::int32_t, 12> kDaysInMonth{
			31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
		};

		Fallout4CalendarAdvanceResult Error(std::string a_error)
		{
			return { .error = std::move(a_error) };
		}
	}

	std::int32_t Fallout4DaysInMonth(std::int32_t a_month)
	{
		if (a_month < 1 || a_month > static_cast<std::int32_t>(kDaysInMonth.size()))
			return 0;
		return kDaysInMonth[static_cast<std::size_t>(a_month - 1)];
	}

	std::optional<std::int64_t> Fallout4DisplayYear(std::int32_t a_rawYear)
	{
		if (a_rawYear < 0)
			return std::nullopt;

		std::int64_t decimalPlace = 10;
		while (decimalPlace <= a_rawYear)
			decimalPlace *= 10;

		// Calendar::GetTimeDateString formats the raw global with literal "2%u".
		return 2 * decimalPlace + a_rawYear;
	}

	Fallout4CalendarAdvanceResult AdvanceFallout4Calendar(
		const Fallout4CalendarState& a_state, double a_hours)
	{
		if (!std::isfinite(a_hours) || a_hours == 0.0 ||
			std::abs(a_hours) > kMaxCalendarAdvanceHours)
			return Error("hours must be finite, non-zero, and within +/-100000");
		if (!std::isfinite(a_state.hour) || !std::isfinite(a_state.daysPassed) ||
			!std::isfinite(a_state.rawDaysPassed))
			return Error("calendar contains a non-finite value");
		if (a_state.hour < 0.0 || a_state.hour > 24.0)
			return Error("calendar hour is outside [0, 24]");
		const auto daysInCurrentMonth = Fallout4DaysInMonth(a_state.month);
		if (daysInCurrentMonth == 0 || a_state.day < 1 || a_state.day > daysInCurrentMonth)
			return Error("calendar date is invalid");
		if (!Fallout4DisplayYear(a_state.rawYear))
			return Error("calendar raw year is invalid");

		const double totalHours = a_state.hour + a_hours;
		const double dayDeltaDouble = std::floor(totalHours / 24.0);
		if (dayDeltaDouble < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
			dayDeltaDouble > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
			return Error("calendar day delta is outside the supported range");
		const auto dayDelta = static_cast<std::int32_t>(dayDeltaDouble);
		double     newHour = totalHours - dayDeltaDouble * 24.0;
		if (newHour < 0.0 && newHour > -1e-9)
			newHour = 0.0;
		if (newHour >= 24.0 && newHour < 24.0 + 1e-9)
			newHour = 0.0;
		if (newHour < 0.0 || newHour >= 24.0)
			return Error("calendar hour normalization failed");

		auto rawYear = a_state.rawYear;
		auto month = a_state.month;
		auto day = a_state.day;
		if (dayDelta > 0)
		{
			auto remaining = dayDelta;
			while (remaining > 0)
			{
				const auto daysRemainingInMonth = Fallout4DaysInMonth(month) - day;
				if (remaining <= daysRemainingInMonth)
				{
					day += remaining;
					remaining = 0;
				}
				else
				{
					remaining -= daysRemainingInMonth + 1;
					day = 1;
					if (++month > 12)
					{
						month = 1;
						if (rawYear == std::numeric_limits<std::int32_t>::max())
							return Error("calendar year overflow");
						++rawYear;
					}
				}
			}
		}
		else if (dayDelta < 0)
		{
			auto remaining = -static_cast<std::int64_t>(dayDelta);
			while (remaining > 0)
			{
				const auto daysBefore = day - 1;
				if (remaining <= daysBefore)
				{
					day -= static_cast<std::int32_t>(remaining);
					remaining = 0;
				}
				else
				{
					remaining -= daysBefore + 1;
					if (--month < 1)
					{
						month = 12;
						if (rawYear == 0)
							return Error("calendar year underflow");
						--rawYear;
					}
					day = Fallout4DaysInMonth(month);
				}
			}
		}

		const double daysPassed = a_state.daysPassed + a_hours / 24.0;
		// Calendar::Update rewrites gameDaysPassed as rawDaysPassed + hour / 24.
		// Keep that FO4-specific relation intact so the next engine tick does not
		// undo the direct calendar jump.
		const double rawDaysPassed = daysPassed - newHour / 24.0;
		if (!std::isfinite(daysPassed) || !std::isfinite(rawDaysPassed))
			return Error("calendar day accumulator overflow");

		return {
			.value = Fallout4CalendarAdvance{
				.state = Fallout4CalendarState{
					.rawYear = rawYear,
					.month = month,
					.day = day,
					.hour = newHour,
					.daysPassed = daysPassed,
					.rawDaysPassed = rawDaysPassed,
				},
				.dayDelta = dayDelta,
			},
		};
	}
}

#include "SkyrimCalendar.h"

#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace dvb::skyrimse::save
{
	namespace
	{
		constexpr std::array<std::int32_t, 12> kDaysInMonth{
			31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
		};

		SkyrimCalendarAdvanceResult Error(std::string a_error)
		{
			return { .error = std::move(a_error) };
		}
	}

	std::int32_t SkyrimDaysInMonth(std::int32_t a_month)
	{
		if (a_month < 0 || a_month >= static_cast<std::int32_t>(kDaysInMonth.size()))
			return 0;
		return kDaysInMonth[static_cast<std::size_t>(a_month)];
	}

	SkyrimCalendarAdvanceResult AdvanceSkyrimCalendar(
		const SkyrimCalendarState& a_state, double a_hours)
	{
		if (!std::isfinite(a_hours) || a_hours == 0.0 ||
			std::abs(a_hours) > kMaxSkyrimCalendarAdvanceHours)
			return Error("hours must be finite, non-zero, and within +/-100000");
		if (!std::isfinite(a_state.hour) || !std::isfinite(a_state.daysPassed) ||
			!std::isfinite(a_state.rawDaysPassed) || !std::isfinite(a_state.hoursPerDay))
			return Error("calendar contains a non-finite value");
		if (a_state.hoursPerDay <= 0.0)
			return Error("calendar hours-per-day must be positive");
		if (a_state.hour < 0.0 || a_state.hour > a_state.hoursPerDay)
			return Error("calendar hour is outside the supported day");
		const auto daysInCurrentMonth = SkyrimDaysInMonth(a_state.month);
		if (daysInCurrentMonth == 0 || a_state.day < 1 || a_state.day > daysInCurrentMonth)
			return Error("calendar date is invalid");

		const double totalHours = a_state.hour + a_hours;
		const double dayDeltaDouble = std::floor(totalHours / a_state.hoursPerDay);
		if (dayDeltaDouble < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
			dayDeltaDouble > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
			return Error("calendar day delta is outside the supported range");
		const auto dayDelta = static_cast<std::int32_t>(dayDeltaDouble);
		double     newHour = totalHours - dayDeltaDouble * a_state.hoursPerDay;
		if (newHour < 0.0 && newHour > -1e-9)
			newHour = 0.0;
		if (newHour >= a_state.hoursPerDay &&
			newHour < a_state.hoursPerDay + 1e-9)
			newHour = 0.0;
		if (newHour < 0.0 || newHour >= a_state.hoursPerDay)
			return Error("calendar hour normalization failed");

		auto year = a_state.year;
		auto month = a_state.month;
		auto day = a_state.day;
		if (dayDelta > 0)
		{
			auto remaining = dayDelta;
			while (remaining > 0)
			{
				const auto daysRemainingInMonth = SkyrimDaysInMonth(month) - day;
				if (remaining <= daysRemainingInMonth)
				{
					day += remaining;
					remaining = 0;
				}
				else
				{
					remaining -= daysRemainingInMonth + 1;
					day = 1;
					if (++month == 12)
					{
						month = 0;
						if (year == std::numeric_limits<std::int32_t>::max())
							return Error("calendar year overflow");
						++year;
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
					if (--month < 0)
					{
						month = 11;
						if (year == std::numeric_limits<std::int32_t>::min())
							return Error("calendar year underflow");
						--year;
					}
					day = SkyrimDaysInMonth(month);
				}
			}
		}

		const double daysPassed = a_state.daysPassed + a_hours / a_state.hoursPerDay;
		const double rawDaysPassed = daysPassed - newHour / a_state.hoursPerDay;
		if (!std::isfinite(daysPassed) || !std::isfinite(rawDaysPassed))
			return Error("calendar day accumulator overflow");

		return {
			.value = SkyrimCalendarAdvance{
				.state = SkyrimCalendarState{
					.year = year,
					.month = month,
					.day = day,
					.hour = newHour,
					.daysPassed = daysPassed,
					.rawDaysPassed = rawDaysPassed,
					.hoursPerDay = a_state.hoursPerDay,
				},
				.dayDelta = dayDelta,
			},
		};
	}
}

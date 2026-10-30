#pragma once

#include <cstdint>

namespace dvb::platform
{
	struct ThrottlingResult
	{
		bool          ok = false;
		std::uint32_t error = 0;  ///< GetLastError when !ok
	};

	enum class ForegroundOutcome
	{
		kAlreadyForeground,
		kBroughtToFront,
		kDenied,
		kInvalidWindow,
	};

	struct ForegroundResult
	{
		ForegroundOutcome outcome = ForegroundOutcome::kInvalidWindow;
		std::uint32_t     error = 0;

		[[nodiscard]] bool ok() const noexcept
		{
			return outcome == ForegroundOutcome::kAlreadyForeground ||
			       outcome == ForegroundOutcome::kBroughtToFront;
		}
	};

	// Opts the process out of EcoQoS execution-speed and timer-resolution throttling.
	ThrottlingResult DisablePowerThrottling() noexcept;

	// Never synthesizes input. Call from the thread that owns a_window.
	ForegroundResult BringToForeground(void* a_window) noexcept;

	bool IsForegroundOwnedByCurrentProcess() noexcept;

	// First visible, unowned top-level window of this process, or null.
	void* FindVisibleTopLevelWindow() noexcept;

	const char* ToString(ForegroundOutcome a_outcome) noexcept;
}

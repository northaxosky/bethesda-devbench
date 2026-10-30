#include "platform/ProcessWindow.h"

#include <Windows.h>

namespace dvb::platform
{
	namespace
	{
		struct WindowSearch
		{
			DWORD pid = 0;
			HWND  found = nullptr;
		};

		BOOL CALLBACK MatchWindow(HWND a_window, LPARAM a_param)
		{
			auto& search = *reinterpret_cast<WindowSearch*>(a_param);
			DWORD pid = 0;
			::GetWindowThreadProcessId(a_window, &pid);
			if (pid != search.pid || !::IsWindowVisible(a_window) ||
				::GetWindow(a_window, GW_OWNER) != nullptr)
				return TRUE;
			search.found = a_window;
			return FALSE;
		}
	}

	ThrottlingResult DisablePowerThrottling() noexcept
	{
		PROCESS_POWER_THROTTLING_STATE state{};
		state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
		                    PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
		state.StateMask = 0;
		if (::SetProcessInformation(
				::GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)))
			return { true, 0 };
		return { false, ::GetLastError() };
	}

	ForegroundResult BringToForeground(void* a_window) noexcept
	{
		const auto window = static_cast<HWND>(a_window);
		if (!window || !::IsWindow(window))
			return { ForegroundOutcome::kInvalidWindow, ERROR_INVALID_WINDOW_HANDLE };

		if (::IsIconic(window))
			::ShowWindow(window, SW_RESTORE);
		if (::GetForegroundWindow() == window)
			return { ForegroundOutcome::kAlreadyForeground, 0 };

		const DWORD self = ::GetCurrentThreadId();
		const HWND  current = ::GetForegroundWindow();
		const DWORD foregroundThread =
			current ? ::GetWindowThreadProcessId(current, nullptr) : 0;
		const bool attached = foregroundThread != 0 && foregroundThread != self &&
		                      ::AttachThreadInput(foregroundThread, self, TRUE);

		::BringWindowToTop(window);
		const bool requested = ::SetForegroundWindow(window) != FALSE;
		DWORD      error = requested ? 0 : ::GetLastError();
		::SetActiveWindow(window);
		::SetFocus(window);

		if (attached)
			::AttachThreadInput(foregroundThread, self, FALSE);

		if (::GetForegroundWindow() == window)
			return { ForegroundOutcome::kBroughtToFront, 0 };
		return { ForegroundOutcome::kDenied, error };
	}

	bool IsWindowVisible(void* a_window) noexcept
	{
		return a_window && ::IsWindowVisible(static_cast<HWND>(a_window)) != FALSE;
	}

	bool IsForegroundOwnedByCurrentProcess() noexcept
	{
		DWORD      pid = 0;
		const HWND foreground = ::GetForegroundWindow();
		if (!foreground)
			return false;
		::GetWindowThreadProcessId(foreground, &pid);
		return pid == ::GetCurrentProcessId();
	}

	void* FindVisibleTopLevelWindow() noexcept
	{
		WindowSearch search{ ::GetCurrentProcessId(), nullptr };
		::EnumWindows(&MatchWindow, reinterpret_cast<LPARAM>(&search));
		return search.found;
	}

	const char* ToString(ForegroundOutcome a_outcome) noexcept
	{
		switch (a_outcome)
		{
			case ForegroundOutcome::kAlreadyForeground:
				return "alreadyForeground";
			case ForegroundOutcome::kBroughtToFront:
				return "broughtToFront";
			case ForegroundOutcome::kDenied:
				return "denied";
			case ForegroundOutcome::kInvalidWindow:
				return "invalidWindow";
		}
		return "unknown";
	}
}

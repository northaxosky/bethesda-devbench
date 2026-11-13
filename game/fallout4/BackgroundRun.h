#pragma once

#include <optional>

// Main-thread only, like any engine setting access.
namespace dvb::fallout4::BackgroundRun
{
	// Live [General] bAlwaysActive; nullopt when the setting is absent.
	std::optional<bool> AlwaysActive();

	// Sets the running engine's value, not the INI file; false when absent.
	bool SetAlwaysActive(bool a_enabled);
}

#pragma once

#include "Config.h"
#include "Json.h"

#include <cstdint>

namespace dvb::fallout4::WindowFocus
{
	// Applies the power-throttling opt-out and records the foreground policy.
	void Configure(const Config& a_config);

	// Requests foreground once, on a main-thread task, after the window is visible.
	void OnF4SEMessage(std::uint32_t a_type);

	// Small state object for inspect kind=state.
	json Snapshot();
}

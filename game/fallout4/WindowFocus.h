#pragma once

#include "Config.h"
#include "Json.h"

#include <cstdint>

namespace dvb::fallout4::WindowFocus
{
	void Configure(const Config& a_config);

	// Requests foreground once, on a main-thread task, after the window is visible.
	void OnF4SEMessage(std::uint32_t a_type);

	json Snapshot();
}

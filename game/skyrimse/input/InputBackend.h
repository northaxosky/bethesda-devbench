#pragma once

#include "tools/input/InputTool.h"

namespace dvb::skyrimse
{
	// Construction is engine-call-free. Native queue/device pointers are resolved only when
	// InputService dispatches after kInputLoaded.
	tools::input::InputBackend MakeInputBackend();
}

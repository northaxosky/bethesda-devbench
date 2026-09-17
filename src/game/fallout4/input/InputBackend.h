#pragma once

#include "tools/input/InputTool.h"

namespace dvb::fallout4
{
	// AE 1.11.240 only. Construction is engine-call-free; relocations and device
	// pointers are resolved only when a ready InputService dispatches a command.
	tools::input::KeyboardBackend MakeInputBackend();
}

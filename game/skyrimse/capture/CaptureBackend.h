#pragma once

#include "tools/capture/CaptureTool.h"

namespace dvb::skyrimse::capture
{
	tools::capture::CaptureBackend MakeCaptureBackend(
		std::function<json()> a_sceneSnapshot);
}

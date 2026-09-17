#pragma once

#include "tools/capture/CaptureTool.h"

namespace dvb::fallout4::capture
{
	// RuntimeGameDirectory supplies the process-visible game-root source path. Existing artifacts
	// are translated through io::PhysicalFilePath before they cross the API boundary, so MO2/USVFS
	// backing paths stay usable by external controllers.
	tools::capture::CaptureBackend MakeCaptureBackend(
		std::function<json()> a_sceneSnapshot);
}

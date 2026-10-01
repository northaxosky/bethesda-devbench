#pragma once

#include "tools/camera/CameraTool.h"

namespace dvb::skyrimse
{
	tools::CameraBackend MakeCameraBackend();

	// Main-thread lifecycle hooks used to invalidate queued VR camera work across loads.
	void CameraPreLoad() noexcept;
	void CameraPostLoad() noexcept;
	void ShutdownCamera() noexcept;
}

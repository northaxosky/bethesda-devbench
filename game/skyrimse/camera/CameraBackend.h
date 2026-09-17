#pragma once

#include "tools/camera/CameraTool.h"

#include <string>

namespace dvb::skyrimse
{
	tools::CameraBackend MakeCameraBackend(std::string a_runtimeVersion);

	// Main-thread lifecycle hooks used to invalidate queued VR camera work across loads.
	void CameraPreLoad() noexcept;
	void CameraPostLoad() noexcept;
	void ShutdownCamera() noexcept;
}

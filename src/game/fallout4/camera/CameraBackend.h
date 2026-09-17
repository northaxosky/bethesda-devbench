#pragma once

#include "tools/camera/CameraTool.h"

#include <string>

namespace dvb::fallout4
{
	tools::CameraBackend MakeCameraBackend(std::string a_runtimeVersion);
}

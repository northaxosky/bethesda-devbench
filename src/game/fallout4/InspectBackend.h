#pragma once

#include "tools/InspectTool.h"

#include <string>

namespace dvb::fallout4
{
	tools::InspectBackend MakeInspectBackend(std::string a_runtimeVersion);
}

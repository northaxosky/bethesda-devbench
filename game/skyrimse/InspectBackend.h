#pragma once

#include "tools/InspectTool.h"

#include <string>

namespace dvb::skyrimse
{
	tools::InspectBackend MakeInspectBackend(std::string a_runtimeVersion);
}

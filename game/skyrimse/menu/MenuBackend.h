#pragma once

#include "tools/menu/MenuTool.h"

#include <string>

namespace dvb::skyrimse
{
	tools::MenuBackend MakeMenuBackend(std::string a_runtimeVersion);
}

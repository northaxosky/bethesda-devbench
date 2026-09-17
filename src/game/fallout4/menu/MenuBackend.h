#pragma once

#include "tools/menu/MenuTool.h"

#include <string>

namespace dvb::fallout4
{
	tools::MenuBackend MakeMenuBackend(std::string a_runtimeVersion);
}

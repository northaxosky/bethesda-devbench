#pragma once

#include "Host.h"

namespace dvb::fallout4
{
	HostAdapter MakeHostAdapter(std::string a_runtimeVersion);
	int         CurrentFrame();
}

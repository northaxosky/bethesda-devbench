#pragma once

#include "Host.h"

namespace dvb::skyrimse
{
	GameProfile CurrentProfile();
	HostAdapter MakeHostAdapter(std::string a_runtimeVersion);
	int         CurrentFrame();
}

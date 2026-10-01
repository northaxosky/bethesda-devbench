#pragma once

#include "Host.h"

namespace dvb::skyrimse
{
	GameProfile CurrentProfile();
	HostAdapter MakeHostAdapter();
	int         CurrentFrame();
}

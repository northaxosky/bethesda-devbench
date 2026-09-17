#pragma once

#include "GameProfile.h"
#include "Json.h"

namespace dvb::tools
{
	json BuildCoreToolCatalog();
	json BuildCoreToolCatalog(const GameProfile& a_profile);
	json BuildCoreToolCatalogBundle();
}

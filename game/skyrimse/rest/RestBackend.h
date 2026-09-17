#pragma once

#include "tools/rest/RestTool.h"

namespace dvb::skyrimse
{
	// Uses Skyrim's real Sleep/Wait Menu and its loaded ActionScript/native OK
	// callback. The backend owns one polling worker; every engine observation and
	// dispatch is posted to SKSE's main-thread task interface.
	tools::rest::RestBackend              MakeRestBackend();
	tools::rest::RestOperationCoordinator MakeRestOperationCoordinator();
}

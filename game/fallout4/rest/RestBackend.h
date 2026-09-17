#pragma once

#include "tools/rest/RestTool.h"

namespace dvb::fallout4
{
	// AE 1.11.240 only. The returned backend owns its polling worker and queues all
	// engine access through F4SE's main-thread task interface.
	tools::rest::RestBackend              MakeRestBackend();
	tools::rest::RestOperationCoordinator MakeRestOperationCoordinator();
}

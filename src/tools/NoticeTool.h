#pragma once

#include "ToolRegistry.h"

namespace dvb::tools
{
	ToolDescriptor BuildNoticeDescriptor();
	void           RegisterNoticeTool(ToolRegistry& a_registry);
}

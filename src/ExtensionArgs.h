#pragma once

#include "ToolRegistry.h"

namespace dvb
{
	// Extensions declare their own types, so they get uncoerced args.
	[[nodiscard]] inline const json& ExtensionArgs(const json& a_args, const ToolContext& a_context)
	{
		return a_context.rawArgs ? *a_context.rawArgs : a_args;
	}
}

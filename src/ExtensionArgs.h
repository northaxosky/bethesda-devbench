#pragma once

#include "ToolRegistry.h"

namespace dvb
{
	/// What an extension handler receives: the caller's arguments as sent, not the
	/// base tool's schema-coerced copy, since the extension declares its own types.
	[[nodiscard]] inline const json& ExtensionArgs(const json& a_args, const ToolContext& a_context)
	{
		return a_context.rawArgs ? *a_context.rawArgs : a_args;
	}
}

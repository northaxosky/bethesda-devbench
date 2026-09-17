#pragma once

namespace dvb::tools
{
	enum class ToolPermission
	{
		kConsoleCommands,
		kGameActions,
		kControlActions,
		kPapyrusCalls,
	};

	void RequireToolPermission(bool a_allowed, ToolPermission a_permission);
}

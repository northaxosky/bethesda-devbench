#include "ToolPermissions.h"

#include "ToolRegistry.h"

namespace dvb::tools
{
	void RequireToolPermission(bool a_allowed, ToolPermission a_permission)
	{
		if (a_allowed)
			return;

		switch (a_permission)
		{
			case ToolPermission::kConsoleCommands:
				throw ToolError(403,
					"console command execution is disabled; set allowConsoleCommands=true to opt in");
			case ToolPermission::kGameActions:
				throw ToolError(403,
					"game save/load actions are disabled; set allowGameActions=true to opt in");
			case ToolPermission::kControlActions:
				throw ToolError(403,
					"synthetic control actions are disabled; set allowControlActions=true to opt in");
			case ToolPermission::kPapyrusCalls:
				throw ToolError(403,
					"Papyrus calls are disabled; set allowPapyrusCalls=true to opt in");
		}
	}
}

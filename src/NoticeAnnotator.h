#pragma once

namespace dvb
{
	class ToolRegistry;

	namespace Notices
	{
		// Makes every reply carry the active `notices` (see ToolRegistry::SetAnnotator).
		void AttachToReplies(ToolRegistry& a_registry);
	}
}

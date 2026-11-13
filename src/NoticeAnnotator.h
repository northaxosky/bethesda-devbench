#pragma once

#include <functional>
#include <string>
#include <vector>

namespace dvb
{
	class ToolRegistry;

	namespace Notices
	{
		// Where the annotator learns which menus are open. Runs on listener
		// threads, so openMenus must be a lock-light snapshot, not a main-thread call.
		struct ModalSource
		{
			std::function<std::vector<std::string>()> openMenus;
			std::vector<std::string>                  nonBlockingMenus;
		};

		// Adds `notices`, and `blockedBy` when a ModalSource is given, to replies.
		void AttachToReplies(ToolRegistry& a_registry, ModalSource a_modal = {});
	}
}

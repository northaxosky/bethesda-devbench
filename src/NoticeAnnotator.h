#pragma once

#include "ModalLogic.h"

namespace dvb
{
	class ToolRegistry;

	namespace Notices
	{
		// `blockedBy` is added only when a ModalSource is given.
		void AttachToReplies(ToolRegistry& a_registry, Modal::ModalSource a_modal = {});
	}
}

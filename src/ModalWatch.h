#pragma once

#include "EventBus.h"
#include "NoticeAnnotator.h"

namespace dvb::Modal
{
	// Publishes modal.blocked / modal.cleared from the game's `menu` events.
	// Returns the subscription for the caller to Unsubscribe.
	EventBus::SubId Watch(EventBus& a_events, Notices::ModalSource a_source);
}

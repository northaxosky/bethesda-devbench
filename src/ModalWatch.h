#pragma once

#include "EventBus.h"
#include "ModalLogic.h"

namespace dvb::Modal
{
	// The caller unsubscribes the returned id.
	EventBus::SubId Watch(EventBus& a_events, ModalSource a_source);
}

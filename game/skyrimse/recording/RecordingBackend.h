#pragma once

#include "tools/recording/RecordingTool.h"

namespace dvb
{
	class EventBus;
}

namespace RE
{
	class InputEvent;
}

namespace dvb::skyrimse::recording
{
	tools::recording::RecordingBackend MakeRecordingBackend();

	// InputService attaches the EventBus before kInputLoaded. The one Skyrim input sink serializes
	// each event while its chain is valid and publishes it through the shared recording seam.
	void AttachActivityEvents(EventBus& a_events);
	void DetachActivityEvents() noexcept;
	void NoteInputEvents(RE::InputEvent* const* a_events);
}

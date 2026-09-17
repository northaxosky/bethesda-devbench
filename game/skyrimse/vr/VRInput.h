#pragma once

#include "Json.h"

#include <string_view>

namespace dvb
{
	class EventBus;
	struct ToolContext;
}

namespace dvb::skyrimse::vrinput
{
	json VRInputCapabilities();
	json HandleVRInput(const json& a_args, const ToolContext& a_ctx);
	bool IsVRInputDevice(std::string_view a_device);
	json ObserveVRTrackedSet();

	// Installs process-lifetime pass-through OpenVR interface hooks. They alter data only while
	// one owned atomic sequence is active; otherwise every call reaches the original runtime.
	void AttachVRInputEvents(::dvb::EventBus& a_events);
	void SetVRInputReady(bool a_ready);
	void MarkVRInputReady(::dvb::EventBus& a_events);
	void ReleaseVRInputForLifecycle(const char* a_reason);
	void ShutdownVRInput() noexcept;
}

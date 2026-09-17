#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	// Provider side of the cross-plugin C-ABI (DevBenchAPI). Lets other xSE plugins
	// register tools / emit events into this host's registry and event bus.
	namespace HostApi
	{
		struct Message
		{
			std::uint32_t type = 0;
			std::uint32_t dataLength = 0;
			const void*   data = nullptr;
			const char*   sender = nullptr;
		};

		// Wire the host interface to the registry + bus before consumers request it.
		void Init(ToolRegistry& a_registry, EventBus& a_events);
		void Reset();

		// Handle a DevBenchMessage::kMessage_GetInterface request. Native adapters
		// translate their extender message into these ABI-neutral fields.
		void OnInterfaceRequest(const Message& a_message);

		// A plugin that requested the C-ABI interface (one entry per GetInterface call — a
		// plugin that calls it more than once appears more than once, oldest first). NOTE: the
		// requested revision is NOT captured here — GetApiFunction is a pointer the consumer
		// calls itself, later, on its own; devbench never observes that call or its argument,
		// only that the pointer was handed out. Reporting a revision here would be a guess
		// dressed up as an observation.
		struct Consumer
		{
			std::string   name;  // a_message->sender, or "<?>" if unset
			long long     atEpoch;
			std::uint32_t atFrame;
		};

		// A successful RegisterTool/RegisterToolExtension call over the C-ABI.
		struct Registration
		{
			std::string   kind;  // "tool" | "extension"
			std::string   name;  // tool name, or "<baseTool>:<key>"
			long long     atEpoch;
			std::uint32_t atFrame;
			bool          replaced;  // true if this call overwrote an earlier registration
		};

		// Every GetInterface request seen so far, oldest first. Thread-safe.
		std::vector<Consumer> Consumers();

		// Every successful tool/extension registration seen so far, oldest first. Thread-safe.
		// There is no reliable per-registration caller identity (the C-ABI interface is one
		// shared singleton), so this cannot be
		// joined against Consumers() by plugin name; both lists are exposed side by side instead.
		std::vector<Registration> Registrations();
	}
}

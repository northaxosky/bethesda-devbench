#pragma once

#include "EventBus.h"
#include "tools/ConsoleTool.h"

#include <memory>

namespace dvb::fallout4
{
	class ConsoleBackendLifetime
	{
	public:
		explicit ConsoleBackendLifetime(EventBus& a_events);
		~ConsoleBackendLifetime();

		ConsoleBackendLifetime(const ConsoleBackendLifetime&) = delete;
		ConsoleBackendLifetime& operator=(const ConsoleBackendLifetime&) = delete;
		ConsoleBackendLifetime(ConsoleBackendLifetime&&) = delete;
		ConsoleBackendLifetime& operator=(ConsoleBackendLifetime&&) = delete;

		tools::ConsoleBackend Backend() const;

	private:
		class Impl;
		std::shared_ptr<Impl> m_impl;
	};

	tools::ConsoleBackend MakeConsoleBackend();
}

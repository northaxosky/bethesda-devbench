#pragma once

#include "EventBus.h"
#include "tools/ConsoleTool.h"

#include <memory>

namespace dvb::skyrimse
{
	class ConsoleBackendLifetime
	{
	public:
		class Impl;

		explicit ConsoleBackendLifetime(EventBus& a_events);
		~ConsoleBackendLifetime();

		ConsoleBackendLifetime(const ConsoleBackendLifetime&) = delete;
		ConsoleBackendLifetime& operator=(const ConsoleBackendLifetime&) = delete;
		ConsoleBackendLifetime(ConsoleBackendLifetime&&) = delete;
		ConsoleBackendLifetime& operator=(ConsoleBackendLifetime&&) = delete;

		tools::ConsoleBackend Backend() const;

	private:
		std::shared_ptr<Impl> impl_;
	};

	tools::ConsoleBackend MakeConsoleBackend();
}

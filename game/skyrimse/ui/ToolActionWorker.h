#pragma once

#include "Json.h"

#include <string>

namespace dvb
{
	class ToolRegistry;
}

namespace dvb::skyrimse::ui
{
	void StartToolActionWorker(ToolRegistry& a_registry);
	bool QueueToolAction(
		std::string a_tool, json a_args, std::string a_clientId);
	bool QueueRecordToggle();
	void ShutdownToolActionWorker() noexcept;
}

#pragma once

#include "ToolRegistry.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::tools
{
	inline constexpr std::size_t kMaxConsoleCommandBytes = 1024;
	inline constexpr std::size_t kMaxConsoleCaptureLines = 200;
	inline constexpr std::size_t kMaxConsoleCaptures = 64;
	inline constexpr std::size_t kMaxPendingConsoleSubmissions = 64;

	struct ConsoleCaptureResult
	{
		bool                     sawBegin = false;
		bool                     sawEnd = false;
		bool                     truncated = false;
		std::vector<std::string> lines;
	};

	ConsoleCaptureResult ExtractConsoleCapture(
		std::string_view a_buffer,
		std::string_view a_beginMarker,
		std::string_view a_endMarker,
		std::size_t      a_maxLines = kMaxConsoleCaptureLines);

	struct ConsoleBackend
	{
		// Queue the supplied commands for execution on the game's main thread. The
		// completion callback must run after the submission lambda has invoked every
		// ExecuteCommand call (the engine may still drain those commands later).
		std::function<bool(std::vector<std::string>, std::function<void()>)> queueCommands;

		// Return a main-thread snapshot of the native console scrollback buffer.
		std::function<std::string()> readBuffer;
	};

	ToolDescriptor BuildConsoleDescriptor();
	void           RegisterConsoleTool(ToolRegistry& a_registry, bool a_allowCommands, ConsoleBackend a_backend);
}

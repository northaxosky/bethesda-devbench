#include "ConsoleTool.h"

#include "ToolPermissions.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace dvb::tools
{
	namespace
	{
		struct Capture
		{
			std::string id;
			std::string beginMarker;
			std::string endMarker;
		};

		struct State
		{
			std::mutex                               mutex;
			std::unordered_map<std::string, Capture> captures;
			std::deque<std::string>                  captureOrder;
			std::uint64_t                            nextCapture = 0;
			std::size_t                              pendingSubmissions = 0;
			const std::uint64_t                      captureSeed =
				static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		};

		void RequireObject(const json& a_args)
		{
			if (!a_args.is_object())
				throw ToolError(400, "console arguments must be an object");
		}

		std::string ReadString(const json& a_args, std::string_view a_name, bool a_required)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
			{
				if (a_required)
					throw ToolError(400, std::format("missing required parameter '{}'", a_name));
				return {};
			}
			if (!it->is_string())
				throw ToolError(400, std::format("'{}' must be a string", a_name));
			return it->get<std::string>();
		}

		bool ReadBool(const json& a_args, std::string_view a_name, bool a_default)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return a_default;
			if (!it->is_boolean())
				throw ToolError(400, std::format("'{}' must be a boolean", a_name));
			return it->get<bool>();
		}

		bool IsWhitespaceOnly(std::string_view a_value)
		{
			return std::ranges::all_of(a_value, [](unsigned char a_ch) {
				return a_ch == ' ' || a_ch == '\t' || a_ch == '\v' || a_ch == '\f';
			});
		}

		bool HasLineSeparator(std::string_view a_value)
		{
			return a_value.find_first_of("\r\n") != std::string_view::npos ||
			       a_value.find("\xE2\x80\xA8") != std::string_view::npos ||
			       a_value.find("\xE2\x80\xA9") != std::string_view::npos;
		}

		std::string FirstWordLower(std::string_view a_command)
		{
			const auto begin = a_command.find_first_not_of(" \t\v\f");
			if (begin == std::string_view::npos)
				return {};
			const auto  end = a_command.find_first_of(" \t\v\f", begin);
			std::string word{ a_command.substr(begin, end - begin) };
			std::ranges::transform(word, word.begin(), [](unsigned char a_ch) {
				return static_cast<char>(a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return word;
		}

		void ValidateCommand(const std::string& a_command)
		{
			if (a_command.empty() || IsWhitespaceOnly(a_command))
				throw ToolError(400, "'command' must not be empty or whitespace-only");
			if (a_command.size() > kMaxConsoleCommandBytes)
				throw ToolError(400, std::format("'command' exceeds {} bytes", kMaxConsoleCommandBytes));
			if (a_command.find('\0') != std::string::npos)
				throw ToolError(400, "'command' must not contain an embedded NUL");
			if (HasLineSeparator(a_command))
				throw ToolError(400, "'command' must contain exactly one line");

			const auto verb = FirstWordLower(a_command);
			if (verb == "save" || verb == "savegame" || verb == "load" || verb == "loadgame")
				throw ToolError(422,
					"raw console save/load commands are unsupported because that execution path can deadlock the game");
		}

		Capture AddCapture(State& a_state)
		{
			const auto sequence = ++a_state.nextCapture;
			Capture    capture{
				.id = std::format("cap-{:016X}-{:016X}", a_state.captureSeed, sequence),
				.beginMarker = std::format("DVBCAPBEGINx{:016X}x{:016X}", a_state.captureSeed, sequence),
				.endMarker = std::format("DVBCAPENDx{:016X}x{:016X}", a_state.captureSeed, sequence),
			};

			a_state.captureOrder.push_back(capture.id);
			a_state.captures.emplace(capture.id, capture);
			while (a_state.captureOrder.size() > kMaxConsoleCaptures)
			{
				a_state.captures.erase(a_state.captureOrder.front());
				a_state.captureOrder.pop_front();
			}
			return capture;
		}

		void RemoveCapture(State& a_state, std::string_view a_id)
		{
			a_state.captures.erase(std::string(a_id));
			std::erase(a_state.captureOrder, a_id);
		}

		json HandleConsole(const json& a_args, bool a_allowCommands, const ConsoleBackend& a_backend,
			const std::shared_ptr<State>& a_state)
		{
			RequireObject(a_args);
			const auto action = a_args.contains("action") ? ReadString(a_args, "action", false) : "exec";

			if (action == "read")
			{
				const auto captureId = ReadString(a_args, "captureId", true);
				if (captureId.empty() || captureId.size() > 80)
					throw ToolError(400, "'captureId' must be a non-empty capture identifier");

				Capture capture;
				{
					const std::lock_guard lock{ a_state->mutex };
					const auto            it = a_state->captures.find(captureId);
					if (it == a_state->captures.end())
						throw ToolError(404, "unknown or evicted console captureId");
					capture = it->second;
				}

				if (!a_backend.readBuffer)
					throw ToolError(500, "console buffer reader unavailable");
				const auto parsed =
					ExtractConsoleCapture(a_backend.readBuffer(), capture.beginMarker, capture.endMarker);

				std::string status = "pendingOrEvicted";
				if (parsed.sawBegin && parsed.sawEnd)
					status = "complete";
				else if (parsed.sawBegin)
					status = "pending";
				else if (parsed.sawEnd)
					status = "beginEvicted";

				return json{
					{ "captureId", capture.id },
					{ "status", status },
					{ "markersFound", parsed.sawBegin && parsed.sawEnd },
					{ "sawBegin", parsed.sawBegin },
					{ "sawEnd", parsed.sawEnd },
					{ "count", parsed.lines.size() },
					{ "truncated", parsed.truncated },
					{ "lines", parsed.lines },
					{ "note",
						"Native console output is asynchronous; unsolicited lines may interleave inside the ordered fence window." },
				};
			}

			if (action != "exec")
				throw ToolError(400, std::format("unknown console action '{}'", action));

			const auto command = ReadString(a_args, "command", true);
			ValidateCommand(command);
			const bool captureRequested = ReadBool(a_args, "capture", false);
			RequireToolPermission(a_allowCommands, ToolPermission::kConsoleCommands);
			if (!a_backend.queueCommands)
				throw ToolError(500, "console command dispatcher unavailable");

			std::optional<Capture> capture;
			{
				const std::lock_guard lock{ a_state->mutex };
				if (a_state->pendingSubmissions >= kMaxPendingConsoleSubmissions)
					throw ToolError(429, "too many pending console submissions");
				++a_state->pendingSubmissions;
				if (captureRequested)
					capture = AddCapture(*a_state);
			}

			std::vector<std::string> commands;
			commands.reserve(capture ? 3 : 1);
			if (capture)
				commands.push_back(capture->beginMarker);
			commands.push_back(command);
			if (capture)
				commands.push_back(capture->endMarker);

			const std::weak_ptr<State> weakState = a_state;
			const bool                 queued = a_backend.queueCommands(std::move(commands), [weakState]() {
				if (const auto state = weakState.lock())
				{
					const std::lock_guard lock{ state->mutex };
					if (state->pendingSubmissions > 0)
						--state->pendingSubmissions;
				}
			});
			if (!queued)
			{
				const std::lock_guard lock{ a_state->mutex };
				if (a_state->pendingSubmissions > 0)
					--a_state->pendingSubmissions;
				if (capture)
					RemoveCapture(*a_state, capture->id);
				throw ToolError(503, "console command could not be queued");
			}

			json out{
				{ "queued", true },
				{ "command", command },
				{ "capturing", captureRequested },
				{ "note",
					"Queued means the main-thread submission was accepted, not that the command or its asynchronous effects succeeded." },
			};
			if (capture)
				out["captureId"] = capture->id;
			return out;
		}
	}

	ConsoleCaptureResult ExtractConsoleCapture(
		std::string_view a_buffer,
		std::string_view a_beginMarker,
		std::string_view a_endMarker,
		std::size_t      a_maxLines)
	{
		ConsoleCaptureResult out;
		if (a_beginMarker.empty() || a_endMarker.empty())
			return out;

		const auto begin = a_buffer.rfind(a_beginMarker);
		const auto anyEnd = a_buffer.find(a_endMarker);
		out.sawBegin = begin != std::string_view::npos;
		out.sawEnd = anyEnd != std::string_view::npos;
		if (!out.sawBegin)
			return out;

		const auto end = a_buffer.find(a_endMarker, begin + a_beginMarker.size());
		out.sawEnd = end != std::string_view::npos;
		if (!out.sawEnd)
			return out;
		const auto firstLineEnd = a_buffer.find('\n', begin);
		if (firstLineEnd == std::string_view::npos)
			return out;

		const auto start = firstLineEnd + 1;
		const auto markerLineStart = a_buffer.rfind('\n', end);
		const auto stop =
			markerLineStart == std::string_view::npos || markerLineStart < start ? start : markerLineStart;

		std::size_t cursor = start;
		while (cursor < stop)
		{
			const auto newline = a_buffer.find('\n', cursor);
			const auto lineEnd = newline == std::string_view::npos || newline > stop ? stop : newline;
			auto       line = a_buffer.substr(cursor, lineEnd - cursor);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			if (!line.empty())
				out.lines.emplace_back(line);
			if (lineEnd == stop)
				break;
			cursor = lineEnd + 1;
		}

		if (out.lines.size() > a_maxLines)
		{
			out.truncated = true;
			out.lines.erase(out.lines.begin(),
				out.lines.end() - static_cast<std::ptrdiff_t>(a_maxLines));
		}
		return out;
	}

	ToolDescriptor BuildConsoleDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "console";
		descriptor.description =
			"Queue one console command on the game thread. action='exec' is disabled "
			"unless allowConsoleCommands=true. capture=true returns a unique captureId; poll "
			"action='read' with that id for an ordered native ConsoleLog slice. queued=true is only "
			"an acceptance acknowledgement, not proof that the command or its asynchronous effects "
			"succeeded. Capture is bounded and best-effort: native output may be delayed/truncated "
			"and unrelated console lines may interleave. Raw save/load/savegame/loadgame commands "
			"are rejected because that console path can deadlock.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "exec", "read" }) }, { "default", "exec" } } },
								{ "command", json{ { "type", "string" }, { "minLength", 1 }, { "maxLength", kMaxConsoleCommandBytes }, { "description", "exec: exactly one non-blank console command" } } },
								{ "capture", json{ { "type", "boolean" }, { "default", false }, { "description", "exec: surround the command with unique native console fences" } } },
								{ "captureId", json{ { "type", "string" }, { "description", "read: id returned by exec with capture=true" } } },
							} },
			{ "additionalProperties", false },
		};
		return descriptor;
	}

	void RegisterConsoleTool(ToolRegistry& a_registry, bool a_allowCommands, ConsoleBackend a_backend)
	{
		auto state = std::make_shared<State>();
		a_registry.Register(BuildConsoleDescriptor(),
			[a_allowCommands, backend = std::move(a_backend), state = std::move(state)](
				const json& a_args, const ToolContext&) {
				return HandleConsole(a_args, a_allowCommands, backend, state);
			});
	}
}

#include "NoticeTool.h"

#include "Notices.h"

#include <format>

namespace dvb::tools
{
	namespace
	{
		json HandleNotice(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("list"));
			auto&             board = Notices::Global();
			if (action == "list")
				return json{ { "notices", Notices::ToJson(board.Active(Notices::NowMs())) } };

			const std::string key = a_args.value("key", std::string{});
			if (key.empty())
				throw ToolError(400, "action '" + action + "' requires a 'key'");
			if (action == "clear")
				return json{ { "cleared", board.Clear(key) } };
			if (action != "post")
				throw ToolError(400, "unknown action '" + action + "' (list|post|clear)");

			const std::string text = a_args.value("text", std::string{});
			if (text.empty())
				throw ToolError(400, "action 'post' requires 'text'");
			Notices::Severity severity = Notices::Severity::kInfo;
			if (a_args.contains("severity") &&
				!Notices::ParseSeverity(a_args.value("severity", std::string{}), severity))
				throw ToolError(400, "severity must be info, warning or critical");
			std::uint32_t ttlMs = 0;
			if (a_args.contains("ttlMs"))
			{
				const auto& value = a_args["ttlMs"];
				if (!value.is_number_unsigned() || value.get<std::uint64_t>() > Notices::kMaxTtlMs)
					throw ToolError(400, std::format("ttlMs must be an integer from 0 to {}", Notices::kMaxTtlMs));
				ttlMs = value.get<std::uint32_t>();
			}
			board.Post(key, a_args.value("source", std::string("agent")), severity, text, ttlMs, Notices::NowMs());
			return json{ { "posted", true }, { "key", key } };
		}
	}

	ToolDescriptor BuildNoticeDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "notice";
		descriptor.description =
			"Post a message that every tool reply carries (a `notices` array, most severe first) until it is cleared or "
			"expires, so an agent or a headless script sees a critical condition on its very next call. "
			"action='post' takes key (a repeat replaces it), text, severity info|warning|critical, optional source and "
			"ttlMs (0 = until cleared); 'clear' takes key; 'list' returns the active notices. Replies also carry "
			"`blockedBy` while a blocking menu (a tutorial, a message box) is open; pass noNotices=true on any call to "
			"leave both out.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties",
				json{
					{ "action", { { "type", "string" }, { "enum", { "list", "post", "clear" } } } },
					{ "key", { { "type", "string" } } },
					{ "text", { { "type", "string" } } },
					{ "severity", { { "type", "string" }, { "enum", { "info", "warning", "critical" } } } },
					{ "source", { { "type", "string" } } },
					{ "ttlMs", { { "type", "integer" }, { "minimum", 0 }, { "maximum", Notices::kMaxTtlMs } } },
				} }
		};
		return descriptor;
	}

	void RegisterNoticeTool(ToolRegistry& a_registry)
	{
		a_registry.Register(BuildNoticeDescriptor(), HandleNotice);
	}
}

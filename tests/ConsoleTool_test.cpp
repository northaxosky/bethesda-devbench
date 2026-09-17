#include "test_framework.h"

#include "tools/ConsoleTool.h"

using dvb::json;
using dvb::ToolContext;
using dvb::ToolRegistry;

namespace
{
	struct FakeConsole
	{
		std::vector<std::vector<std::string>> submissions;
		std::vector<std::function<void()>>    completions;
		std::string                           buffer;

		dvb::tools::ConsoleBackend Backend()
		{
			return {
				.queueCommands = [this](std::vector<std::string> a_commands, std::function<void()> a_completed) {
					submissions.push_back(std::move(a_commands));
					completions.push_back(std::move(a_completed));
					return true; },
				.readBuffer = [this] { return buffer; },
			};
		}
	};

	dvb::ToolResult Invoke(ToolRegistry& a_registry, json a_args)
	{
		return a_registry.Invoke("console", a_args, ToolContext{});
	}

	std::string Echo(std::string_view a_marker)
	{
		return std::format("Script command \"{}\" not found.", a_marker);
	}
}

TEST_CASE("console validates arguments and requires explicit permission")
{
	FakeConsole  fake;
	ToolRegistry registry;
	dvb::tools::RegisterConsoleTool(registry, false, fake.Backend());

	CHECK(Invoke(registry, json{ { "command", "help" } }).errorCode == 403);
	CHECK(Invoke(registry, json::array()).errorCode == 400);
	CHECK(Invoke(registry, json{ { "command", 1 } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "command", "   \t" } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "command", "help\nquit" } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "command", std::string("a\0b", 3) } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "command", "help" }, { "capture", "true" } }).errorCode == 400);
	CHECK(fake.submissions.empty());
}

TEST_CASE("console rejects the known deadlocking raw save and load verbs")
{
	FakeConsole  fake;
	ToolRegistry registry;
	dvb::tools::RegisterConsoleTool(registry, true, fake.Backend());

	for (const auto* command : { "save x", "SAVEGAME x", " load x", "LoadGame x" })
		CHECK(Invoke(registry, json{ { "command", command } }).errorCode == 422);
	CHECK(fake.submissions.empty());
}

TEST_CASE("console exec is a fire-and-forget game-thread queue boundary")
{
	FakeConsole  fake;
	ToolRegistry registry;
	dvb::tools::RegisterConsoleTool(registry, true, fake.Backend());

	const auto result = Invoke(registry, json{ { "command", "getav health" }, { "capture", true } });
	CHECK(result.ok);
	CHECK(result.value.at("queued") == true);
	CHECK(result.value.at("capturing") == true);
	CHECK(result.value.contains("captureId"));
	CHECK(fake.submissions.size() == 1);
	CHECK(fake.submissions[0].size() == 3);
	CHECK(fake.submissions[0][1] == "getav health");
	CHECK(fake.submissions[0][0].find("DVBCAPBEGIN") == 0);
	CHECK(fake.submissions[0][2].find("DVBCAPEND") == 0);
	// The fake has only accepted the closure; command execution is deliberately not
	// synchronous with the request.
	CHECK(fake.completions.size() == 1);
}

TEST_CASE("console capture parser handles CRLF empty incomplete and truncated windows")
{
	const auto complete = dvb::tools::ExtractConsoleCapture(
		"old\r\nbegin-token echoed\r\none\r\ntwo\r\nend-token echoed\r\nnew\r\n",
		"begin-token", "end-token", 200);
	CHECK(complete.sawBegin);
	CHECK(complete.sawEnd);
	CHECK(complete.lines.size() == 2);
	CHECK(complete.lines[0] == "one");
	CHECK(complete.lines[1] == "two");

	const auto empty = dvb::tools::ExtractConsoleCapture(
		"begin-token echoed\r\nend-token echoed\r\n", "begin-token", "end-token", 200);
	CHECK(empty.sawBegin && empty.sawEnd);
	CHECK(empty.lines.empty());

	const auto missingEnd =
		dvb::tools::ExtractConsoleCapture("begin-token echoed\npartial\n", "begin-token", "end-token", 200);
	CHECK(missingEnd.sawBegin);
	CHECK(!missingEnd.sawEnd);
	CHECK(missingEnd.lines.empty());

	const auto beginEvicted =
		dvb::tools::ExtractConsoleCapture("noise\nend-token echoed\n", "begin-token", "end-token", 200);
	CHECK(!beginEvicted.sawBegin);
	CHECK(beginEvicted.sawEnd);
	CHECK(beginEvicted.lines.empty());

	const auto truncated = dvb::tools::ExtractConsoleCapture(
		"begin-token\none\ntwo\nthree\nend-token\n", "begin-token", "end-token", 2);
	CHECK(truncated.truncated);
	CHECK(truncated.lines.size() == 2);
	CHECK(truncated.lines[0] == "two");
	CHECK(truncated.lines[1] == "three");
}

TEST_CASE("console capture ids isolate concurrent requests and reject wrong ids")
{
	FakeConsole  fake;
	ToolRegistry registry;
	dvb::tools::RegisterConsoleTool(registry, true, fake.Backend());

	const auto first = Invoke(registry, json{ { "command", "help first" }, { "capture", true } });
	const auto second = Invoke(registry, json{ { "command", "help second" }, { "capture", true } });
	CHECK(first.ok && second.ok);
	const auto firstId = first.value.at("captureId").get<std::string>();
	const auto secondId = second.value.at("captureId").get<std::string>();
	CHECK(firstId != secondId);
	CHECK(fake.submissions.size() == 2);

	fake.buffer =
		Echo(fake.submissions[0][0]) + "\r\nfirst output\r\n" + Echo(fake.submissions[0][2]) + "\r\n" +
		Echo(fake.submissions[1][0]) + "\r\nsecond output\r\n" + Echo(fake.submissions[1][2]) + "\r\n";

	const auto firstRead = Invoke(registry, json{ { "action", "read" }, { "captureId", firstId } });
	const auto secondRead = Invoke(registry, json{ { "action", "read" }, { "captureId", secondId } });
	CHECK(firstRead.ok && secondRead.ok);
	CHECK(firstRead.value.at("markersFound") == true);
	CHECK(firstRead.value.at("lines") == json::array({ "first output" }));
	CHECK(secondRead.value.at("lines") == json::array({ "second output" }));
	CHECK(Invoke(registry, json{ { "action", "read" }, { "captureId", "wrong-id" } }).errorCode == 404);
}

TEST_CASE("console capture read reports incomplete fences without asserting success")
{
	FakeConsole  fake;
	ToolRegistry registry;
	dvb::tools::RegisterConsoleTool(registry, true, fake.Backend());
	const auto queued = Invoke(registry, json{ { "command", "help" }, { "capture", true } });
	const auto id = queued.value.at("captureId").get<std::string>();

	fake.buffer = Echo(fake.submissions[0][0]) + "\npartial output\n";
	const auto pending = Invoke(registry, json{ { "action", "read" }, { "captureId", id } });
	CHECK(pending.ok);
	CHECK(pending.value.at("markersFound") == false);
	CHECK(pending.value.at("status") == "pending");
	CHECK(pending.value.at("lines").empty());
}

TEST_CASE("console bounds pending submissions and capture bookkeeping")
{
	FakeConsole  fake;
	ToolRegistry registry;
	dvb::tools::RegisterConsoleTool(registry, true, fake.Backend());

	for (std::size_t i = 0; i < dvb::tools::kMaxPendingConsoleSubmissions; ++i)
		CHECK(Invoke(registry, json{ { "command", "help" } }).ok);
	CHECK(Invoke(registry, json{ { "command", "help" } }).errorCode == 429);
	for (auto& completion : fake.completions)
		completion();

	FakeConsole  captureFake;
	ToolRegistry captureRegistry;
	dvb::tools::RegisterConsoleTool(captureRegistry, true, captureFake.Backend());
	std::string firstId;
	for (std::size_t i = 0; i <= dvb::tools::kMaxConsoleCaptures; ++i)
	{
		const auto result =
			Invoke(captureRegistry, json{ { "command", "help" }, { "capture", true } });
		CHECK(result.ok);
		if (i == 0)
			firstId = result.value.at("captureId").get<std::string>();
		captureFake.completions.back()();
	}
	CHECK(Invoke(
			  captureRegistry, json{ { "action", "read" }, { "captureId", firstId } })
			  .errorCode == 404);
}

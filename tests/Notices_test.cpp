#include "test_framework.h"

#include "NoticeAnnotator.h"
#include "Notices.h"
#include "ToolRegistry.h"

using dvb::Notices::Severity;

TEST_CASE("A posted notice replaces the same key and sorts most severe first")
{
	dvb::Notices::Board board;
	board.Post("a", "p", Severity::kInfo, "first", 0, 0);
	board.Post("b", "p", Severity::kCritical, "bad", 0, 0);
	board.Post("a", "p", Severity::kWarning, "second", 0, 0);
	const auto active = board.Active(1);
	CHECK(active.size() == 2);
	CHECK(active[0].key == "b");
	CHECK(active[1].text == "second");
}

TEST_CASE("A notice expires after its ttl and can be cleared")
{
	dvb::Notices::Board board;
	board.Post("t", "p", Severity::kInfo, "x", 100, 1000);
	CHECK(board.Active(1099).size() == 1);
	CHECK(board.Active(1100).empty());
	board.Post("c", "p", Severity::kInfo, "y", 0, 0);
	CHECK(board.Clear("c"));
	CHECK(!board.Clear("c"));
}

TEST_CASE("The notice annotator decorates replies only while a notice is active")
{
	dvb::ToolRegistry registry;
	registry.Register({ "ok", "d" }, [](const dvb::json&, const dvb::ToolContext&) { return dvb::json::object(); });
	registry.Register({ "bad", "d" }, [](const dvb::json&, const dvb::ToolContext&) -> dvb::json {
		throw dvb::ToolError(404, "nope");
	});
	dvb::Notices::AttachToReplies(registry);
	CHECK(!registry.Invoke("ok", dvb::json::object(), {}).value.contains("notices"));

	auto& board = dvb::Notices::Global();
	board.Post("test", "plugin", Severity::kCritical, "disk full", 0, dvb::Notices::NowMs());
	const auto reply = registry.Invoke("ok", dvb::json::object(), {});
	CHECK(reply.value["notices"][0]["text"] == "disk full");
	CHECK(registry.Invoke("bad", dvb::json::object(), {}).errorMessage ==
		  "nope [critical notice from plugin: disk full]");
	CHECK(!registry.Invoke("ok", dvb::json{ { "noNotices", true } }, {}).value.contains("notices"));
	board.Clear("test");
}

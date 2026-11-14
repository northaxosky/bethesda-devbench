#include "test_framework.h"

#include "GameProfile.h"
#include "ModalLogic.h"
#include "ModalWatch.h"
#include "NoticeAnnotator.h"
#include "Notices.h"
#include "ToolRegistry.h"

#include <chrono>
#include <future>

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

TEST_CASE("Only menus outside the profile list block, each with a hint")
{
	const auto fo4 = dvb::Fallout4Profile().nonBlockingMenus;
	const auto blockers = dvb::Modal::Blockers(
		fo4, { "HUDMenu", "TutorialMenu", "MessageBoxMenu", "CursorMenu", "FaderMenu", "Console" });
	CHECK(blockers.size() == 2);
	CHECK(blockers[0].name == "MessageBoxMenu");
	CHECK(blockers[0].hint.find("accept") != std::string::npos);
	CHECK(blockers[1].hint == "menu action=close name=\"TutorialMenu\"");
	CHECK(dvb::Modal::Blockers(fo4, { "HUDMenu", "CursorMenu" }).empty());

	const auto skyrim = dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimAE).nonBlockingMenus;
	CHECK(dvb::Modal::Blockers(skyrim, { "HUD Menu", "Mist Menu", "LootMenu" }).empty());
	CHECK(dvb::Modal::Blockers(skyrim, { "HUDMenu" }).size() == 1);
}

TEST_CASE("Replies gain blockedBy while a blocking menu is open and lose it on close")
{
	dvb::ToolRegistry registry;
	registry.Register({ "ok", "d" }, [](const dvb::json&, const dvb::ToolContext&) { return dvb::json::object(); });
	registry.Register({ "bad", "d" }, [](const dvb::json&, const dvb::ToolContext&) -> dvb::json {
		throw dvb::ToolError(409, "nope");
	});
	std::vector<std::string> open{ "HUDMenu" };
	dvb::Notices::AttachToReplies(
		registry,
		{ .openMenus = [&open] { return open; },
			.nonBlockingMenus = dvb::Fallout4Profile().nonBlockingMenus });

	CHECK(registry.Invoke("ok", dvb::json::object(), {}).value.is_object());
	CHECK(!registry.Invoke("ok", dvb::json::object(), {}).value.contains("blockedBy"));

	open = { "CursorMenu", "HUDMenu", "MessageBoxMenu" };
	const auto reply = registry.Invoke("ok", dvb::json::object(), {});
	CHECK(reply.value["blockedBy"].size() == 1);
	CHECK(reply.value["blockedBy"][0]["name"] == "MessageBoxMenu");
	CHECK(registry.Invoke("bad", dvb::json::object(), {}).errorMessage.starts_with("nope [blocked by MessageBoxMenu: "));
	CHECK(!registry.Invoke("ok", dvb::json{ { "noNotices", true } }, {}).value.contains("blockedBy"));

	open = { "HUDMenu" };
	CHECK(!registry.Invoke("ok", dvb::json::object(), {}).value.contains("blockedBy"));
}

TEST_CASE("Modal events fire when a blocker opens and when the last one closes")
{
	dvb::EventBus            events;
	std::vector<std::string> open;
	const auto               source = dvb::Modal::ModalSource{
		.openMenus = [&open] { return open; },
		.nonBlockingMenus = dvb::Fallout4Profile().nonBlockingMenus
	};
	std::promise<std::string> cleared;
	const auto                spy = events.Subscribe([&](const dvb::EventBus::Event& a_event) {
        if (a_event.topic == "modal.cleared")
            cleared.set_value(a_event.payload.value("name", ""));
    });
	const auto                watch = dvb::Modal::Watch(events, source);

	open = { "HUDMenu", "MessageBoxMenu" };
	events.Publish("menu", { { "name", "HUDMenu" }, { "opening", true } });
	events.Publish("menu", { { "name", "MessageBoxMenu" }, { "opening", true } });
	open = { "HUDMenu" };
	events.Publish("menu", { { "name", "MessageBoxMenu" }, { "opening", false } });

	auto future = cleared.get_future();
	const bool ready = future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
	CHECK(ready);
	if (ready)
		CHECK(future.get() == "MessageBoxMenu");
	const auto blocked = events.Since(0);
	const auto count = std::ranges::count_if(
		blocked, [](const auto& a_event) { return a_event.topic == "modal.blocked"; });
	CHECK(count == 1);
	events.Unsubscribe(watch);
	events.Unsubscribe(spy);
}

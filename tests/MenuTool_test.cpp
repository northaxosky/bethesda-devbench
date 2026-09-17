#include "test_framework.h"

#include "ToolExtensions.h"
#include "tools/menu/MenuTool.h"

using dvb::json;
using dvb::ToolContext;
using dvb::ToolError;
using dvb::ToolRegistry;

namespace
{
	dvb::ToolResult Invoke(ToolRegistry& a_registry, json a_args)
	{
		return a_registry.Invoke("menu", a_args, ToolContext{});
	}

	dvb::tools::MenuDialogSnapshot Dialog(
		std::string a_body, std::vector<std::string> a_buttons)
	{
		return {
			.fingerprint = {
				.headerText = "Warning",
				.bodyText = std::move(a_body),
				.buttons = std::move(a_buttons),
			},
			.cancelIndex = std::nullopt,
		};
	}
}

TEST_CASE("menu descriptor exposes the approved actions and conservative dialog contract")
{
	const auto descriptor = dvb::tools::BuildMenuDescriptor();
	CHECK(descriptor.name == "menu");
	CHECK(!descriptor.readOnly);
	const auto& actions =
		descriptor.inputSchema.at("properties").at("action").at("enum");
	CHECK(std::ranges::find(actions, "list") != actions.end());
	CHECK(std::ranges::find(actions, "describe") != actions.end());
	CHECK(std::ranges::find(actions, "accept") != actions.end());
	CHECK(std::ranges::find(actions, "open") != actions.end());
	CHECK(std::ranges::find(actions, "close") != actions.end());
	CHECK(std::ranges::find(actions, "invoke") != actions.end());
	CHECK(descriptor.description.find("cancelIndex is null") != std::string::npos);
}

TEST_CASE("menu reads remain available while control actions fail closed")
{
	int                     mutations = 0;
	dvb::tools::MenuBackend backend{
		.listOpenMenus = [] { return std::vector<std::string>{ "PauseMenu", "MessageBoxMenu" }; },
		.describeDialog = [] { return std::optional{ Dialog("Missing content. Continue?", { "Yes", "No" }) }; },
		.acceptDialog = [&](dvb::tools::MenuAcceptRequest) {
			++mutations;
			return json::object(); },
		.openMenu = [&](std::string) {
			++mutations;
			return json::object(); },
		.closeMenu = [&](std::string) {
			++mutations;
			return json::object(); },
	};

	ToolRegistry registry;
	dvb::tools::RegisterMenuTool(registry, false, std::move(backend));

	const auto list = Invoke(registry, json::object());
	CHECK(list.ok);
	CHECK(list.value.at("messageBoxOpen") == true);
	const auto describe = Invoke(registry, json{ { "action", "describe" } });
	CHECK(describe.ok);
	CHECK(describe.value.at("cancelIndex").is_null());
	CHECK(describe.value.at("fingerprint").at("buttons").size() == 2);

	CHECK(Invoke(registry,
			  json{
				  { "action", "accept" },
				  { "index", 0 },
				  { "matchBody", "missing content" },
			  })
			  .errorCode == 403);
	CHECK(Invoke(registry, json{ { "action", "open" }, { "name", "PauseMenu" } })
			  .errorCode == 403);
	CHECK(mutations == 0);
}

TEST_CASE("menu accept requires an explicit index and identifying body match")
{
	int                     accepted = 0;
	dvb::tools::MenuBackend backend{
		.describeDialog = [] { return std::optional{ Dialog("Missing content. Continue?", { "Yes", "No" }) }; },
		.acceptDialog = [&](dvb::tools::MenuAcceptRequest) {
			++accepted;
			return json{ { "accepted", true } }; },
	};
	ToolRegistry registry;
	dvb::tools::RegisterMenuTool(registry, true, std::move(backend));

	CHECK(Invoke(registry,
			  json{ { "action", "accept" }, { "matchBody", "missing content" } })
			  .errorCode == 400);
	CHECK(Invoke(registry,
			  json{ { "action", "accept" }, { "index", 0 } })
			  .errorCode == 400);
	CHECK(Invoke(registry,
			  json{
				  { "action", "accept" },
				  { "index", 0 },
				  { "matchBody", "unrelated prompt" },
			  })
			  .errorCode == 409);
	CHECK(accepted == 0);
}

TEST_CASE("menu accept carries the complete observed fingerprint into dispatch")
{
	auto                    current = Dialog("Missing content. Continue?", { "Yes", "No" });
	bool                    dispatched = false;
	dvb::tools::MenuBackend backend{
		.describeDialog = [&] {
			auto observed = current;
			current = Dialog("Delete this save?", { "Delete", "Cancel" });
			return std::optional{ observed }; },
		.acceptDialog = [&](dvb::tools::MenuAcceptRequest a_request) -> json {
			dispatched = true;
			if (a_request.fingerprint != current.fingerprint)
				throw ToolError(
					409, "the active MessageBoxMenu changed before acceptance");
			return json{ { "accepted", true } };
		},
	};
	ToolRegistry registry;
	dvb::tools::RegisterMenuTool(registry, true, std::move(backend));

	const auto result = Invoke(registry,
		json{
			{ "action", "accept" },
			{ "index", 0 },
			{ "matchBody", "missing content" },
		});
	CHECK(dispatched);
	CHECK(!result.ok);
	CHECK(result.errorCode == 409);
}

TEST_CASE("menu open is allowlisted and MessageBoxMenu cannot be hidden as cancellation")
{
	int                     opens = 0;
	int                     closes = 0;
	dvb::tools::MenuBackend backend{
		.openMenu = [&](std::string a_name) {
			++opens;
			return json{ { "name", a_name } }; },
		.closeMenu = [&](std::string a_name) {
			++closes;
			return json{ { "name", a_name } }; },
	};
	ToolRegistry registry;
	dvb::tools::RegisterMenuTool(registry, true, std::move(backend));

	CHECK(Invoke(registry,
			  json{ { "action", "open" }, { "name", "ContainerMenu" } })
			  .errorCode == 422);
	CHECK(Invoke(registry,
		json{ { "action", "open" }, { "name", "PauseMenu" } })
			.ok);
	CHECK(Invoke(registry,
			  json{ { "action", "close" }, { "name", "MessageBoxMenu" } })
			  .errorCode == 422);
	CHECK(Invoke(registry,
		json{ { "action", "close" }, { "name", "PauseMenu" } })
			.ok);
	CHECK(opens == 1);
	CHECK(closes == 1);
}

TEST_CASE("menu invoke reuses the existing extension registry")
{
	const std::string key = "MenuToolContractFixture";
	dvb::ToolExtensions::Register(
		"menu", key, json{ { "description", "fixture menu" } },
		[](const json& a_args, const ToolContext&) {
			return json{ { "echo", a_args.at("value") } };
		});

	ToolRegistry registry;
	int          listCalls = 0;
	auto         service = dvb::tools::RegisterMenuTool(registry, true, {
																			.listOpenMenus = [&] {
																		++listCalls;
																		return std::vector<std::string>{};
																			},
																		});
	const auto   list = Invoke(registry, json::object());
	CHECK(list.ok);
	CHECK(listCalls == 1);
	const auto registered =
		list.value.at("registered").get<std::vector<std::string>>();
	CHECK(std::ranges::find(registered, key) != registered.end());

	const auto describe =
		Invoke(registry, json{ { "action", "describe" }, { "name", key } });
	CHECK(describe.ok);
	CHECK(describe.value.at("descriptor").at("description") == "fixture menu");

	const auto invoked = Invoke(registry,
		json{
			{ "action", "invoke" },
			{ "name", key },
			{ "value", 42 },
		});
	CHECK(invoked.ok);
	CHECK(invoked.value.at("echo") == 42);

	const std::string refreshedKey = "MenuToolRefreshFixture";
	dvb::ToolExtensions::Register(
		"menu", refreshedKey, json{ { "description", "refresh fixture" } },
		[](const json&, const ToolContext&) { return json::object(); });
	CHECK(registry.Describe("menu")->description.find(refreshedKey) ==
		  std::string::npos);

	service->RefreshDescriptor();
	CHECK(registry.Describe("menu")->description.find(refreshedKey) !=
		  std::string::npos);
	CHECK(Invoke(registry, json::object()).ok);
	CHECK(listCalls == 2);
}

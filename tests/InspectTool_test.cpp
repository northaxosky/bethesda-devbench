#include "test_framework.h"

#include "ToolExtensions.h"
#include "tools/InspectTool.h"

using dvb::json;
using dvb::ToolContext;
using dvb::ToolRegistry;

TEST_CASE("player readiness rejects menus and incomplete actor state without invented values")
{
	using dvb::tools::AssessPlayerReadiness;
	using dvb::tools::PlayerReadinessInput;

	CHECK(AssessPlayerReadiness(PlayerReadinessInput{}).reason == "gameDataNotReady");
	CHECK(AssessPlayerReadiness({ .gameDataReady = true, .inMainMenu = true }).reason == "mainMenu");
	CHECK(AssessPlayerReadiness({ .gameDataReady = true, .inLoadingMenu = true }).reason == "loadingMenu");
	CHECK(AssessPlayerReadiness(
			  { .gameDataReady = true, .loadInProgress = true, .hasPlayer = true, .hasNpc = true, .has3D = true })
			  .reason == "loadInProgress");
	CHECK(AssessPlayerReadiness(
			  { .gameDataReady = true, .loadFailed = true, .hasPlayer = true, .hasNpc = true, .has3D = true })
			  .reason == "loadFailed");
	CHECK(AssessPlayerReadiness(
			  { .gameDataReady = true, .inLoadingMenu = true, .loadInProgress = false, .hasPlayer = true, .hasNpc = true, .has3D = true })
			  .reason == "loadingMenu");
	CHECK(AssessPlayerReadiness({ .gameDataReady = true }).reason == "playerUnavailable");
	CHECK(AssessPlayerReadiness({ .gameDataReady = true, .hasPlayer = true }).reason == "playerBaseUnavailable");
	CHECK(AssessPlayerReadiness({ .gameDataReady = true, .hasPlayer = true, .hasNpc = true }).reason ==
		  "player3DUnavailable");
	CHECK(AssessPlayerReadiness(
		{ .gameDataReady = true, .hasPlayer = true, .hasNpc = true, .has3D = true })
			.loaded);
}

TEST_CASE("inspect validates kind and dispatches builtins through injectable providers")
{
	int                             healthCalls = 0;
	int                             stateCalls = 0;
	dvb::tools::inspection::Request inventoryRequest;
	dvb::tools::inspection::Request refsRequest;
	ToolRegistry                    registry;
	dvb::tools::RegisterInspectTool(registry, {
												  .health = [&] {
													 ++healthCalls;
													 return json{ { "kind", "health" } }; },
												  .state = [&] {
													 ++stateCalls;
													 return json{ { "kind", "state" } }; },
												  .player = [] { return json{ { "kind", "player" } }; },
												  .scene = [] { return json{ { "kind", "scene" } }; },
												  .mods = [] { return json{ { "kind", "mods" } }; },
												  .vm = [](const auto&) { return json{ { "kind", "vm" } }; },
												  .inventory = [&](const auto& a_request) {
													 inventoryRequest = a_request;
													 return json{ { "kind", "inventory" } }; },
												  .quests = [](const auto&) { return json{ { "kind", "quests" } }; },
												  .effects = [](const auto&) { return json{ { "kind", "effects" } }; },
												  .refs = [&](const auto& a_request) {
													 refsRequest = a_request;
													 return json{ { "kind", "refs" } }; },
												  .registrants = [](const auto&) { return json{ { "kind", "registrants" } }; },
											  });

	const auto defaultState = registry.Invoke("inspect", json::object(), ToolContext{});
	const auto health =
		registry.Invoke("inspect", json{ { "kind", "health" } }, ToolContext{});
	CHECK(defaultState.ok && defaultState.value.at("kind") == "state");
	CHECK(health.ok && health.value.at("kind") == "health");
	CHECK(stateCalls == 1);
	CHECK(healthCalls == 1);
	CHECK(registry.Invoke("inspect", json::array(), ToolContext{}).errorCode == 400);
	CHECK(registry.Invoke("inspect", json{ { "kind", 7 } }, ToolContext{}).errorCode == 400);
	const auto inventory = registry.Invoke(
		"inspect",
		json{
			{ "kind", "INVENTORY" },
			{ "formId", "0x14" },
			{ "formType", "Weapon" },
			{ "limit", 7 },
		},
		ToolContext{});
	CHECK(inventory.ok);
	CHECK(inventoryRequest.kind == "inventory");
	CHECK(inventoryRequest.formId == "0x14");
	CHECK(inventoryRequest.formType == "weap");
	CHECK(inventoryRequest.limit == 7);
	const auto selected =
		registry.Invoke("inspect", json{ { "kind", "refs" }, { "selected", true } }, ToolContext{});
	CHECK(selected.ok);
	CHECK(refsRequest.selected);
	CHECK(registry.Invoke("inspect", json{ { "kind", "screenshots" } }, ToolContext{}).errorCode == 400);
}

TEST_CASE("inspect schema lists implemented Fallout 4 builtins but not screenshots")
{
	const auto descriptor = dvb::tools::BuildInspectDescriptor();
	const auto kinds = descriptor.inputSchema.at("properties").at("kind").at("enum");
	CHECK(std::ranges::find(kinds, "health") != kinds.end());
	CHECK(std::ranges::find(kinds, "state") != kinds.end());
	CHECK(std::ranges::find(kinds, "player") != kinds.end());
	CHECK(std::ranges::find(kinds, "scene") != kinds.end());
	CHECK(std::ranges::find(kinds, "mods") != kinds.end());
	CHECK(std::ranges::find(kinds, "vm") != kinds.end());
	CHECK(std::ranges::find(kinds, "inventory") != kinds.end());
	CHECK(std::ranges::find(kinds, "quests") != kinds.end());
	CHECK(std::ranges::find(kinds, "effects") != kinds.end());
	CHECK(std::ranges::find(kinds, "refs") != kinds.end());
	CHECK(std::ranges::find(kinds, "registrants") != kinds.end());
	CHECK(std::ranges::find(kinds, "screenshots") == kinds.end());
}

TEST_CASE("inspect extension discovery and dispatch reuse ToolExtensions")
{
	const std::string key = "tier2-test-extension";
	dvb::ToolExtensions::Register(
		"inspect", key, json{ { "description", "test" } },
		[](const json& a_args, const ToolContext&) {
			return json{ { "extension", true }, { "echo", a_args.at("value") } };
		});

	ToolRegistry registry;
	dvb::tools::RegisterInspectTool(registry, {});
	const auto list =
		registry.Invoke("inspect", json{ { "kind", "extensions" } }, ToolContext{});
	CHECK(list.ok);
	CHECK(list.value.at("extensions").size() >= 1);

	const auto call = registry.Invoke(
		"inspect", json{ { "kind", key }, { "value", 42 }, { "limit", "extension-defined" } }, ToolContext{});
	CHECK(call.ok);
	CHECK(call.value.at("extension") == true);
	CHECK(call.value.at("echo") == 42);
}

TEST_CASE("inspect reports unavailable builtins and preserves extension errors")
{
	const std::string key = "inspection-error-extension";
	dvb::ToolExtensions::Register(
		"inspect", key, json{ { "description", "fails deliberately" } },
		[](const json&, const ToolContext&) -> json {
			throw dvb::ToolError(422, "extension rejected request");
		});

	ToolRegistry registry;
	dvb::tools::RegisterInspectTool(registry, {});

	const auto unavailable =
		registry.Invoke("inspect", json{ { "kind", "vm" } }, ToolContext{});
	CHECK(!unavailable.ok);
	CHECK(unavailable.errorCode == 503);
	CHECK(unavailable.errorMessage.find("vm") != std::string::npos);

	const auto extension =
		registry.Invoke("inspect", json{ { "kind", key } }, ToolContext{});
	CHECK(!extension.ok);
	CHECK(extension.errorCode == 422);
	CHECK(extension.errorMessage == "extension rejected request");
}

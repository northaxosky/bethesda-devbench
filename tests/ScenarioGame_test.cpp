#include "test_framework.h"

#include "tools/ConsoleTool.h"
#include "tools/GameTool.h"
#include "tools/InspectTool.h"
#include "tools/scenario/ScenarioTool.h"

#include <memory>

namespace
{
	using dvb::json;

	struct FixtureState
	{
		bool loaded = false;
		int  level = 2;
		int  loads = 0;
		int  commands = 0;
		std::uint64_t operationId = 0;
	};

	void RegisterFixtureTools(
		dvb::ToolRegistry& a_registry, dvb::EventBus& a_events,
		const std::shared_ptr<FixtureState>& a_state, bool a_allowGame, bool a_allowConsole)
	{
		dvb::tools::RegisterGameTool(a_registry, a_allowGame, {
																  .listSaves = [] { return dvb::tools::GameSaveList{
																						.directory = "fixture-saves",
																						.saves = { { "fixture", 1 } },
																					}; },
																  .status = [state = a_state] { return json{
																									{ "operation", json{
																													   { "operationId", state->operationId },
																													   { "active", false },
																													   { "success", state->loaded },
																												   } },
																								}; },
																  .queueOperation = [state = a_state, events = &a_events](dvb::tools::GameOperationRequest a_request) {
				if (a_request.kind != dvb::tools::GameOperationKind::kLoad)
					throw dvb::ToolError(422, "fixture only implements loads");
				const auto cursor = events->HeadSeq();
				++state->loads;
				state->operationId = static_cast<std::uint64_t>(state->loads);
				state->loaded = true;
				events->Publish("lifecycle", json{ { "event", "postLoadGame" }, { "success", true } });
				return dvb::tools::GameQueueReceipt{
					.operationId = state->operationId,
					.actionCursor = cursor,
				}; },
															  });
		dvb::tools::RegisterInspectTool(a_registry, {
														.state = [state = a_state] { return json{ { "playerLoaded", state->loaded } }; },
														.player = [state = a_state] { return json{ { "playerLoaded", state->loaded }, { "level", state->level } }; },
													});
		dvb::tools::RegisterConsoleTool(a_registry, a_allowConsole, {
																		.queueCommands = [state = a_state](std::vector<std::string> a_commands, std::function<void()> a_done) {
																			for (const auto& command : a_commands)
																			{
																				if (command != "player.setlevel 100")
																					throw dvb::ToolError(400, "unexpected fixture command");
																				++state->commands;
																				state->level = 100;
																			}
																			a_done();
																			return true;
																		},
																	});
	}

	json FixtureScenario()
	{
		return json{ { "steps", json::array({
									json{ { "tool", "game" }, { "args", json{ { "action", "load" }, { "name", "fixture" } } } },
									json{ { "waitFor", "postLoadGame" } },
									json{ { "waitUntil", "playerLoaded" } },
									json{ { "tool", "console" }, { "args", json{ { "command", "player.setlevel 100" } } } },
									json{ { "assert", json{
														  { "tool", "inspect" },
														  { "args", json{ { "kind", "player" } } },
														  { "path", "/level" },
														  { "eq", 100 },
													  } } },
								}) } };
	}
}

TEST_CASE("scenario composes game console and inspect without losing a fast load event")
{
	dvb::ToolRegistry registry;
	dvb::EventBus     events;
	auto              state = std::make_shared<FixtureState>();
	RegisterFixtureTools(registry, events, state, true, true);
	dvb::tools::scenario::ScenarioService scenarios(registry, events);
	scenarios.Register();

	const auto run = registry.Invoke("scenario", FixtureScenario(), {});
	CHECK(run.ok);
	CHECK(run.value.at("ok") == true);
	CHECK(run.value.at("stepsRun") == 5);
	CHECK(state->loads == 1);
	CHECK(state->commands == 1);
	CHECK(run.value.at("results").back().at("actual") == 100);
}

TEST_CASE("scenario cannot bypass game or console permission gates")
{
	for (const auto allowGame : { false, true })
	{
		dvb::ToolRegistry registry;
		dvb::EventBus     events;
		auto              state = std::make_shared<FixtureState>();
		RegisterFixtureTools(registry, events, state, allowGame, false);
		dvb::tools::scenario::ScenarioService scenarios(registry, events);
		scenarios.Register();

		const auto run = registry.Invoke("scenario", FixtureScenario(), {});
		CHECK(run.ok);
		CHECK(run.value.at("ok") == false);
		CHECK(run.value.at("aborted") == true);
		CHECK(state->loads == (allowGame ? 1 : 0));
		CHECK(state->commands == 0);
		CHECK(state->level == 2);
	}
}

TEST_CASE("scenario polls game operation status only through the read-only inspect projection")
{
	dvb::ToolRegistry registry;
	dvb::EventBus     events;
	auto              state = std::make_shared<FixtureState>();
	RegisterFixtureTools(registry, events, state, true, true);
	dvb::tools::scenario::ScenarioService scenarios(registry, events);
	scenarios.Register();

	const auto run = registry.Invoke("scenario", json{ { "steps", json::array({
																	  json{ { "tool", "game" }, { "args", json{ { "action", "load" }, { "name", "fixture" } } } },
																	  json{ { "assert", json{
																							{ "tool", "inspect" },
																							{ "args", json{ { "kind", "saveload" } } },
																							{ "path", "/operation/success" },
																							{ "eq", true },
																						} } },
																  }) } },
		{});
	CHECK(run.ok);
	CHECK(run.value.at("ok") == true);
	CHECK(state->loads == 1);
}

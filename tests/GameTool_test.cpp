#include "test_framework.h"

#include "tools/GameTool.h"
#include "tools/InspectTool.h"

using dvb::json;
using dvb::ToolContext;
using dvb::ToolRegistry;

namespace
{
	struct FakeGame
	{
		dvb::tools::GameSaveList                      saves;
		json                                          status = json::object();
		std::vector<dvb::tools::GameOperationRequest> requests;
		std::vector<dvb::tools::GameSaveListRequest>  listRequests;
		std::vector<std::string>                      metadataRequests;
		std::uint64_t                                 nextOperationId = 1;
		std::uint64_t                                 actionCursor = 17;
		std::optional<dvb::tools::game::SaveMetadata> metadata;
		std::string                                   metadataError;

		dvb::tools::GameBackend Backend()
		{
			return {
				.listSaves = [this] { return saves; },
				.status = [this] { return status; },
				.queueOperation = [this](dvb::tools::GameOperationRequest a_request) {
					requests.push_back(std::move(a_request));
					return dvb::tools::GameQueueReceipt{
						.operationId = nextOperationId++,
						.actionCursor = actionCursor,
					}; },
				.listSavesWithOptions = [this](dvb::tools::GameSaveListRequest a_request) {
					listRequests.push_back(a_request);
					auto result = saves;
					if (a_request.directory)
						result.directory = *a_request.directory;
					return result; },
				.readSaveMetadata = [this](const std::string&, const std::string& a_name) {
					metadataRequests.push_back(a_name);
					return dvb::tools::game::SaveMetadataResult{
						.metadata = metadata,
						.error = metadataError,
					}; },
				.resolveMutationDirectory = [](const std::optional<std::string>& a_directory) { return a_directory.value_or("native"); },
				.advanceTime = [this](double a_hours) { return dvb::tools::GameAdvanceReceipt{
															.operationId = nextOperationId++,
															.actionCursor = actionCursor,
															.completed = true,
															.result = json{
																{ "gameHour", 3.0 },
																{ "daysPassed", 42.125 },
																{ "day", 2 },
																{ "month", 11 },
																{ "year", 2287 },
																{ "rawYear", 287 },
																{ "dayDelta", a_hours > 0 ? 1 : -1 },
															},
														}; },
			};
		}
	};

	dvb::ToolResult Invoke(ToolRegistry& a_registry, json a_args)
	{
		return a_registry.Invoke("game", a_args, ToolContext{});
	}
}

TEST_CASE("game validates action types names and permission before mutation")
{
	FakeGame     fake;
	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, false, fake.Backend());

	CHECK(Invoke(registry, json::array()).errorCode == 400);
	CHECK(Invoke(registry, json{ { "action", 1 } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "action", "bogus" } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "action", "save" }, { "name", "safe" } }).errorCode == 403);
	CHECK(Invoke(registry, json{ { "action", "loadLast" } }).errorCode == 403);
	CHECK(fake.requests.empty());
	const auto status = Invoke(registry, json::object());
	CHECK(status.ok);
	CHECK(status.value.at("actionsAllowed") == false);

	ToolRegistry allowed;
	dvb::tools::RegisterGameTool(allowed, true, fake.Backend());
	for (const auto& bad : {
			 json{ { "action", "save" } },
			 json{ { "action", "save" }, { "name", 4 } },
			 json{ { "action", "save" }, { "name", "" } },
			 json{ { "action", "save" }, { "name", "../escape" } },
			 json{ { "action", "save" }, { "name", "folder\\escape" } },
			 json{ { "action", "save" }, { "name", "slot.fos" } },
			 json{ { "action", "save" }, { "name", "NUL" } },
			 json{ { "action", "load" }, { "name", std::string("a\0b", 3) } },
			 json{ { "action", "loadLast" }, { "name", "extra" } },
		 })
		CHECK(Invoke(allowed, bad).errorCode == 400);
	CHECK(fake.requests.empty());
}

TEST_CASE("game list is deterministic bounded and reports matched versus returned")
{
	FakeGame fake;
	fake.saves = {
		.directory = "C:\\virtual\\Saves",
		.saves = {
			{ "Zulu", 10 },
			{ "alpha-two", 20 },
			{ "Alpha-one", 20 },
			{ "unknown-time", std::nullopt },
		},
	};
	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, false, fake.Backend());

	const auto result = Invoke(registry, json{
											 { "action", "list" },
											 { "filter", "ALPHA" },
											 { "limit", 1 },
										 });
	CHECK(result.ok);
	CHECK(result.value.at("dir") == "C:\\virtual\\Saves");
	CHECK(result.value.at("count") == 2);
	CHECK(result.value.at("matched") == 2);
	CHECK(result.value.at("returned") == 1);
	CHECK(result.value.at("truncated") == true);
	CHECK(result.value.at("saves")[0].at("name") == "Alpha-one");
	CHECK(result.value.at("saves")[0].at("mtimeUnix") == 20);

	CHECK(Invoke(registry, json{ { "action", "list" }, { "limit", 0 } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "action", "list" }, { "limit", 501 } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "action", "list" }, { "limit", 1.5 } }).errorCode == 400);
	CHECK(Invoke(registry, json{ { "action", "status" }, { "filter", "x" } }).errorCode == 400);
}

TEST_CASE("game list detail is bounded to returned saves and reports per-entry errors")
{
	FakeGame fake;
	fake.saves = {
		.directory = "native",
		.saves = {
			{ "older", 10 },
			{ "newest", 20 },
		},
	};
	fake.metadata = dvb::tools::game::SaveMetadata{
		.formatVersion = 15,
		.saveNumber = 7,
		.characterName = "Nora",
		.level = 12,
		.location = "Cambridge",
		.playTime = "1d.2h.3m",
		.race = "HumanRace",
		.currentExperience = 10.5F,
		.requiredExperience = 20.0F,
		.fileTimeUnix = 0,
		.screenshotWidth = 640,
		.screenshotHeight = 384,
	};
	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, false, fake.Backend());

	const auto result = Invoke(registry, json{
											 { "action", "list" },
											 { "dir", "C:\\readonly\\saves" },
											 { "detail", true },
											 { "limit", 1 },
										 });
	CHECK(result.ok);
	CHECK(result.value.at("dir") == "C:\\readonly\\saves");
	CHECK(result.value.at("count") == 2);
	CHECK(result.value.at("returned") == 1);
	CHECK(result.value.at("metaAvailable") == true);
	CHECK(result.value.at("saves")[0].at("name") == "newest");
	CHECK(result.value.at("saves")[0].at("meta").at("characterName") == "Nora");
	CHECK(result.value.at("saves")[0].at("meta").at("fileTimeUnix") == 0);
	CHECK(fake.listRequests.size() == 1);
	CHECK(fake.listRequests[0].directory == "C:\\readonly\\saves");
	CHECK(fake.metadataRequests.size() == 1);
	CHECK(fake.metadataRequests[0] == "newest");

	fake.metadata.reset();
	fake.metadataError = "truncated Fallout 4 save header";
	fake.metadataRequests.clear();
	const auto malformed = Invoke(registry, json{
												{ "action", "list" },
												{ "detail", true },
												{ "limit", 1 },
											});
	CHECK(malformed.ok);
	CHECK(malformed.value.at("metaAvailable") == false);
	CHECK(malformed.value.at("saves")[0].at("metaError") ==
		  "truncated Fallout 4 save header");
}

TEST_CASE("game save refuses overwrite and load requires an existing save")
{
	FakeGame fake;
	fake.saves.saves = {
		{ "Existing", 20 },
	};
	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, true, fake.Backend());

	CHECK(Invoke(registry, json{ { "action", "save" }, { "name", "existing" } }).errorCode == 409);
	CHECK(Invoke(registry, json{ { "action", "load" }, { "name", "missing" } }).errorCode == 404);
	CHECK(fake.requests.empty());

	const auto save = Invoke(registry, json{ { "action", "save" }, { "name", "DevBench-clean" } });
	CHECK(save.ok);
	CHECK(save.value.at("queued") == true);
	CHECK(save.value.at("action") == "save");
	CHECK(save.value.at("operationId") == 1);
	CHECK(save.value.at("actionCursor") == 17);
	CHECK(save.value.at("eventCursor") == 17);
	CHECK(fake.requests.size() == 1);
	CHECK(fake.requests[0].kind == dvb::tools::GameOperationKind::kSave);
	CHECK(fake.requests[0].name == "DevBench-clean");

	const auto load = Invoke(registry, json{ { "action", "load" }, { "name", "existing" } });
	CHECK(load.ok);
	CHECK(load.value.at("name") == "Existing");
	CHECK(fake.requests.size() == 2);
	CHECK(fake.requests[1].kind == dvb::tools::GameOperationKind::kLoad);
}

TEST_CASE("game mutations pass directory only as a native-directory guard")
{
	FakeGame fake;
	fake.saves.saves = { { "existing", 20 } };
	auto backend = fake.Backend();
	backend.resolveMutationDirectory = [](const std::optional<std::string>&) -> std::string {
		throw dvb::ToolError(
			409, "directory is read-only selection and does not match the native save directory");
	};

	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, true, std::move(backend));
	const auto result = Invoke(registry, json{
											 { "action", "load" },
											 { "name", "existing" },
											 { "dir", "D:\\foreign-saves" },
										 });
	CHECK(result.errorCode == 409);
	CHECK(fake.listRequests.empty());
	CHECK(fake.requests.empty());
}

TEST_CASE("game advanceTime validates signed bounds and reports completion truth")
{
	FakeGame     fake;
	ToolRegistry denied;
	dvb::tools::RegisterGameTool(denied, false, fake.Backend());
	CHECK(Invoke(denied, json{ { "action", "advanceTime" }, { "hours", 1 } }).errorCode == 403);

	ToolRegistry allowed;
	dvb::tools::RegisterGameTool(allowed, true, fake.Backend());
	for (const auto& bad : {
			 json{ { "action", "advanceTime" } },
			 json{ { "action", "advanceTime" }, { "hours", "1" } },
			 json{ { "action", "advanceTime" }, { "hours", 0 } },
			 json{ { "action", "advanceTime" }, { "hours", 100001 } },
			 json{ { "action", "advanceTime" }, { "hours", -100001 } },
		 })
		CHECK(Invoke(allowed, bad).errorCode == 400);

	const auto result =
		Invoke(allowed, json{ { "action", "advanceTime" }, { "hours", -2.5 } });
	CHECK(result.ok);
	CHECK(result.value.at("queued") == false);
	CHECK(result.value.at("completed") == true);
	CHECK(result.value.at("action") == "advanceTime");
	CHECK(result.value.at("hours") == -2.5);
	CHECK(result.value.at("operationId") == 1);
	CHECK(result.value.at("dayDelta") == -1);
	CHECK(result.value.at("year") == 2287);
	CHECK(result.value.at("rawYear") == 287);
}

TEST_CASE("game advanceTime exposes an operation id when completion is still uncertain")
{
	FakeGame fake;
	auto     backend = fake.Backend();
	backend.advanceTime = [](double) {
		return dvb::tools::GameAdvanceReceipt{
			.operationId = 77,
			.actionCursor = 400,
			.completed = false,
		};
	};

	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, true, std::move(backend));
	const auto result =
		Invoke(registry, json{ { "action", "advanceTime" }, { "hours", 1 } });
	CHECK(result.ok);
	CHECK(result.value.at("queued") == true);
	CHECK(result.value.at("completed") == false);
	CHECK(result.value.at("operationId") == 77);
	CHECK(result.value.at("actionCursor") == 400);
}

TEST_CASE("game loadLast resolves an explicit newest name before dispatch")
{
	FakeGame fake;
	fake.saves.saves = {
		{ "older", 100 },
		{ "newest", 300 },
		{ "middle", 200 },
	};
	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, true, fake.Backend());

	const auto result = Invoke(registry, json{ { "action", "loadLast" } });
	CHECK(result.ok);
	CHECK(result.value.at("name") == "newest");
	CHECK(fake.requests.size() == 1);
	CHECK(fake.requests[0].name == "newest");

	fake.saves.saves.clear();
	CHECK(Invoke(registry, json{ { "action", "loadLast" } }).errorCode == 404);
}

TEST_CASE("game operation tracker serializes mutations and correlates lifecycle")
{
	using dvb::tools::GameOperationKind;
	using dvb::tools::GameOperationPhase;
	using dvb::tools::GameOperationTracker;

	GameOperationTracker tracker;
	const auto           first = tracker.TryBegin(GameOperationKind::kLoad, "DevBench-slot", 40);
	CHECK(first.has_value());
	CHECK(first->actionCursor == 40);
	CHECK(!tracker.TryBegin(GameOperationKind::kSave, "other", 40).has_value());
	CHECK(!tracker.ObserveStarted(GameOperationKind::kLoad, "DevBench-slot", 41));
	CHECK(!tracker.ObserveCompleted(GameOperationKind::kLoad, "DevBench-slot", true, 42));
	CHECK(tracker.MarkDispatching(first->operationId));
	CHECK(tracker.MarkQueued(first->operationId));
	CHECK(!tracker.ObserveStarted(GameOperationKind::kLoad, "unrelated", 43));
	CHECK(!tracker.ObserveCompleted(GameOperationKind::kLoad, std::nullopt, true, 44));
	CHECK(tracker.ObserveStarted(
		GameOperationKind::kLoad, "C:\\virtual\\DevBench-slot.fos", 45));
	CHECK(tracker.ObserveCompleted(GameOperationKind::kLoad, std::nullopt, true, 46));

	const auto completed = tracker.GetSnapshot();
	CHECK(completed.has_value());
	CHECK(!completed->active);
	CHECK(completed->phase == GameOperationPhase::kSucceeded);
	CHECK(completed->success == true);
	CHECK(completed->startedCursor == 45);
	CHECK(completed->completedCursor == 46);
	CHECK(tracker.TryBegin(GameOperationKind::kSave, "next", 46).has_value());
}

TEST_CASE("game rejects a second mutation while the backend operation is active")
{
	dvb::tools::GameOperationTracker tracker;
	FakeGame                         fake;
	fake.saves.saves = {};
	auto backend = fake.Backend();
	backend.queueOperation = [&](dvb::tools::GameOperationRequest a_request) {
		const auto operation = tracker.TryBegin(a_request.kind, std::move(a_request.name), 80);
		if (!operation)
			throw dvb::ToolError(409, "another operation is active");
		tracker.MarkDispatching(operation->operationId);
		tracker.MarkQueued(operation->operationId);
		return dvb::tools::GameQueueReceipt{
			.operationId = operation->operationId,
			.actionCursor = operation->actionCursor,
		};
	};

	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, true, std::move(backend));
	CHECK(Invoke(registry, json{ { "action", "save" }, { "name", "first" } }).ok);
	CHECK(Invoke(registry, json{ { "action", "save" }, { "name", "second" } }).errorCode == 409);
}

TEST_CASE("game operation tracker only cancels work that has not started")
{
	using dvb::tools::GameOperationKind;
	using dvb::tools::GameOperationPhase;
	using dvb::tools::GameOperationTracker;

	GameOperationTracker tracker;
	const auto           first = tracker.TryBegin(GameOperationKind::kSave, "first", 5);
	CHECK(first.has_value());
	CHECK(tracker.MarkCancelled(first->operationId, "timed out before start"));
	CHECK(tracker.GetSnapshot()->phase == GameOperationPhase::kCancelled);
	CHECK(!tracker.GetSnapshot()->active);

	const auto second = tracker.TryBegin(GameOperationKind::kSave, "second", 6);
	CHECK(second.has_value());
	CHECK(tracker.MarkDispatching(second->operationId));
	CHECK(tracker.ObserveStarted(GameOperationKind::kSave, "second.fos", 7));
	CHECK(!tracker.MarkCancelled(second->operationId, "too late"));
	CHECK(!tracker.TryBegin(GameOperationKind::kLoad, "third", 8).has_value());
}

TEST_CASE("game operation tracker retains completed calendar results")
{
	using dvb::tools::GameOperationKind;
	using dvb::tools::GameOperationPhase;
	using dvb::tools::GameOperationTracker;

	GameOperationTracker tracker;
	const auto           operation = tracker.TryBegin(GameOperationKind::kAdvanceTime, "-2 hours", 30);
	CHECK(operation.has_value());
	CHECK(operation->operationId == 1);
	CHECK(tracker.MarkDispatching(operation->operationId));
	CHECK(tracker.MarkCompleted(
		operation->operationId,
		json{ { "gameHour", 23.0 }, { "day", 31 }, { "month", 12 }, { "year", 2286 } }));

	const auto completed = tracker.GetSnapshot();
	CHECK(completed.has_value());
	CHECK(completed->phase == GameOperationPhase::kSucceeded);
	CHECK(completed->success == true);
	CHECK(completed->active == false);
	CHECK(completed->result->at("gameHour") == 23.0);
	CHECK(!tracker.MarkQueued(operation->operationId));
	const auto next = tracker.TryBegin(GameOperationKind::kSave, "next", 31);
	CHECK(next.has_value());
	CHECK(next->operationId == 2);
}

TEST_CASE("game tracker accepts synchronous lifecycle during dispatch without inventing save success")
{
	using dvb::tools::GameOperationKind;
	using dvb::tools::GameOperationPhase;
	using dvb::tools::GameOperationTracker;

	GameOperationTracker tracker;
	const auto           save = tracker.TryBegin(GameOperationKind::kSave, "checkpoint", 10);
	CHECK(save.has_value());
	CHECK(tracker.MarkDispatching(save->operationId));
	CHECK(tracker.ObserveStarted(GameOperationKind::kSave, "checkpoint.fos", 11));
	CHECK(tracker.ObserveCompleted(GameOperationKind::kSave, "checkpoint.fos", std::nullopt, 12));
	const auto saved = tracker.GetSnapshot();
	CHECK(saved->phase == GameOperationPhase::kCompleted);
	CHECK(!saved->success.has_value());
	CHECK(!saved->active);
	// Completion occurred synchronously inside dispatch, so a later queue marker
	// must not regress or reclaim the already-terminal operation.
	CHECK(!tracker.MarkQueued(save->operationId));

	const auto loadOk = tracker.TryBegin(GameOperationKind::kLoad, "checkpoint", 12);
	CHECK(loadOk.has_value());
	CHECK(tracker.MarkDispatching(loadOk->operationId));
	CHECK(tracker.ObserveStarted(GameOperationKind::kLoad, "checkpoint", 13));
	CHECK(tracker.ObserveCompleted(GameOperationKind::kLoad, std::nullopt, true, 14));
	CHECK(tracker.GetSnapshot()->phase == GameOperationPhase::kSucceeded);
	CHECK(tracker.GetSnapshot()->success == true);

	const auto loadFailed = tracker.TryBegin(GameOperationKind::kLoad, "checkpoint", 14);
	CHECK(loadFailed.has_value());
	CHECK(tracker.MarkDispatching(loadFailed->operationId));
	CHECK(tracker.ObserveStarted(GameOperationKind::kLoad, "checkpoint", 15));
	CHECK(tracker.ObserveCompleted(GameOperationKind::kLoad, std::nullopt, false, 16));
	CHECK(tracker.GetSnapshot()->phase == GameOperationPhase::kFailed);
	CHECK(tracker.GetSnapshot()->success == false);
}

TEST_CASE("rest and save load operations share one reservation and ID sequence")
{
	using dvb::tools::GameOperationKind;
	using dvb::tools::GameOperationPhase;
	using dvb::tools::GameOperationTracker;

	GameOperationTracker tracker;
	const auto           save = tracker.TryBegin(GameOperationKind::kSave, "checkpoint", 20);
	CHECK(save.has_value());
	CHECK(save->operationId == 1);
	CHECK(!tracker.TryBegin(GameOperationKind::kWait, "wait 8 hours", 20));
	CHECK(tracker.MarkDispatching(save->operationId));
	CHECK(tracker.ObserveStarted(GameOperationKind::kSave, "checkpoint.fos", 21));
	CHECK(tracker.ObserveCompleted(
		GameOperationKind::kSave, "checkpoint.fos", std::nullopt, 22));

	const auto wait = tracker.TryBegin(GameOperationKind::kWait, "wait 8 hours", 22);
	CHECK(wait.has_value());
	CHECK(wait->operationId == 2);
	CHECK(!tracker.TryBegin(GameOperationKind::kLoad, "checkpoint", 22));
	CHECK(tracker.MarkDispatching(wait->operationId));
	CHECK(tracker.MarkQueued(wait->operationId));

	// Save/load lifecycle traffic cannot claim or terminate a rest reservation.
	CHECK(!tracker.ObserveStarted(GameOperationKind::kSave, "autosave1.fos", 23));
	CHECK(!tracker.ObserveCompleted(
		GameOperationKind::kLoad, std::nullopt, true, 24));
	CHECK(tracker.GetSnapshot()->active);
	CHECK(tracker.GetSnapshot()->phase == GameOperationPhase::kQueued);

	const json interrupted{
		{ "outcome", "completed" },
		{ "interrupted", true },
		{ "elapsedGameHours", 2.5 },
		{ "message", "combat interrupted waiting" },
	};
	CHECK(tracker.MarkCompleted(
		wait->operationId, false, interrupted, "combat interrupted waiting"));
	const auto terminal = tracker.GetSnapshot();
	CHECK(terminal.has_value());
	CHECK(terminal->phase == GameOperationPhase::kFailed);
	CHECK(terminal->success == false);
	CHECK(!terminal->active);
	CHECK(terminal->error == "combat interrupted waiting");
	CHECK(terminal->result == interrupted);

	const auto sleep =
		tracker.TryBegin(GameOperationKind::kSleep, "sleep 6 hours at BedRef", 24);
	CHECK(sleep.has_value());
	CHECK(sleep->operationId == 3);
	CHECK(tracker.MarkDispatching(sleep->operationId));
	CHECK(tracker.ObserveStarted(GameOperationKind::kSleep, std::nullopt, 25));
	CHECK(tracker.MarkCompleted(
		sleep->operationId, true, json{ { "outcome", "completed" } }));
	CHECK(tracker.GetSnapshot()->phase == GameOperationPhase::kSucceeded);
}

TEST_CASE("game read-only inspect aliases cannot dispatch mutations")
{
	FakeGame fake;
	fake.status = json{ { "ready", true } };
	fake.saves.saves = { { "Alpha", 20 }, { "Beta", 10 } };

	ToolRegistry registry;
	dvb::tools::RegisterGameTool(registry, true, fake.Backend());
	dvb::tools::RegisterInspectTool(registry, {});

	const auto status =
		registry.Invoke("inspect", json{ { "kind", "saveLoad" } }, ToolContext{});
	CHECK(status.ok);
	CHECK(status.value.at("ready") == true);
	CHECK(status.value.at("actionsAllowed") == true);

	const auto saves = registry.Invoke(
		"inspect", json{ { "kind", "saves" }, { "filter", "bet" }, { "limit", 1 } }, ToolContext{});
	CHECK(saves.ok);
	CHECK(saves.value.at("returned") == 1);
	CHECK(saves.value.at("saves")[0].at("name") == "Beta");

	CHECK(registry.Invoke(
					  "inspect",
					  json{ { "kind", "saveLoad" }, { "action", "save" }, { "name", "smuggled" } },
					  ToolContext{})
			  .errorCode == 400);
	CHECK(registry.Invoke(
					  "inspect",
					  json{ { "kind", "saves" }, { "action", "loadLast" } },
					  ToolContext{})
			  .errorCode == 400);
	CHECK(fake.requests.empty());
}

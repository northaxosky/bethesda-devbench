#include "test_framework.h"

#include "tools/scenario/ScenarioTool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

using dvb::EventBus;
using dvb::json;
using dvb::ToolContext;
using dvb::ToolDescriptor;
using dvb::ToolRegistry;
using dvb::ToolResult;
using dvb::tools::scenario::ScenarioConfig;
using dvb::tools::scenario::ScenarioService;

namespace
{
	using namespace std::chrono_literals;

	ToolDescriptor Descriptor(std::string a_name, bool a_readOnly = false)
	{
		ToolDescriptor descriptor;
		descriptor.name = std::move(a_name);
		descriptor.description = "scenario test tool";
		descriptor.readOnly = a_readOnly;
		return descriptor;
	}

	ToolResult Scenario(ToolRegistry& a_registry, json a_args)
	{
		return a_registry.Invoke("scenario", a_args, ToolContext{});
	}

	json WaitForDone(
		ToolRegistry&             a_registry,
		std::uint64_t             a_runId,
		std::chrono::milliseconds a_timeout = 2s)
	{
		const auto deadline = std::chrono::steady_clock::now() + a_timeout;
		for (;;)
		{
			const auto status = Scenario(
				a_registry,
				json{ { "action", "status" }, { "runId", a_runId } });
			if (!status.ok)
				return json{ { "lookupError", status.errorCode } };
			if (status.value.at("done").get<bool>())
				return status.value;
			if (std::chrono::steady_clock::now() >= deadline)
				return json{ { "pollTimedOut", true } };
			std::this_thread::sleep_for(1ms);
		}
	}

	json Predicate(
		std::string a_tool,
		std::string a_path,
		std::string a_comparison,
		json        a_expected,
		json        a_args = json::object())
	{
		json predicate{
			{ "tool", std::move(a_tool) },
			{ "args", std::move(a_args) },
			{ "path", std::move(a_path) },
		};
		predicate[std::move(a_comparison)] = std::move(a_expected);
		return predicate;
	}
}

TEST_CASE("scenario preflights every step before dispatching the first mutation")
{
	ToolRegistry registry;
	EventBus     events;
	int          mutations = 0;
	registry.Register(Descriptor("mutate"), [&](const json&, const ToolContext&) {
		++mutations;
		return json{ { "ok", true } };
	});
	ScenarioService service(registry, events);
	CHECK(service.Register());

	const auto result = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{ { "tool", "mutate" } },
						   json{ { "wait", "later typo" } },
					   }) },
		});
	CHECK(!result.ok);
	CHECK(result.errorCode == 400);
	CHECK(mutations == 0);
}

TEST_CASE("VALUE assertions distinguish missing null and numeric JSON types")
{
	ToolRegistry registry;
	EventBus     events;
	registry.Register(
		Descriptor("observe", true),
		[](const json&, const ToolContext&) {
			return json{
				{ "nil", nullptr },
				{ "integer", 5 },
				{ "floating", 5.0 },
			};
		});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "continueOnError", true },
			{ "steps", json::array({
						   json{ { "assert", Predicate("observe", "/missing", "eq", nullptr) } },
						   json{ { "assert", Predicate("observe", "/missing", "exists", false) } },
						   json{ { "assert", Predicate("observe", "/nil", "eq", nullptr) } },
						   json{ { "assert", Predicate("observe", "/integer", "eq", 5.0) } },
						   json{ { "assert", Predicate("observe", "/floating", "eq", "5") } },
					   }) },
		});
	CHECK(result.ok);
	CHECK(!result.value.at("ok").get<bool>());
	CHECK(result.value.at("stepsRun") == 5);
	const auto& rows = result.value.at("results");
	CHECK(!rows[0].at("ok").get<bool>());
	CHECK(rows[0].at("missing").get<bool>());
	CHECK(rows[0].at("actual").is_null());
	CHECK(rows[1].at("ok").get<bool>());
	CHECK(rows[1].at("actual") == false);
	CHECK(rows[2].at("ok").get<bool>());
	CHECK(rows[3].at("ok").get<bool>());
	CHECK(!rows[4].at("ok").get<bool>());
	CHECK(rows[4].at("status") == 422);
}

TEST_CASE("VALUE numeric predicates preserve exact 64-bit integer ordering")
{
	constexpr std::uint64_t kTwo53 = 9007199254740992ULL;
	constexpr std::uint64_t kTwo53PlusOne = 9007199254740993ULL;
	constexpr std::uint64_t kMaxUnsigned =
		std::numeric_limits<std::uint64_t>::max();
	constexpr std::int64_t kMaxSigned =
		std::numeric_limits<std::int64_t>::max();

	ToolRegistry registry;
	EventBus     events;
	registry.Register(
		Descriptor("wide-numbers", true),
		[=](const json&, const ToolContext&) {
			return json{
				{ "two53", kTwo53 },
				{ "two53PlusOne", kTwo53PlusOne },
				{ "maxUnsigned", kMaxUnsigned },
				{ "maxSigned", kMaxSigned },
			};
		});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "continueOnError", true },
			{ "steps", json::array({
						   json{ { "assert", Predicate("wide-numbers", "/two53PlusOne", "eq", kTwo53) } },
						   json{ { "assert", Predicate("wide-numbers", "/two53PlusOne", "ne", kTwo53) } },
						   json{ { "assert", Predicate("wide-numbers", "/two53", "lt", kTwo53PlusOne) } },
						   json{ { "assert", Predicate("wide-numbers", "/maxUnsigned", "eq", kMaxUnsigned) } },
						   json{ { "assert", Predicate("wide-numbers", "/maxUnsigned", "gt", kMaxSigned) } },
						   json{ { "assert", Predicate("wide-numbers", "/maxSigned", "lt", kMaxUnsigned) } },
						   json{ { "assert", Predicate("wide-numbers", "/two53PlusOne", "gt", 9007199254740992.0) } },
						   json{ { "assert", Predicate("wide-numbers", "/maxUnsigned", "lt", 18446744073709551616.0) } },
					   }) },
		});
	CHECK(result.ok);
	CHECK(!result.value.at("ok").get<bool>());
	const auto& rows = result.value.at("results");
	CHECK(rows.size() == 8);
	CHECK(!rows[0].at("ok").get<bool>());
	CHECK(rows[1].at("ok").get<bool>());
	CHECK(rows[2].at("ok").get<bool>());
	CHECK(rows[3].at("ok").get<bool>());
	CHECK(rows[4].at("ok").get<bool>());
	CHECK(rows[5].at("ok").get<bool>());
	CHECK(rows[6].at("ok").get<bool>());
	CHECK(rows[7].at("ok").get<bool>());
}

TEST_CASE("VALUE predicates reject mutating tools before a run starts")
{
	ToolRegistry registry;
	EventBus     events;
	int          mutations = 0;
	registry.Register(Descriptor("mutating"), [&](const json&, const ToolContext&) {
		++mutations;
		return json{ { "value", 1 } };
	});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{ { "assert", Predicate("mutating", "/value", "eq", 1) } },
					   }) },
		});
	CHECK(!result.ok);
	CHECK(result.errorCode == 400);
	CHECK(mutations == 0);
}

TEST_CASE("waitUntil polls a read-only observation until true and preserves timeout evidence")
{
	ToolRegistry    registry;
	EventBus        events;
	std::atomic_int calls = 0;
	registry.Register(
		Descriptor("inspect", true),
		[&](const json&, const ToolContext&) {
			const int call = ++calls;
			return json{ { "playerLoaded", call >= 3 } };
		});
	ScenarioService service(registry, events);
	service.Register();

	const auto ready = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{
							   { "waitUntil", "playerLoaded" },
							   { "timeoutMs", 100 },
							   { "pollMs", 1 },
						   },
					   }) },
		});
	CHECK(ready.ok);
	CHECK(ready.value.at("ok").get<bool>());
	CHECK(calls.load() >= 3);
	CHECK(ready.value.at("results")[0].at("actual") == true);

	registry.Register(
		Descriptor("never", true),
		[](const json&, const ToolContext&) { return json{ { "ready", false } }; });
	const auto timeout = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{
							   { "waitUntil", Predicate("never", "/ready", "eq", true) },
							   { "timeoutMs", 5 },
							   { "pollMs", 1 },
						   },
					   }) },
		});
	CHECK(timeout.ok);
	CHECK(!timeout.value.at("ok").get<bool>());
	CHECK(timeout.value.at("status") == "failed");
	CHECK(timeout.value.at("results")[0].at("timedOut").get<bool>());
	CHECK(timeout.value.at("results")[0].at("actual") == false);
	CHECK(timeout.value.at("results")[0].at("expected") == true);
}

TEST_CASE("waitUntil retries only temporary observation errors")
{
	ToolRegistry registry;
	EventBus     events;
	int          temporaryCalls = 0;
	int          malformedCalls = 0;
	registry.Register(
		Descriptor("temporary", true),
		[&](const json&, const ToolContext&) -> json {
			if (++temporaryCalls < 3)
				throw dvb::ToolError(503, "not ready");
			return json{ { "ready", true } };
		});
	registry.Register(
		Descriptor("malformed", true),
		[&](const json&, const ToolContext&) -> json {
			++malformedCalls;
			throw dvb::ToolError(400, "bad observation request");
		});
	ScenarioService service(registry, events);
	service.Register();

	const auto recovered = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{
							   { "waitUntil", Predicate("temporary", "/ready", "eq", true) },
							   { "timeoutMs", 100 },
							   { "pollMs", 1 },
						   },
					   }) },
		});
	CHECK(recovered.ok);
	CHECK(recovered.value.at("ok").get<bool>());
	CHECK(temporaryCalls == 3);

	const auto failed = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{
							   { "waitUntil", Predicate("malformed", "/ready", "eq", true) },
							   { "timeoutMs", 100 },
							   { "pollMs", 1 },
						   },
					   }) },
		});
	CHECK(failed.ok);
	CHECK(!failed.value.at("ok").get<bool>());
	CHECK(failed.value.at("results")[0].at("status") == 400);
	CHECK(malformedCalls == 1);
}

TEST_CASE("waitUntil permits unknown null values to become known without weakening assertions")
{
	ToolRegistry registry;
	EventBus     events;
	int          calls = 0;
	registry.Register(Descriptor("operation", true), [&](const json&, const ToolContext&) {
		return json{ { "success", ++calls < 3 ? json(nullptr) : json(true) } };
	});
	ScenarioService service(registry, events);
	service.Register();
	const auto waiting = Scenario(registry, json{ { "steps", json::array({
																 json{ { "waitUntil", Predicate("operation", "/success", "eq", true) },
																	 { "timeoutMs", 1000 }, { "pollMs", 1 } },
															 }) } });
	CHECK(waiting.ok);
	CHECK(waiting.value.at("ok") == true);
	CHECK(calls == 3);
	CHECK(waiting.value.at("results")[0].at("actual") == true);

	calls = 0;
	const auto asserted = Scenario(registry, json{ { "steps", json::array({
																  json{ { "assert", Predicate("operation", "/success", "eq", true) } },
															  }) } });
	CHECK(asserted.ok);
	CHECK(asserted.value.at("ok") == false);
	CHECK(asserted.value.at("results")[0].at("status") == 422);
	CHECK(calls == 1);
}

TEST_CASE("waitUntil times out on unknown null but rejects a concrete type mismatch")
{
	ToolRegistry registry;
	EventBus     events;
	registry.Register(Descriptor("unknown", true), [](const json&, const ToolContext&) {
		return json{ { "success", nullptr } };
	});
	int malformedCalls = 0;
	registry.Register(Descriptor("malformed", true), [&](const json&, const ToolContext&) {
		++malformedCalls;
		return json{ { "success", "true" } };
	});
	ScenarioService service(registry, events);
	service.Register();
	const auto unknown = Scenario(registry, json{ { "steps", json::array({
																 json{ { "waitUntil", Predicate("unknown", "/success", "eq", true) },
																	 { "timeoutMs", 5 }, { "pollMs", 1 } },
															 }) } });
	CHECK(unknown.ok);
	CHECK(unknown.value.at("ok") == false);
	CHECK(unknown.value.at("results")[0].at("timedOut") == true);
	CHECK(unknown.value.at("results")[0].at("actual").is_null());
	const auto malformed = Scenario(registry, json{ { "steps", json::array({
																   json{ { "waitUntil", Predicate("malformed", "/success", "eq", true) },
																	   { "timeoutMs", 1000 }, { "pollMs", 1 } },
															   }) } });
	CHECK(malformed.ok);
	CHECK(malformed.value.at("ok") == false);
	CHECK(malformed.value.at("results")[0].at("status") == 422);
	CHECK(malformedCalls == 1);
}

TEST_CASE("unsupported legacy lifecycle shorthands fail preflight before mutation")
{
	ToolRegistry registry;
	EventBus     events;
	int          mutations = 0;
	registry.Register(Descriptor("mutate"), [&](const json&, const ToolContext&) {
		++mutations;
		return json{ { "ok", true } };
	});
	ScenarioService service(registry, events);
	service.Register();
	for (const auto name : { "dataLoaded", "saveGame" })
	{
		const auto run = Scenario(registry, json{ { "steps", json::array({
																 json{ { "tool", "mutate" } },
																 json{ { "waitFor", name } },
															 }) } });
		CHECK(!run.ok);
		CHECK(run.errorCode == 400);
	}
	CHECK(mutations == 0);
}

TEST_CASE("waitFor sees a rapid event published inside the preceding tool")
{
	ToolRegistry registry;
	EventBus     events;
	registry.Register(Descriptor("emit"), [&](const json&, const ToolContext&) {
		events.Publish("fixture", json{ { "state", "ready" } });
		return json{ { "emitted", true } };
	});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{ { "tool", "emit" } },
						   json{
							   { "waitFor", json{
												{ "topic", "fixture" },
												{ "match", json{ { "state", "ready" } } },
											} },
							   { "timeoutMs", 20 },
							   { "pollMs", 1 },
						   },
					   }) },
		});
	CHECK(result.ok);
	CHECK(result.value.at("ok").get<bool>());
	CHECK(result.value.at("results")[1].at("available").get<bool>());
	CHECK(result.value.at("results")[1].at("actual").at("payload").at("state") == "ready");
}

TEST_CASE("a valid tool receipt eventCursor refines the pre-dispatch cursor")
{
	ToolRegistry registry;
	EventBus     events;
	registry.Register(Descriptor("emit-refined"), [&](const json&, const ToolContext&) {
		events.Publish("fixture", json{ { "state", "before-action" } });
		const auto eventCursor = events.HeadSeq();
		events.Publish("fixture", json{ { "state", "after-action" } });
		return json{ { "eventCursor", eventCursor } };
	});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{ { "tool", "emit-refined" } },
						   json{
							   { "waitFor", json{ { "topic", "fixture" } } },
							   { "timeoutMs", 20 },
							   { "pollMs", 1 },
						   },
					   }) },
		});
	CHECK(result.ok);
	CHECK(result.value.at("ok").get<bool>());
	CHECK(result.value.at("results")[0].contains("eventCursorUsed"));
	CHECK(
		result.value.at("results")[1].at("actual").at("payload").at("state") ==
		"after-action");
}

TEST_CASE("waitFor resets its cursor between repetitions and ignores stale events")
{
	ToolRegistry registry;
	EventBus     events;
	int          calls = 0;
	registry.Register(Descriptor("emit-once"), [&](const json&, const ToolContext&) {
		if (++calls == 1)
			events.Publish("once", json{ { "value", 1 } });
		return json{ { "call", calls } };
	});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "repeat", 2 },
			{ "steps", json::array({
						   json{ { "tool", "emit-once" } },
						   json{
							   { "waitFor", json{ { "topic", "once" } } },
							   { "timeoutMs", 5 },
							   { "pollMs", 1 },
						   },
					   }) },
		});
	CHECK(result.ok);
	CHECK(!result.value.at("ok").get<bool>());
	CHECK(result.value.at("stepsRun") == 4);
	CHECK(result.value.at("results")[1].at("ok").get<bool>());
	CHECK(!result.value.at("results")[3].at("ok").get<bool>());
	CHECK(result.value.at("results")[3].at("repetition") == 1);
}

TEST_CASE("waitFor reports event ring loss instead of blessing an incomplete trace")
{
	ToolRegistry registry;
	EventBus     events;
	registry.Register(Descriptor("flood"), [&](const json&, const ToolContext&) {
		for (int index = 0; index < 300; ++index)
			events.Publish("noise", json{ { "index", index } });
		events.Publish("target", json{ { "ready", true } });
		return json{ { "ok", true } };
	});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{ { "tool", "flood" } },
						   json{
							   { "waitFor", json{ { "topic", "target" } } },
							   { "timeoutMs", 20 },
							   { "pollMs", 1 },
						   },
					   }) },
		});
	CHECK(result.ok);
	CHECK(!result.value.at("ok").get<bool>());
	CHECK(result.value.at("results")[1].at("eventHistoryLost").get<bool>());
	CHECK(result.value.at("results")[1].at("status") == 409);
}

TEST_CASE("embedded failures fail fast unless continueOnError is explicit")
{
	ToolRegistry registry;
	EventBus     events;
	int          afterCalls = 0;
	registry.Register(
		Descriptor("domain-failure"),
		[](const json&, const ToolContext&) {
			return json{ { "ok", false }, { "errorCode", "fixture_failed" }, { "error", "fixture failed" } };
		});
	registry.Register(Descriptor("after"), [&](const json&, const ToolContext&) {
		++afterCalls;
		return json{ { "ok", true } };
	});
	ScenarioService service(registry, events);
	service.Register();

	const json steps = json::array({
		json{ { "tool", "domain-failure" } },
		json{ { "tool", "after" } },
	});
	const auto stopped = Scenario(registry, json{ { "steps", steps } });
	CHECK(stopped.ok);
	CHECK(!stopped.value.at("ok").get<bool>());
	CHECK(stopped.value.at("aborted").get<bool>());
	CHECK(stopped.value.at("stepsRun") == 1);
	CHECK(stopped.value.at("results")[0].at("errorCode") == "fixture_failed");
	CHECK(afterCalls == 0);

	const auto continued = Scenario(
		registry,
		json{ { "steps", steps }, { "continueOnError", true } });
	CHECK(continued.ok);
	CHECK(!continued.value.at("ok").get<bool>());
	CHECK(!continued.value.at("aborted").get<bool>());
	CHECK(continued.value.at("stepsRun") == 2);
	CHECK(afterCalls == 1);
}

TEST_CASE("async status retains a failed transcript")
{
	ToolRegistry registry;
	EventBus     events;
	registry.Register(
		Descriptor("observe", true),
		[](const json&, const ToolContext&) { return json{ { "value", 1 } }; });
	ScenarioService service(registry, events);
	service.Register();

	const auto queued = Scenario(
		registry,
		json{
			{ "async", true },
			{ "steps", json::array({
						   json{ { "assert", Predicate("observe", "/value", "eq", 2) } },
					   }) },
		});
	CHECK(queued.ok);
	CHECK(queued.value.at("queued").get<bool>());
	const auto status =
		WaitForDone(registry, queued.value.at("runId").get<std::uint64_t>());
	CHECK(!status.contains("pollTimedOut"));
	CHECK(status.at("done").get<bool>());
	CHECK(!status.at("ok").get<bool>());
	CHECK(!status.at("result").at("ok").get<bool>());
	CHECK(status.at("result").at("results").size() == 1);
}

TEST_CASE("cancellation is cooperative stops later commands and concurrent runs conflict")
{
	ToolRegistry            registry;
	EventBus                events;
	std::mutex              gateMutex;
	std::condition_variable gateCv;
	bool                    entered = false;
	bool                    release = false;
	int                     laterCalls = 0;
	registry.Register(Descriptor("blocking"), [&](const json&, const ToolContext&) {
		std::unique_lock lock(gateMutex);
		entered = true;
		gateCv.notify_all();
		gateCv.wait(lock, [&] { return release; });
		return json{ { "ok", true } };
	});
	registry.Register(Descriptor("later"), [&](const json&, const ToolContext&) {
		++laterCalls;
		return json{ { "ok", true } };
	});
	ScenarioService service(registry, events);
	service.Register();

	const auto queued = Scenario(
		registry,
		json{
			{ "async", true },
			{ "steps", json::array({
						   json{ { "tool", "blocking" } },
						   json{ { "tool", "later" } },
					   }) },
		});
	CHECK(queued.ok);
	const auto runId = queued.value.at("runId").get<std::uint64_t>();
	{
		std::unique_lock lock(gateMutex);
		CHECK(gateCv.wait_for(lock, 1s, [&] { return entered; }));
	}

	const auto conflict =
		Scenario(registry, json{ { "steps", json::array() } });
	CHECK(!conflict.ok);
	CHECK(conflict.errorCode == 409);
	const auto cancel = Scenario(
		registry,
		json{ { "action", "cancel" }, { "runId", runId } });
	CHECK(cancel.ok);
	CHECK(cancel.value.at("cancelRequested").get<bool>());
	{
		std::lock_guard lock(gateMutex);
		release = true;
	}
	gateCv.notify_all();

	const auto status = WaitForDone(registry, runId);
	CHECK(status.at("done").get<bool>());
	CHECK(status.at("result").at("cancelled").get<bool>());
	CHECK(status.at("result").at("status") == "cancelled");
	CHECK(laterCalls == 0);
}

TEST_CASE("scenario rejects recursion during preflight")
{
	ToolRegistry    registry;
	EventBus        events;
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{ { "tool", "scenario" }, { "args", json{ { "steps", json::array() } } } },
					   }) },
		});
	CHECK(!result.ok);
	CHECK(result.errorCode == 400);
}

TEST_CASE("scenario completed history is bounded without evicting the active run")
{
	ToolRegistry   registry;
	EventBus       events;
	ScenarioConfig config;
	config.historyLimit = 2;
	ScenarioService service(registry, events, config);
	service.Register();

	const auto first = Scenario(registry, json{ { "steps", json::array() } });
	const auto second = Scenario(registry, json{ { "steps", json::array() } });
	const auto third = Scenario(registry, json{ { "steps", json::array() } });
	CHECK(first.ok && second.ok && third.ok);

	const auto expired = Scenario(
		registry,
		json{
			{ "action", "status" },
			{ "runId", first.value.at("runId") },
		});
	CHECK(!expired.ok);
	CHECK(expired.errorCode == 404);
	const auto retained = Scenario(
		registry,
		json{
			{ "action", "status" },
			{ "runId", third.value.at("runId") },
		});
	CHECK(retained.ok);
	CHECK(retained.value.at("done").get<bool>());
}

TEST_CASE("pose preflight rejects a bad coordinate before any mutation")
{
	ToolRegistry registry;
	EventBus     events;
	int          mutations = 0;
	registry.Register(Descriptor("mutate"), [&](const json&, const ToolContext&) {
		++mutations;
		return json::object();
	});
	registry.Register(Descriptor("console"), [&](const json&, const ToolContext&) {
		++mutations;
		return json{ { "queued", false } };
	});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{ { "tool", "mutate" } },
						   json{ { "pose", json::array({ 1, 2, 3, 4, "bad" }) } },
					   }) },
		});
	CHECK(!result.ok);
	CHECK(result.errorCode == 400);
	CHECK(mutations == 0);
}

TEST_CASE("scenario cleans up only freecam acquired by that run")
{
	{
		ToolRegistry registry;
		EventBus events;
		int gets = 0;
		int releases = 0;
		registry.Register(
			Descriptor("camera"),
			[&](const json& a_args, const ToolContext&) {
				const auto action =
					a_args.value("action", std::string("get"));
				if (action == "get")
				{
					++gets;
					return json{
						{ "pov", "third" },
						{ "freeCam", true },
						{ "freeCamOwned", true },
					};
				}
				if (a_args.value("on", false))
					return json{
						{ "completed", true },
						{ "changed", false },
						{ "freeCam", true },
						{ "freeCamOwned", true },
					};
				++releases;
				return json::object();
			});
		ScenarioService service(registry, events);
		service.Register();

		const auto result = Scenario(
			registry,
			json{
				{ "steps",
					json::array({
						json{
							{ "tool", "camera" },
							{ "args",
								json{
									{ "action", "freecam" },
									{ "on", true },
								} },
						},
					}) },
			});
		CHECK_MESSAGE(result.ok, result.errorMessage);
		CHECK(gets == 1);
		CHECK(releases == 0);
		CHECK(!result.value.contains("cleanup"));
	}

	{
		ToolRegistry registry;
		EventBus events;
		int gets = 0;
		int releases = 0;
		registry.Register(
			Descriptor("camera"),
			[&](const json& a_args, const ToolContext&) {
				const auto action =
					a_args.value("action", std::string("get"));
				if (action == "get")
				{
					++gets;
					return json{
						{ "pov", "first" },
						{ "freeCam", false },
						{ "freeCamOwned", false },
					};
				}
				if (a_args.value("on", false))
					return json{
						{ "completed", true },
						{ "changed", true },
						{ "freeCam", true },
						{ "freeCamOwned", true },
					};
				++releases;
				return json{
					{ "completed", true },
					{ "changed", true },
					{ "pov", "first" },
					{ "freeCam", false },
					{ "freeCamOwned", false },
				};
			});
		ScenarioService service(registry, events);
		service.Register();

		const auto result = Scenario(
			registry,
			json{
				{ "steps",
					json::array({
						json{
							{ "tool", "camera" },
							{ "args",
								json{
									{ "action", "freecam" },
									{ "on", true },
								} },
						},
					}) },
			});
		CHECK_MESSAGE(result.ok, result.errorMessage);
		CHECK(gets == 1);
		CHECK(releases == 1);
		CHECK(result.value.at("cleanup").at("camera").at("acquired") == true);
		CHECK(result.value.at("cleanup").at("camera").at("observedPov") ==
		      "first");
	}

	{
		ToolRegistry registry;
		EventBus events;
		int releases = 0;
		registry.Register(
			Descriptor("camera"),
			[&](const json& a_args, const ToolContext&) -> json {
				const auto action =
					a_args.value("action", std::string("get"));
				if (action == "get")
					return json{
						{ "pov", "third" },
						{ "freeCam", false },
						{ "freeCamOwned", false },
					};
				if (a_args.value("on", false))
					return json{
						{ "completed", true },
						{ "changed", true },
						{ "freeCam", true },
						{ "freeCamOwned", true },
					};
				++releases;
				throw dvb::ToolError(
					504,
					"camera mutation started; completion is uncertain and must not be retried");
			});
		ScenarioService service(registry, events);
		service.Register();

		const auto result = Scenario(
			registry,
			json{
				{ "steps",
					json::array({
						json{
							{ "tool", "camera" },
							{ "args",
								json{
									{ "action", "freecam" },
									{ "on", true },
								} },
						},
					}) },
			});
		CHECK_MESSAGE(result.ok, result.errorMessage);
		CHECK(releases == 1);
		CHECK(result.value.at("ok") == false);
		CHECK(result.value.at("cleanupUncertain") == true);
		CHECK(result.value.at("cleanup")
			      .at("camera")
			      .at("releaseUncertain") == true);
	}
}

TEST_CASE("soft scene mismatch reaches capture context and inconclusive capture fails")
{
	ToolRegistry registry;
	EventBus     events;
	json         captureArgs;
	registry.Register(
		Descriptor("inspect", true),
		[](const json&, const ToolContext&) {
			return json{
				{ "playerLoaded", true },
				{ "cell", json{ { "formId", 2 } } },
				{ "worldspace", nullptr },
			};
		});
	registry.Register(
		Descriptor("capture"),
		[&](const json& a_args, const ToolContext&) {
			captureArgs = a_args;
			return json{
				{ "checkpointId", "visual" },
				{ "passed", true },
				{ "inconclusive", true },
				{ "inconclusiveReason", "native capture has no pre-UI proof" },
			};
		});
	ScenarioService service(registry, events);
	service.Register();

	const auto result = Scenario(
		registry,
		json{
			{ "steps", json::array({
						   json{
							   { "assert", "scene" },
							   { "interior", true },
							   { "cellFormID", 1 },
							   { "soft", true },
							   { "timeoutMs", 5 },
							   { "pollMs", 1 },
						   },
						   json{
							   { "tool", "capture" },
							   { "args", json{ { "checkpointId", "visual" } } },
						   },
					   }) },
		});
	CHECK(result.ok);
	CHECK(result.value.at("ok") == false);
	CHECK(result.value.at("results")[0].at("ok") == true);
	CHECK(result.value.at("results")[0].at("sceneMismatch") == true);
	CHECK(result.value.at("results")[1].at("status") == 422);
	CHECK(result.value.at("results")[1].at("errorCode") ==
	      "capture_inconclusive");
	CHECK(captureArgs.at("runId").get<std::uint64_t>() > 0);
	CHECK(captureArgs.at("repeat") == 0);
	CHECK(captureArgs.at("sceneMismatch") == true);
}

TEST_CASE("modal acceptance reports changed dialogs and never accepts after cancellation")
{
	{
		ToolRegistry registry;
		EventBus     events;
		int          accepts = 0;
		registry.Register(
			Descriptor("menu"),
			[&](const json& a_args, const ToolContext&) -> json {
				if (a_args.at("action") == "describe")
					return json{
						{ "messageBoxOpen", true },
						{ "headerText", "Missing Content" },
						{ "bodyText", "This content is no longer present." },
						{ "buttons", json::array({ "Continue", "Cancel" }) },
						{ "fingerprint",
							json{
								{ "headerText", "Missing Content" },
								{ "bodyText", "This content is no longer present." },
								{ "buttons", json::array({ "Continue", "Cancel" }) },
							} },
					};
				++accepts;
				throw dvb::ToolError(
					409, "the active MessageBoxMenu changed before acceptance");
			});
		ScenarioService service(registry, events);
		service.Register();
		const auto changed = Scenario(
			registry,
			json{
				{ "steps", json::array({
							   json{
								   { "waitFor",
									   json{ { "topic", "never" } } },
								   { "timeoutMs", 50 },
								   { "pollMs", 1 },
								   { "acceptModal",
									   json{
										   { "matchBody", "no longer present" },
										   { "buttonIndex", 0 },
									   } },
							   },
						   }) },
			});
		CHECK(changed.ok);
		CHECK(changed.value.at("ok") == false);
		CHECK(changed.value.at("results")[0].at("status") == 409);
		CHECK(accepts == 1);
	}

	{
		ToolRegistry           registry;
		EventBus               events;
		std::mutex             mutex;
		std::condition_variable cv;
		bool                   describeEntered = false;
		bool                   releaseDescribe = false;
		int                    accepts = 0;
		registry.Register(
			Descriptor("menu"),
			[&](const json& a_args, const ToolContext&) -> json {
				if (a_args.at("action") == "accept")
				{
					++accepts;
					return json{ { "accepted", true } };
				}
				std::unique_lock lock(mutex);
				describeEntered = true;
				cv.notify_all();
				cv.wait(lock, [&] { return releaseDescribe; });
				return json{
					{ "messageBoxOpen", true },
					{ "bodyText", "This content is no longer present." },
					{ "buttons", json::array({ "Continue" }) },
					{ "fingerprint",
						json{
							{ "headerText", "" },
							{ "bodyText", "This content is no longer present." },
							{ "buttons", json::array({ "Continue" }) },
						} },
				};
			});
		ScenarioService service(registry, events);
		service.Register();
		const auto queued = Scenario(
			registry,
			json{
				{ "async", true },
				{ "steps", json::array({
							   json{
								   { "waitFor",
									   json{ { "topic", "never" } } },
								   { "timeoutMs", 1000 },
								   { "pollMs", 1 },
								   { "acceptModal",
									   json{
										   { "matchBody", "no longer present" },
									   } },
							   },
						   }) },
			});
		CHECK(queued.ok);
		{
			std::unique_lock lock(mutex);
			CHECK(cv.wait_for(lock, 1s, [&] { return describeEntered; }));
		}
		const auto cancelled = Scenario(
			registry,
			json{
				{ "action", "cancel" },
				{ "runId", queued.value.at("runId") },
			});
		CHECK(cancelled.ok);
		{
			const std::lock_guard lock(mutex);
			releaseDescribe = true;
		}
		cv.notify_all();
		const auto done = WaitForDone(
			registry, queued.value.at("runId").get<std::uint64_t>());
		CHECK(done.at("result").at("cancelled") == true);
		CHECK(accepts == 0);
	}
}

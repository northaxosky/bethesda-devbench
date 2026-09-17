#include "test_framework.h"

#include "tools/rest/RestTool.h"

#include <mutex>
#include <thread>

using dvb::ToolContext;
using dvb::ToolRegistry;
using namespace dvb::tools::rest;
using namespace std::chrono_literals;

namespace
{
	struct FakeRestBackend
	{
		std::mutex                mutex;
		std::vector<RestCommand>  queued;
		std::optional<RestResult> immediate;
		std::optional<int>        lastHours;
		bool                      beginGate = false;
		int                       dispatchCount = 0;

		RestBackend Backend()
		{
			return {
				.implementation = "test.sleep-wait",
				.available = true,
				.runtimeValidated = false,
				.dispatch = [this](RestCommand a_command) {
					std::optional<RestResult> result;
					{
						const std::lock_guard lock{ mutex };
						++dispatchCount;
						lastHours = a_command.hours;
						if (beginGate)
						{
							CHECK(a_command.gate->TryBegin());
							CHECK(a_command.markDispatching());
							a_command.gate->Finish();
						}
						if (immediate)
						{
							result = immediate;
							immediate.reset();
						}
						else
						{
							if (beginGate)
								CHECK(a_command.markQueued());
							queued.push_back(std::move(a_command));
							return RestSubmission{ true, std::nullopt };
						}
					}
					if (a_command.complete)
						a_command.complete(*result);
					return RestSubmission{ false, result };
				},
			};
		}

		RestCommand Queued(std::size_t a_index)
		{
			const std::lock_guard lock{ mutex };
			return queued.at(a_index);
		}
	};

	struct FakeCoordinator
	{
		std::mutex    mutex;
		std::uint64_t nextId = 0;
		bool          active = false;
		int           dispatchingCount = 0;
		int           queuedCount = 0;
		int           finishCount = 0;
		int           failCount = 0;
		int           cancelCount = 0;

		RestOperationCoordinator Coordinator()
		{
			return {
				.tryBegin = [this](RestKind, int, const std::optional<std::string>&) {
					const std::lock_guard lock{ mutex };
					if (active)
						return std::optional<RestOperationReservation>{};
					active = true;
					return std::optional{ RestOperationReservation{
						.operationId = ++nextId,
						.actionCursor = 100 + nextId,
					} }; },
				.markDispatching = [this](std::uint64_t) {
					const std::lock_guard lock{ mutex };
					++dispatchingCount;
					return active; },
				.markQueued = [this](std::uint64_t) {
					const std::lock_guard lock{ mutex };
					++queuedCount;
					return active; },
				.finish = [this](std::uint64_t, const RestResult&) {
					const std::lock_guard lock{ mutex };
					++finishCount;
					active = false;
					return true; },
				.fail = [this](std::uint64_t, std::string) {
					const std::lock_guard lock{ mutex };
					++failCount;
					active = false;
					return true; },
				.cancelReserved = [this](std::uint64_t, std::string) {
					const std::lock_guard lock{ mutex };
					++cancelCount;
					active = false;
					return true; },
			};
		}
	};

	dvb::ToolResult Invoke(ToolRegistry& a_registry, std::string_view a_tool, dvb::json a_args)
	{
		return a_registry.Invoke(a_tool, a_args, ToolContext{});
	}
}

TEST_CASE("wait and sleep validate permission hours and furniture target before dispatch")
{
	FakeRestBackend deniedBackend;
	FakeCoordinator deniedCoordinator;
	ToolRegistry    deniedRegistry;
	auto            denied =
		RegisterRestTools(deniedRegistry, false, deniedBackend.Backend(),
			deniedCoordinator.Coordinator(), { 1ms });
	denied->SetReady(true);

	const auto status =
		Invoke(deniedRegistry, "wait", dvb::json{ { "action", "status" } });
	CHECK(status.ok);
	CHECK(status.value["capabilities"]["available"] == true);
	CHECK(status.value["capabilities"]["nativeRuntimeValidated"] == false);
	CHECK(Invoke(deniedRegistry, "wait", dvb::json{ { "hours", 1 } }).errorCode == 403);
	CHECK(deniedBackend.dispatchCount == 0);

	FakeRestBackend allowedBackend;
	FakeCoordinator allowedCoordinator;
	ToolRegistry    allowedRegistry;
	auto            allowed =
		RegisterRestTools(allowedRegistry, true, allowedBackend.Backend(),
			allowedCoordinator.Coordinator(), { 1ms });
	allowed->SetReady(true);

	for (const auto& args : {
			 dvb::json::array(),
			 dvb::json{ { "hours", "1" } },
			 dvb::json{ { "hours", 0 } },
			 dvb::json{ { "hours", 100001 } },
			 dvb::json{ { "hours", 1 }, { "target", "0x1" } },
		 })
		CHECK(Invoke(allowedRegistry, "wait", args).errorCode == 400);

	CHECK(Invoke(allowedRegistry, "sleep", dvb::json{ { "hours", 1 } }).errorCode == 400);
	CHECK(Invoke(allowedRegistry, "sleep",
			  dvb::json{ { "hours", 1 }, { "target", "" } })
			  .errorCode == 400);
	CHECK(allowedBackend.dispatchCount == 0);

	allowedBackend.beginGate = true;
	allowedBackend.immediate = RestResult{
		RestResultStatus::kRefused,
		false,
		std::nullopt,
		"loaded menu restricted the requested hours",
	};
	const auto moddedHours =
		Invoke(allowedRegistry, "wait", dvb::json{ { "hours", 25 } });
	CHECK(moddedHours.ok);
	CHECK(moddedHours.value["outcome"] == "refused");
	CHECK(allowedBackend.dispatchCount == 1);
	CHECK(allowedBackend.lastHours == 25);
	CHECK(allowedCoordinator.finishCount == 1);
	CHECK(allowedCoordinator.failCount == 0);

	allowed->Shutdown();
	denied->Shutdown();
}

TEST_CASE("wait listener timeout cancels a command that never began")
{
	FakeRestBackend fake;
	FakeCoordinator coordinator;
	ToolRegistry    registry;
	auto            service = RegisterRestTools(
		registry, true, fake.Backend(), coordinator.Coordinator(), { 2ms });
	service->SetReady(true);

	const auto result = Invoke(registry, "wait", dvb::json{ { "hours", 2 } });
	CHECK(!result.ok);
	CHECK(result.errorCode == 504);
	CHECK(fake.dispatchCount == 1);
	CHECK(coordinator.cancelCount == 1);

	const auto command = fake.Queued(0);
	CHECK(!command.gate->TryBegin());
	const auto status =
		Invoke(registry, "wait", dvb::json{ { "action", "status" } });
	CHECK(status.ok);
	CHECK(status.value["operation"]["phase"] == "cancelled");
	CHECK(status.value["operation"]["active"] == false);
	service->Shutdown();
}

TEST_CASE("shared wait sleep reservation survives timeout and records late completion")
{
	FakeRestBackend fake;
	FakeCoordinator coordinator;
	fake.beginGate = true;
	ToolRegistry registry;
	auto         service = RegisterRestTools(
		registry, true, fake.Backend(), coordinator.Coordinator(), { 2ms });
	service->SetReady(true);

	const auto wait = Invoke(registry, "wait", dvb::json{ { "hours", 3 } });
	CHECK(wait.ok);
	CHECK(wait.value["phase"] == "uncertain");
	CHECK(wait.value["active"] == true);
	CHECK(wait.value["uncertain"] == true);

	const auto conflict = Invoke(registry, "sleep",
		dvb::json{ { "hours", 2 }, { "target", "0x00001234" } });
	CHECK(!conflict.ok);
	CHECK(conflict.errorCode == 409);
	CHECK(fake.dispatchCount == 1);
	CHECK(coordinator.dispatchingCount == 1);
	CHECK(coordinator.queuedCount == 1);

	auto command = fake.Queued(0);
	command.complete(RestResult{
		RestResultStatus::kCompleted,
		false,
		3.0,
		"native finish observed",
		dvb::json{ { "nativeCompleted", true } },
	});

	const auto status =
		Invoke(registry, "sleep", dvb::json{ { "action", "status" } });
	CHECK(status.ok);
	CHECK(status.value["operation"]["phase"] == "completed");
	CHECK(status.value["operation"]["completed"] == true);
	CHECK(status.value["operation"]["active"] == false);
	CHECK(status.value["operation"]["elapsedGameHours"] == 3.0);
	CHECK(status.value["operation"]["nativeCompleted"] == true);
	CHECK(coordinator.finishCount == 1);
	service->Shutdown();
}

TEST_CASE("native queued completion can finish within the listener budget")
{
	struct AsyncBackend
	{
		std::jthread worker;

		RestBackend Backend()
		{
			return {
				.implementation = "test.async",
				.available = true,
				.dispatch = [this](RestCommand a_command) {
					worker = std::jthread([command = std::move(a_command)] {
						if (!command.gate->TryBegin())
						{
							command.complete(RestResult{
								RestResultStatus::kCancelled,
								false,
								std::nullopt,
								"cancelled",
							});
							return;
						}
						if (!command.markDispatching())
						{
							command.complete(RestResult{
								RestResultStatus::kCancelled,
								false,
								std::nullopt,
								"reservation lost",
							});
							return;
						}
						command.gate->Finish();
						if (!command.markQueued())
						{
							command.complete(RestResult{
								RestResultStatus::kFailed,
								false,
								std::nullopt,
								"queue transition failed",
							});
							return;
						}
						std::this_thread::sleep_for(5ms);
						command.complete(RestResult{
							RestResultStatus::kCompleted,
							true,
							1.25,
							"interrupted",
						});
					});
					return RestSubmission{ true, std::nullopt }; },
				.shutdown = [this] {
					if (worker.joinable())
						worker.join(); },
			};
		}
	} fake;

	FakeCoordinator coordinator;
	ToolRegistry    registry;
	auto            service = RegisterRestTools(
		registry, true, fake.Backend(), coordinator.Coordinator(), { 100ms });
	service->SetReady(true);

	const auto result = Invoke(registry, "sleep",
		dvb::json{ { "hours", 2 }, { "target", "BedRef" } });
	CHECK(result.ok);
	CHECK(result.value["phase"] == "completed");
	CHECK(result.value["completed"] == true);
	CHECK(result.value["interrupted"] == true);
	CHECK(result.value["elapsedGameHours"] == 1.25);
	CHECK(!RestResultSucceeded(RestResult{
		RestResultStatus::kCompleted, true, 1.25, "interrupted" }));
	CHECK(coordinator.finishCount == 1);
	CHECK(coordinator.failCount == 0);
	service->Shutdown();
}

TEST_CASE("native refusal is a completed non-mutating result")
{
	FakeRestBackend fake;
	FakeCoordinator coordinator;
	fake.beginGate = true;
	fake.immediate = RestResult{
		RestResultStatus::kRefused,
		false,
		std::nullopt,
		"Fallout 4 refused the native gate",
	};
	ToolRegistry registry;
	auto         service = RegisterRestTools(
		registry, true, fake.Backend(), coordinator.Coordinator(), { 50ms });
	service->SetReady(true);

	const auto result = Invoke(registry, "wait", dvb::json{ { "hours", 1 } });
	CHECK(result.ok);
	CHECK(result.value["phase"] == "refused");
	CHECK(result.value["completed"] == false);
	CHECK(result.value["outcome"] == "refused");
	CHECK(result.value["message"] == "Fallout 4 refused the native gate");
	CHECK(coordinator.finishCount == 1);
	CHECK(coordinator.failCount == 0);
	CHECK(!RestResultSucceeded(RestResult{
		RestResultStatus::kRefused, false, std::nullopt,
		"Fallout 4 refused the native gate" }));
	service->Shutdown();
}

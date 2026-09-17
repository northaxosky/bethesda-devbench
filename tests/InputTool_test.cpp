#include "test_framework.h"

#include "EventBus.h"
#include "tools/input/InputTool.h"

#include <deque>
#include <mutex>
#include <thread>

using dvb::EventBus;
using dvb::ToolContext;
using dvb::ToolRegistry;
using namespace dvb::tools::input;

namespace
{
	struct FakeBackend
	{
		std::mutex                 mutex;
		std::vector<ButtonCommand> queued;
		std::deque<DispatchResult> immediate;
		int                        dispatchCount = 0;

		KeyboardBackend Backend()
		{
			return KeyboardBackend{
				.injection = "test.queue",
				.available = true,
				.dispatch = [this](ButtonCommand a_command) {
					std::optional<DispatchResult> result;
					{
						const std::lock_guard lock{ mutex };
						++dispatchCount;
						if (!immediate.empty())
						{
							result = immediate.front();
							immediate.pop_front();
						}
						else
						{
							queued.push_back(std::move(a_command));
							return DispatchSubmission{ true, std::nullopt };
						}
					}

					if (!a_command.gate->TryBegin())
						*result = { DispatchStatus::kCancelled, -1, false, "cancelled" };
					a_command.gate->Finish();
					if (a_command.complete)
						a_command.complete(*result);
					return DispatchSubmission{ false, result };
				},
			};
		}

		void PushImmediate(DispatchResult a_result)
		{
			const std::lock_guard lock{ mutex };
			immediate.push_back(std::move(a_result));
		}

		DispatchStatus RunQueued(std::size_t a_index, DispatchResult a_result)
		{
			ButtonCommand command;
			{
				const std::lock_guard lock{ mutex };
				command = std::move(queued.at(a_index));
			}
			if (!command.gate->TryBegin())
				a_result = { DispatchStatus::kCancelled, -1, false, "cancelled" };
			command.gate->Finish();
			if (command.complete)
				command.complete(a_result);
			return a_result.status;
		}

		std::size_t QueuedCount()
		{
			const std::lock_guard lock{ mutex };
			return queued.size();
		}
	};

	ToolContext Owner(std::string a_id)
	{
		return ToolContext{ .clientId = std::move(a_id) };
	}
}

TEST_CASE("input permission is fail closed while discovery stays available")
{
	EventBus    events;
	FakeBackend fake;
	auto        service = std::make_shared<InputService>(events, false, fake.Backend());
	service->SetReady(true);

	const auto capabilities = service->Handle(nlohmann::json::object(), Owner("one"));
	CHECK(capabilities["capabilities"]["keyboard"]["ready"] == true);
	CHECK(capabilities["contract"]["version"]["major"] == 2);
	CHECK(capabilities["capabilities"]["keyboard"]["version"] == 1);

	ToolRegistry        registry;
	const std::weak_ptr weak = service;
	registry.Register(BuildInputDescriptor(), [weak](const auto& a_args, const auto& a_context) {
		return weak.lock()->Handle(a_args, a_context);
	});
	const auto denied =
		registry.Invoke("input", nlohmann::json{ { "action", "down" }, { "key", "a" } }, Owner("one"));
	CHECK(!denied.ok);
	CHECK(denied.errorCode == 403);
	CHECK(fake.dispatchCount == 0);
	service->Shutdown();
}

TEST_CASE("input owner conflict does not enqueue another native down")
{
	EventBus    events;
	FakeBackend fake;
	fake.PushImmediate({ DispatchStatus::kApplied, 10, false, {} });
	fake.PushImmediate({ DispatchStatus::kApplied, 11, false, {} });
	auto service = std::make_shared<InputService>(events, true, fake.Backend());
	service->SetReady(true);

	const auto down =
		service->Handle(nlohmann::json{ { "action", "down" }, { "key", "leftShift" } }, Owner("one"));
	CHECK(down["pending"] == false);

	ToolRegistry        registry;
	const std::weak_ptr weak = service;
	registry.Register(BuildInputDescriptor(), [weak](const auto& a_args, const auto& a_context) {
		return weak.lock()->Handle(a_args, a_context);
	});
	const auto conflict = registry.Invoke("input",
		nlohmann::json{ { "action", "down" }, { "key", "leftShift" } }, Owner("two"));
	CHECK(!conflict.ok);
	CHECK(conflict.errorCode == 409);
	CHECK(fake.dispatchCount == 1);

	CHECK_NOTHROW(service->Handle(
		nlohmann::json{ { "action", "up" }, { "key", "leftShift" } }, Owner("one")));
	service->Shutdown();
}

TEST_CASE("physical key conflict rejects synthetic down without retaining a lease")
{
	EventBus    events;
	FakeBackend fake;
	fake.PushImmediate(
		{ DispatchStatus::kPhysicalConflict, 12, true, "physical key is down" });
	auto service = std::make_shared<InputService>(events, true, fake.Backend());
	service->SetReady(true);

	ToolRegistry        registry;
	const std::weak_ptr weak = service;
	registry.Register(BuildInputDescriptor(), [weak](const auto& a_args, const auto& a_context) {
		return weak.lock()->Handle(a_args, a_context);
	});
	const auto result = registry.Invoke(
		"input", nlohmann::json{ { "action", "down" }, { "key", "space" } }, Owner("one"));
	CHECK(!result.ok);
	CHECK(result.errorCode == 409);
	CHECK(service->Handle(nlohmann::json{ { "action", "status" } }, Owner("one"))["held"].empty());
	service->Shutdown();
}

TEST_CASE("cancelled queued down cannot affect a newer generation")
{
	EventBus    events;
	FakeBackend fake;
	auto        service = std::make_shared<InputService>(events, true, fake.Backend());
	service->SetReady(true);

	const auto first =
		service->Handle(nlohmann::json{ { "action", "down" }, { "key", "a" } }, Owner("one"));
	CHECK(first["pending"] == true);
	CHECK(fake.QueuedCount() == 1);
	const auto up =
		service->Handle(nlohmann::json{ { "action", "up" }, { "key", "a" } }, Owner("one"));
	CHECK(up["released"] == true);

	const auto second =
		service->Handle(nlohmann::json{ { "action", "down" }, { "key", "a" } }, Owner("one"));
	CHECK(second["generation"].get<std::uint64_t>() >
		  first["generation"].get<std::uint64_t>());
	CHECK(fake.QueuedCount() == 2);

	CHECK(fake.RunQueued(0, { DispatchStatus::kApplied, 20, false, {} }) ==
		  DispatchStatus::kCancelled);
	CHECK(fake.RunQueued(1, { DispatchStatus::kApplied, 21, false, {} }) ==
		  DispatchStatus::kApplied);
	const auto status = service->Handle(
		nlohmann::json{ { "action", "status" }, { "device", "keyboard" } }, Owner("one"));
	CHECK(status["held"].size() == 1);
	CHECK(status["held"][0]["generation"] == second["generation"]);

	fake.PushImmediate({ DispatchStatus::kApplied, 22, false, {} });
	CHECK_NOTHROW(
		service->Handle(nlohmann::json{ { "action", "up" }, { "key", "a" } }, Owner("one")));
	service->Shutdown();
}

TEST_CASE("lifecycle cleanup invalidates queued native down")
{
	EventBus    events;
	FakeBackend fake;
	auto        service = std::make_shared<InputService>(events, true, fake.Backend());
	service->SetReady(true);

	service->Handle(
		nlohmann::json{ { "action", "down" }, { "key", "tab" } }, Owner("one"));
	CHECK(fake.QueuedCount() == 1);
	service->ReleaseForLifecycle("preLoadGame");
	CHECK(fake.RunQueued(0, { DispatchStatus::kApplied, 25, false, {} }) ==
		  DispatchStatus::kCancelled);
	CHECK(service->Handle(nlohmann::json{ { "action", "status" } }, Owner("one"))["held"].empty());
	service->Shutdown();
}

TEST_CASE("expired pending down is cancelled before native execution")
{
	EventBus    events;
	FakeBackend fake;
	auto        service = std::make_shared<InputService>(events, true, fake.Backend());
	service->SetReady(true);

	service->Handle(nlohmann::json{
						{ "action", "down" },
						{ "key", "b" },
						{ "maxHoldMs", 100 },
					},
		Owner("one"));
	CHECK(fake.QueuedCount() == 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(180));

	const auto status =
		service->Handle(nlohmann::json{ { "action", "status" } }, Owner("one"));
	CHECK(status["held"].empty());
	CHECK(fake.RunQueued(0, { DispatchStatus::kApplied, 30, false, {} }) ==
		  DispatchStatus::kCancelled);
	service->Shutdown();
}

TEST_CASE("failed release keeps its lease until a retry succeeds")
{
	EventBus    events;
	FakeBackend fake;
	fake.PushImmediate({ DispatchStatus::kApplied, 40, false, {} });
	fake.PushImmediate(
		{ DispatchStatus::kQueueFull, 41, false, "event pool full" });
	fake.PushImmediate({ DispatchStatus::kApplied, 42, false, {} });
	auto service = std::make_shared<InputService>(events, true, fake.Backend());
	service->SetReady(true);

	service->Handle(
		nlohmann::json{ { "action", "down" }, { "key", "c" } }, Owner("one"));
	ToolRegistry        registry;
	const std::weak_ptr weak = service;
	registry.Register(BuildInputDescriptor(), [weak](const auto& a_args, const auto& a_context) {
		return weak.lock()->Handle(a_args, a_context);
	});
	const auto failed =
		registry.Invoke("input", nlohmann::json{ { "action", "up" }, { "key", "c" } }, Owner("one"));
	CHECK(!failed.ok);
	CHECK(failed.errorCode == 503);
	CHECK(service->Handle(nlohmann::json{ { "action", "status" } }, Owner("one"))["held"].size() ==
		  1);

	std::this_thread::sleep_for(std::chrono::milliseconds(180));
	CHECK(service->Handle(nlohmann::json{ { "action", "status" } }, Owner("one"))["held"].empty());
	CHECK(fake.dispatchCount == 3);
	service->Shutdown();
}

TEST_CASE("uncertain native outcomes are not retried as mutations")
{
	EventBus    events;
	FakeBackend fake;
	fake.PushImmediate(
		{ DispatchStatus::kUnknown, 50, false, "concurrent queue generation" });
	fake.PushImmediate(
		{ DispatchStatus::kUnknown, 51, false, "concurrent queue generation" });
	auto service = std::make_shared<InputService>(events, true, fake.Backend());
	service->SetReady(true);

	const auto down =
		service->Handle(nlohmann::json{ { "action", "down" }, { "key", "d" } }, Owner("one"));
	CHECK(down["uncertain"] == true);
	CHECK(service->Handle(nlohmann::json{ { "action", "status" } }, Owner("one"))["held"].size() ==
		  1);

	const auto up =
		service->Handle(nlohmann::json{ { "action", "up" }, { "key", "d" } }, Owner("one"));
	CHECK(up["uncertain"] == true);
	CHECK(service->Handle(nlohmann::json{ { "action", "status" } }, Owner("one"))["held"].empty());
	CHECK(fake.dispatchCount == 2);
	service->Shutdown();
}

TEST_CASE("unbalanced sequence fails before any native mutation")
{
	EventBus    events;
	FakeBackend fake;
	auto        service = std::make_shared<InputService>(events, true, fake.Backend());
	service->SetReady(true);

	ToolRegistry        registry;
	const std::weak_ptr weak = service;
	registry.Register(BuildInputDescriptor(), [weak](const auto& a_args, const auto& a_context) {
		return weak.lock()->Handle(a_args, a_context);
	});
	const auto result = registry.Invoke("input",
		nlohmann::json{
			{ "action", "sequence" },
			{ "events", nlohmann::json::array({
							{ { "action", "down" }, { "key", "leftControl" } },
						}) },
		},
		Owner("one"));
	CHECK(!result.ok);
	CHECK(result.errorCode == 400);
	CHECK(fake.dispatchCount == 0);
	service->Shutdown();
}

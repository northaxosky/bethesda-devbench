#include "test_framework.h"

#include "tools/camera/CameraTool.h"
#include "tools/camera/MutationDispatch.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

using dvb::json;
using dvb::ToolContext;
using dvb::ToolError;
using dvb::ToolRegistry;

namespace
{
	dvb::ToolResult Invoke(ToolRegistry& a_registry, json a_args)
	{
		return a_registry.Invoke("camera", a_args, ToolContext{});
	}
}

TEST_CASE("camera descriptor exposes the approved actions and units")
{
	const auto descriptor = dvb::tools::BuildCameraDescriptor();
	CHECK(descriptor.name == "camera");
	const auto& actions =
		descriptor.inputSchema.at("properties").at("action").at("enum");
	CHECK(std::ranges::find(actions, "get") != actions.end());
	CHECK(std::ranges::find(actions, "setPov") != actions.end());
	CHECK(std::ranges::find(actions, "freecam") != actions.end());
	CHECK(std::ranges::find(actions, "drive") != actions.end());
	CHECK(descriptor.description.find("world-unit") != std::string::npos);
	CHECK(descriptor.description.find("radians") != std::string::npos);
}

TEST_CASE("camera mutations require control permission")
{
	int                       mutations = 0;
	dvb::tools::CameraBackend backend{
		.get = [] { return dvb::tools::CameraSnapshot{
						.pov = dvb::tools::CameraPov::kFirst,
					}; },
		.setPov = [&](dvb::tools::CameraPov) {
			++mutations;
			return json::object(); },
		.setFreeCamera = [&](bool) {
			++mutations;
			return json::object(); },
		.drive = [&](const dvb::tools::CameraDriveRequest&) {
			++mutations;
			return json::object(); },
	};
	ToolRegistry registry;
	dvb::tools::RegisterCameraTool(registry, false, std::move(backend));

	CHECK(Invoke(registry, json::object()).ok);
	CHECK(Invoke(registry,
			  json{ { "action", "setPov" }, { "pov", "third" } })
			  .errorCode == 403);
	CHECK(Invoke(registry, json{ { "action", "freecam" }, { "on", true } })
			  .errorCode == 403);
	CHECK(Invoke(registry,
			  json{
				  { "action", "drive" },
				  { "x", 1 },
				  { "y", 2 },
				  { "z", 3 },
				  { "pitch", 0 },
				  { "yaw", 0 },
			  })
			  .errorCode == 403);
	CHECK(mutations == 0);
}

TEST_CASE("camera drive rejects inactive and externally owned free camera")
{
	dvb::tools::CameraSnapshot state;
	int                        drives = 0;
	dvb::tools::CameraBackend  backend{
		.get = [&] { return state; },
		.drive = [&](const dvb::tools::CameraDriveRequest&) {
			++drives;
			return json::object(); },
	};
	ToolRegistry registry;
	dvb::tools::RegisterCameraTool(registry, true, std::move(backend));
	const json args{
		{ "action", "drive" },
		{ "x", 1 },
		{ "y", 2 },
		{ "z", 3 },
		{ "pitch", 0 },
		{ "yaw", 0 },
	};

	CHECK(Invoke(registry, args).errorCode == 409);
	state.freeCam = true;
	CHECK(Invoke(registry, args).errorCode == 409);
	CHECK(drives == 0);
}

TEST_CASE("camera drive validates and normalizes an explicit absolute transform")
{
	dvb::tools::CameraDriveRequest observed;
	int                            drives = 0;
	dvb::tools::CameraBackend      backend{
		.get = [] { return dvb::tools::CameraSnapshot{
						.pov = dvb::tools::CameraPov::kOther,
						.freeCam = true,
						.freeCamOwned = true,
					}; },
		.drive = [&](const dvb::tools::CameraDriveRequest& a_request) {
			observed = a_request;
			++drives;
			return json{ { "applied", true } }; },
	};
	ToolRegistry registry;
	dvb::tools::RegisterCameraTool(registry, true, std::move(backend));

	CHECK(Invoke(registry,
		json{
			{ "action", "drive" },
			{ "x", 1 },
			{ "y", 2 },
			{ "z", 3 },
			{ "pitch", 7.0 },
			{ "yaw", -7.0 },
		})
			.ok);
	CHECK(drives == 1);
	CHECK(observed.x == 1.0F);
	CHECK(observed.y == 2.0F);
	CHECK(observed.z == 3.0F);
	CHECK(std::abs(observed.pitch - std::remainder(7.0, 6.2831853071795864769)) <
		  0.00001);
	CHECK(std::abs(observed.yaw - std::remainder(-7.0, 6.2831853071795864769)) <
		  0.00001);

	CHECK(Invoke(registry,
			  json{
				  { "action", "drive" },
				  { "x", 1 },
				  { "y", 2 },
				  { "z", 3 },
				  { "pitch", 0 },
			  })
			  .errorCode == 400);
	CHECK(Invoke(registry,
			  json{
				  { "action", "drive" },
				  { "x", std::numeric_limits<double>::infinity() },
				  { "y", 2 },
				  { "z", 3 },
				  { "pitch", 0 },
				  { "yaw", 0 },
			  })
			  .errorCode == 400);
}

TEST_CASE("free camera ownership clears stale ownership without exiting another state")
{
	dvb::tools::FreeCameraOwnership ownership;
	CHECK(ownership.ClaimTransition(false, true));
	CHECK(ownership.Observe(true));
	CHECK(ownership.PrepareExit(false) ==
		  dvb::tools::FreeCameraExitDecision::kOwnershipLost);
	CHECK(!ownership.Observe(false));
	CHECK(ownership.PrepareExit(true) ==
		  dvb::tools::FreeCameraExitDecision::kNotOwned);
}

TEST_CASE("camera descriptor refresh preserves backend service state")
{
	int                       reads = 0;
	dvb::tools::CameraBackend backend{
		.get = [&] {
			++reads;
			return dvb::tools::CameraSnapshot{
				.pov = dvb::tools::CameraPov::kFirst,
			};
		},
	};
	ToolRegistry registry;
	auto         service =
		dvb::tools::RegisterCameraTool(registry, true, std::move(backend));

	CHECK(Invoke(registry, json::object()).ok);
	CHECK(reads == 1);
	service->RefreshDescriptor();
	CHECK(Invoke(registry, json::object()).ok);
	CHECK(reads == 2);
}

TEST_CASE("pending main-thread mutations cancel before they can apply")
{
	std::function<void()>                queued;
	dvb::tools::CancelableMutationRunner runner(
		[&](std::function<void()> a_task) {
			queued = std::move(a_task);
			return true;
		},
		std::chrono::milliseconds(1));

	bool applied = false;
	try
	{
		(void)runner.Run("test mutation", [&] {
			applied = true;
			return json::object();
		});
		CHECK(false);
	}
	catch (const ToolError& error)
	{
		CHECK(error.code == 504);
		CHECK(std::string(error.what()).find("cancelled before execution") !=
			  std::string::npos);
	}
	CHECK(static_cast<bool>(queued));
	queued();
	CHECK(!applied);
}

TEST_CASE("started main-thread mutations report uncertainty and are not retried")
{
	std::atomic<bool>                    started{ false };
	std::atomic<int>                     applied{ 0 };
	std::promise<void>                   release;
	auto                                 completion = release.get_future().share();
	std::jthread                         worker;
	dvb::tools::CancelableMutationRunner runner(
		[&](std::function<void()> a_task) {
			worker = std::jthread([task = std::move(a_task)]() mutable { task(); });
			while (!started.load(std::memory_order_acquire))
				std::this_thread::yield();
			return true;
		},
		std::chrono::milliseconds(1));

	try
	{
		try
		{
			(void)runner.Run("test mutation", [&] {
				started.store(true, std::memory_order_release);
				completion.wait();
				applied.fetch_add(1, std::memory_order_relaxed);
				return json::object();
			});
			CHECK(false);
		}
		catch (const ToolError& error)
		{
			CHECK(error.code == 504);
			CHECK(std::string(error.what()).find("outcome is uncertain") !=
				  std::string::npos);
			CHECK(std::string(error.what()).find("must not be retried") !=
				  std::string::npos);
		}
	}
	catch (...)
	{
		release.set_value();
		worker.join();
		throw;
	}
	release.set_value();
	worker.join();
	CHECK(applied.load(std::memory_order_relaxed) == 1);
}

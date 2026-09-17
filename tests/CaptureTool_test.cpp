#include "test_framework.h"

#include "ToolExtensions.h"
#include "tools/capture/CaptureTool.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <thread>

using dvb::json;
using dvb::ToolContext;
using dvb::ToolRegistry;
using namespace std::chrono_literals;

namespace
{
	namespace fs = std::filesystem;
	using dvb::tools::capture::CaptureBackend;
	using dvb::tools::capture::CaptureConfiguration;
	using dvb::tools::capture::CaptureService;

	class TemporaryRoot
	{
	public:
		TemporaryRoot()
		{
			static std::atomic_uint64_t sequence{ 0 };
			path = fs::temp_directory_path() /
			       std::format("devbench_capture_tests_{}_{}", ::GetCurrentProcessId(), ++sequence);
			std::error_code ec;
			fs::remove_all(path, ec);
			fs::create_directories(path);
		}

		~TemporaryRoot()
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}

		fs::path path;
	};

	void WriteBmp24(const fs::path& a_path, int a_width = 16, int a_height = 16, bool a_invert = false)
	{
		fs::create_directories(a_path.parent_path());
		const int                 rowSize = ((a_width * 3 + 3) / 4) * 4;
		const std::uint32_t       pixelBytes = static_cast<std::uint32_t>(rowSize) * a_height;
		const std::uint32_t       fileSize = 14 + 40 + pixelBytes;
		std::vector<std::uint8_t> bytes;
		bytes.reserve(fileSize);
		auto put16 = [&](std::uint16_t value) {
			bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
			bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
		};
		auto put32 = [&](std::uint32_t value) {
			bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
			bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
			bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
			bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
		};
		bytes.push_back('B');
		bytes.push_back('M');
		put32(fileSize);
		put32(0);
		put32(54);
		put32(40);
		put32(static_cast<std::uint32_t>(a_width));
		put32(static_cast<std::uint32_t>(a_height));
		put16(1);
		put16(24);
		put32(0);
		put32(pixelBytes);
		put32(0);
		put32(0);
		put32(0);
		put32(0);
		for (int y = a_height - 1; y >= 0; --y)
		{
			int written = 0;
			for (int x = 0; x < a_width; ++x)
			{
				auto value = static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 2 == 0 ? 220 : 30);
				if (a_invert)
					value = static_cast<std::uint8_t>(255 - value);
				bytes.push_back(value);
				bytes.push_back(value);
				bytes.push_back(value);
				written += 3;
			}
			while (written++ < rowSize)
				bytes.push_back(0);
		}
		std::ofstream output(a_path, std::ios::binary | std::ios::trunc);
		output.write(
			reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	}

	struct Harness
	{
		TemporaryRoot                   root;
		dvb::EventBus                   events;
		ToolRegistry                    registry;
		std::function<bool()>           nativeAction;
		std::shared_ptr<CaptureService> service;

		Harness()
		{
			CaptureConfiguration configuration{
				.captureDirectory = "captures",
				.scanDirectories = { "native" },
				.timeout = 150ms,
				.settle = 0ms,
			};
			CaptureBackend backend{
				.gameRoot = [this] { return root.path; },
				.resolvePhysicalPath = [](const fs::path& a_path) { return fs::absolute(a_path).lexically_normal(); },
				.queueNativeScreenshot = [this] { return nativeAction ? nativeAction() : false; },
				.sceneSnapshot = [] { return json{ { "sceneToken", "fixture" } }; },
				.currentFrame = [] { return 77; },
			};
			service = dvb::tools::capture::RegisterCaptureTool(
				registry, events, std::move(configuration), std::move(backend));
		}

		dvb::ToolResult Invoke(json a_args)
		{
			if (!a_args.contains("timeoutMs"))
				a_args["timeoutMs"] = 150;
			if (!a_args.contains("pollMs"))
				a_args["pollMs"] = 10;
			return registry.Invoke("capture", a_args, ToolContext{});
		}
	};

	void RegisterProvider(std::string a_key, dvb::ToolHandler a_handler)
	{
		dvb::ToolExtensions::Register(
			"capture", std::move(a_key),
			json{ { "kind", "screenshot" }, { "contract", "capture.ready" } },
			std::move(a_handler));
	}

	bool HasDegraded(const json& a_result, std::string_view a_reason)
	{
		for (const auto& reason : a_result.at("degraded"))
			if (reason.is_string() && reason.get_ref<const std::string&>() == a_reason)
				return true;
		return false;
	}
}

TEST_CASE("capture auto requires a provider unless native fallback is explicitly allowed")
{
	Harness harness;
	CHECK(harness.Invoke(json{ { "checkpointId", "none" } }).errorCode == 404);

	harness.nativeAction = [&] {
		WriteBmp24(harness.root.path / "native" / "ScreenShot0.bmp");
		return true;
	};
	const auto native = harness.Invoke(json{
		{ "checkpointId", "native-default" },
		{ "allowNative", true },
	});
	CHECK(native.ok);
	CHECK(native.value.at("provider") == "native");
	CHECK(native.value.at("inconclusive") == true);
	CHECK(native.value.at("nativeRelocated") == true);
	CHECK(HasDegraded(native.value, "uiExclusionUnavailable"));
	const fs::path returned = native.value.at("path").get<std::string>();
	CHECK(returned == harness.root.path / "captures" / "adhoc" / "default" / "native-default.bmp");
	CHECK(fs::exists(returned));
	CHECK(fs::exists(fs::path(returned).replace_extension(".json")));
	CHECK(!fs::exists(harness.root.path / "native" / "ScreenShot0.bmp"));

	std::error_code ec;
	fs::remove_all(harness.root.path / "native", ec);
	CHECK(!ec);
	CHECK(fs::exists(returned));
}

TEST_CASE("capture native outDir overrides configured canonical storage without retaining the source")
{
	Harness harness;
	harness.nativeAction = [&] {
		WriteBmp24(harness.root.path / "native" / "ScreenShotOverride.bmp");
		return true;
	};
	const auto result = harness.Invoke(json{
		{ "kind", "native" },
		{ "checkpointId", "native-override" },
		{ "outDir", "alternate-captures" },
	});
	CHECK(result.ok);
	const fs::path returned = result.value.at("path").get<std::string>();
	CHECK(returned ==
		  harness.root.path / "alternate-captures" / "adhoc" / "default" / "native-override.bmp");
	CHECK(fs::exists(returned));
	CHECK(!fs::exists(harness.root.path / "native" / "ScreenShotOverride.bmp"));
	CHECK(!fs::exists(
		harness.root.path / "captures" / "adhoc" / "default" / "native-override.bmp"));
}

TEST_CASE("capture provider accepts an immediate correlated event only after the image is complete")
{
	Harness           harness;
	const std::string key = "capture-immediate-provider";
	RegisterProvider(key, [&](const json& a_args, const ToolContext&) {
		const fs::path output = a_args.at("outputPath").get<std::string>();
		WriteBmp24(output);
		harness.events.Publish("capture.ready", json{
													{ "requestId", a_args.at("requestId") },
													{ "path", a_args.at("outputPath") },
													{ "ok", true },
													{ "uiExcluded", true },
												});
		return json{ { "accepted", true } };
	});

	const auto result = harness.Invoke(json{
		{ "kind", key },
		{ "checkpointId", "immediate" },
	});
	CHECK(result.ok);
	CHECK(result.value.at("readyBy") == "event");
	CHECK(result.value.at("requestId").get<std::string>().find("immediate#") == 0);
	CHECK(result.value.at("width") == 16);
	CHECK(result.value.at("height") == 16);
	CHECK(result.value.at("uiExcluded") == true);
	CHECK(result.value.at("inconclusive") == false);
}

TEST_CASE("capture auto rejects multiple providers instead of guessing")
{
	RegisterProvider("capture-ambiguity-provider", [](const json&, const ToolContext&) {
		return json::object();
	});
	Harness    harness;
	const auto result = harness.Invoke(json{ { "checkpointId", "ambiguous-provider" } });
	CHECK(!result.ok);
	CHECK(result.errorCode == 400);
	CHECK(result.errorMessage.find("multiple capture providers") != std::string::npos);
}

TEST_CASE("capture descriptor refresh reuses the live service and shared descriptor factory")
{
	Harness           harness;
	const std::string key = "capture-refresh-provider";
	RegisterProvider(key, [&](const json& a_args, const ToolContext&) {
		WriteBmp24(fs::path(a_args.at("outputPath").get<std::string>()));
		harness.events.Publish("capture.ready", json{
													{ "requestId", a_args.at("requestId") },
													{ "ok", true },
													{ "uiExcluded", true },
												});
		return json::object();
	});

	harness.service->RefreshDescriptor(harness.registry);
	const auto registered = harness.registry.Describe("capture");
	CHECK(registered.has_value());
	CHECK(registered->inputSchema == dvb::tools::capture::BuildCaptureDescriptor().inputSchema);
	const auto kinds = registered->inputSchema.at("properties").at("kind").at("enum");
	bool       found = false;
	for (const auto& kind : kinds)
		found = found || (kind.is_string() && kind.get_ref<const std::string&>() == key);
	CHECK(found);

	const auto result = harness.Invoke(json{
		{ "kind", key },
		{ "checkpointId", "refreshed" },
	});
	CHECK(result.ok);
	CHECK(result.value.at("sceneToken") == "fixture");
}

TEST_CASE("capture provider supports late readiness while making missing event degradation explicit")
{
	Harness           harness;
	const std::string key = "capture-late-provider";
	std::jthread      publisher;
	RegisterProvider(key, [&](const json& a_args, const ToolContext&) {
		WriteBmp24(fs::path(a_args.at("outputPath").get<std::string>()));
		const auto requestId = a_args.at("requestId").get<std::string>();
		publisher = std::jthread([&, requestId] {
			std::this_thread::sleep_for(80ms);
			harness.events.Publish("capture.ready", json{
														{ "requestId", requestId },
														{ "ok", true },
														{ "uiExcluded", true },
													});
		});
		return json::object();
	});

	const auto result = harness.Invoke(json{
		{ "kind", key },
		{ "checkpointId", "late" },
	});
	CHECK(result.ok);
	CHECK(result.value.at("readyBy") == "poll");
	CHECK(result.value.at("inconclusive") == true);
	CHECK(HasDegraded(result.value, "providerReadyEventMissing"));
}

TEST_CASE("capture provider failure event is terminal and event success without a file is not success")
{
	Harness           failureHarness;
	const std::string failureKey = "capture-failure-provider";
	RegisterProvider(failureKey, [&](const json& a_args, const ToolContext&) {
		failureHarness.events.Publish("capture.ready", json{
														   { "requestId", a_args.at("requestId") },
														   { "ok", false },
														   { "error", "render source unavailable" },
													   });
		return json::object();
	});
	const auto failure = failureHarness.Invoke(json{
		{ "kind", failureKey },
		{ "checkpointId", "failure" },
	});
	CHECK(!failure.ok);
	CHECK(failure.errorCode == 502);
	CHECK(failure.errorMessage.find("render source unavailable") != std::string::npos);

	Harness           noFileHarness;
	const std::string noFileKey = "capture-event-only-provider";
	RegisterProvider(noFileKey, [&](const json& a_args, const ToolContext&) {
		noFileHarness.events.Publish("capture.ready", json{
														  { "requestId", a_args.at("requestId") },
														  { "ok", true },
														  { "uiExcluded", true },
													  });
		return json::object();
	});
	const auto noFile = noFileHarness.Invoke(json{
		{ "kind", noFileKey },
		{ "checkpointId", "event-only" },
		{ "timeoutMs", 60 },
	});
	CHECK(!noFile.ok);
	CHECK(noFile.errorCode == 504);
}

TEST_CASE("capture waits through an exclusive writer lock and a temporarily partial image")
{
	Harness           lockedHarness;
	const std::string lockedKey = "capture-locked-provider";
	std::jthread      unlocker;
	RegisterProvider(lockedKey, [&](const json& a_args, const ToolContext&) {
		const fs::path output = a_args.at("outputPath").get<std::string>();
		WriteBmp24(output);
		const HANDLE handle = ::CreateFileW(
			output.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		CHECK(handle != INVALID_HANDLE_VALUE);
		unlocker = std::jthread([handle] {
			std::this_thread::sleep_for(60ms);
			::CloseHandle(handle);
		});
		lockedHarness.events.Publish("capture.ready", json{
														  { "requestId", a_args.at("requestId") },
														  { "ok", true },
														  { "uiExcluded", true },
													  });
		return json::object();
	});
	CHECK(lockedHarness.Invoke(json{
								   { "kind", lockedKey },
								   { "checkpointId", "locked" },
							   })
			.ok);

	Harness           partialHarness;
	const std::string partialKey = "capture-partial-provider";
	std::jthread      finisher;
	RegisterProvider(partialKey, [&](const json& a_args, const ToolContext&) {
		const fs::path output = a_args.at("outputPath").get<std::string>();
		fs::create_directories(output.parent_path());
		{
			std::ofstream partial(output, std::ios::binary | std::ios::trunc);
			partial << "BMpartial";
		}
		finisher = std::jthread([output] {
			std::this_thread::sleep_for(60ms);
			WriteBmp24(output);
		});
		partialHarness.events.Publish("capture.ready", json{
														   { "requestId", a_args.at("requestId") },
														   { "ok", true },
														   { "uiExcluded", true },
													   });
		return json::object();
	});
	const auto partial = partialHarness.Invoke(json{
		{ "kind", partialKey },
		{ "checkpointId", "partial" },
		{ "timeoutMs", 500 },
	});
	CHECK(partial.ok);
	if (partial.ok)
		CHECK(partial.value.at("width") == 16);
}

TEST_CASE("capture reports event-ring eviction while retaining correlated provider success")
{
	Harness           harness;
	const std::string key = "capture-gap-provider";
	RegisterProvider(key, [&](const json& a_args, const ToolContext&) {
		WriteBmp24(fs::path(a_args.at("outputPath").get<std::string>()));
		for (int i = 0; i < 300; ++i)
			harness.events.Publish("test.noise", json{ { "index", i } });
		harness.events.Publish("capture.ready", json{
													{ "requestId", a_args.at("requestId") },
													{ "ok", true },
													{ "uiExcluded", true },
												});
		return json::object();
	});
	const auto result = harness.Invoke(json{
		{ "kind", key },
		{ "checkpointId", "gap" },
	});
	CHECK(result.ok);
	CHECK(result.value.contains("eventGap"));
	CHECK(result.value.at("inconclusive") == true);
	CHECK(HasDegraded(result.value, "eventRingGap"));
}

TEST_CASE("capture native correlation rejects two changed image candidates")
{
	Harness harness;
	harness.nativeAction = [&] {
		WriteBmp24(harness.root.path / "native" / "ScreenShot1.bmp");
		WriteBmp24(harness.root.path / "native" / "ScreenShot2.bmp");
		return true;
	};
	const auto result = harness.Invoke(json{
		{ "kind", "native" },
		{ "checkpointId", "ambiguous-native" },
	});
	CHECK(!result.ok);
	CHECK(result.errorCode == 409);
	CHECK(result.errorMessage.find("ambiguous") != std::string::npos);
}

TEST_CASE("capture rejects traversal and never dispatches onto an existing output")
{
	Harness harness;
	for (const auto& args : {
			 json{ { "kind", "native" }, { "checkpointId", "../escape" } },
			 json{ { "kind", "native" }, { "checkpointId", "safe" }, { "recording", ".." } },
			 json{ { "kind", "native" }, { "checkpointId", "safe" }, { "variant", R"(a\b)" } },
			 json{ { "kind", "native" }, { "checkpointId", "safe" }, { "outDir", R"(..\escape)" } },
		 })
		CHECK(harness.Invoke(args).errorCode == 400);

	const std::string key = "capture-no-overwrite-provider";
	int               dispatches = 0;
	RegisterProvider(key, [&](const json& a_args, const ToolContext&) {
		++dispatches;
		WriteBmp24(fs::path(a_args.at("outputPath").get<std::string>()));
		harness.events.Publish("capture.ready", json{
													{ "requestId", a_args.at("requestId") },
													{ "ok", true },
													{ "uiExcluded", true },
												});
		return json::object();
	});
	const json request{
		{ "kind", key },
		{ "checkpointId", "same-output" },
	};
	CHECK(harness.Invoke(request).ok);
	CHECK(harness.Invoke(request).errorCode == 409);
	CHECK(dispatches == 1);
}

TEST_CASE("capture rejects a permanently bad image after allowing completion time")
{
	Harness           harness;
	const std::string key = "capture-bad-image-provider";
	RegisterProvider(key, [&](const json& a_args, const ToolContext&) {
		const fs::path output = a_args.at("outputPath").get<std::string>();
		fs::create_directories(output.parent_path());
		std::ofstream(output, std::ios::binary | std::ios::trunc) << "not an image";
		harness.events.Publish("capture.ready", json{
													{ "requestId", a_args.at("requestId") },
													{ "ok", true },
												});
		return json::object();
	});
	const auto result = harness.Invoke(json{
		{ "kind", key },
		{ "checkpointId", "bad-image" },
		{ "timeoutMs", 60 },
	});
	CHECK(!result.ok);
	CHECK(result.errorCode == 422);
	CHECK(result.errorMessage.find("decodable") != std::string::npos);
}

TEST_CASE("capture preserves region thresholds while provider degradation makes the verdict inconclusive")
{
	Harness    harness;
	const auto golden = harness.root.path / "golden.bmp";
	WriteBmp24(golden);
	const std::string key = "capture-regions-provider";
	RegisterProvider(key, [&](const json& a_args, const ToolContext&) {
		WriteBmp24(fs::path(a_args.at("outputPath").get<std::string>()));
		harness.events.Publish("capture.ready", json{
													{ "requestId", a_args.at("requestId") },
													{ "ok", true },
													{ "uiExcluded", true },
													{ "subrectApplied", true },
													{ "degraded", json::array({ "temporalVariance" }) },
												});
		return json::object();
	});
	const auto result = harness.Invoke(json{
		{ "kind", key },
		{ "checkpointId", "regions" },
		{ "golden", golden.string() },
		{ "threshold", 0.98 },
		{ "subrect", json{ { "x", 0.0 }, { "y", 0.0 }, { "w", 1.0 }, { "h", 1.0 } } },
		{ "regions", json::array({
						 json{ { "name", "left" }, { "x", 0.0 }, { "y", 0.0 }, { "w", 0.5 }, { "h", 1.0 } },
						 json{ { "name", "right" }, { "x", 0.5 }, { "y", 0.0 }, { "w", 0.5 }, { "h", 1.0 }, { "threshold", 0.99 } },
					 }) },
	});
	CHECK(result.ok);
	CHECK(result.value.at("passed") == true);
	CHECK(result.value.at("regions").size() == 2);
	CHECK(result.value.at("threshold") == 0.98);
	CHECK(result.value.at("inconclusive") == true);
	CHECK(HasDegraded(result.value, "temporalVariance"));
	CHECK(result.value.at("subrectApplied") == true);
}

TEST_CASE("screenshot listing returns physical paths and surfaces missing directories")
{
	Harness harness;
	dvb::tools::capture::RegisterScreenshotInspectionExtension(harness.service);
	WriteBmp24(harness.root.path / "native" / "ScreenShot9.bmp");
	const auto extension = dvb::ToolExtensions::Find("inspect", "screenshots");
	CHECK(extension.has_value());
	const auto listed = extension->handler(
		json{ { "kind", "screenshots" }, { "limit", 1 } }, ToolContext{});
	CHECK(listed.at("count") == 1);
	CHECK(listed.at("returned") == 1);
	CHECK(fs::path(listed.at("screenshots")[0].at("path").get<std::string>()).is_absolute());

	CHECK_THROWS(harness.service->ListScreenshots(json{
		{ "dir", "does-not-exist" },
	}));
}

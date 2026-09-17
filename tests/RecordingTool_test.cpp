#include "test_framework.h"

#include "tools/recording/RecordingModel.h"
#include "tools/recording/RecordingTool.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
#include <thread>

using dvb::EventBus;
using dvb::json;
using dvb::ToolContext;
using dvb::ToolDescriptor;
using dvb::ToolRegistry;
using dvb::tools::recording::ParseRecording;
using dvb::tools::recording::RecordingBackend;
using dvb::tools::recording::RecordingConfiguration;
using dvb::tools::recording::RecordingService;
using dvb::tools::recording::RecordingStore;
using dvb::tools::scenario::ScenarioService;

namespace
{
	using namespace std::chrono_literals;
	namespace fs = std::filesystem;

	struct TempRoot
	{
		fs::path path = fs::temp_directory_path() /
		                std::format(
							"devbench_recording_test_{}",
							std::chrono::steady_clock::now()
								.time_since_epoch()
								.count());

		TempRoot()
		{
			fs::create_directories(path);
		}

		~TempRoot()
		{
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	};

	ToolDescriptor Descriptor(std::string a_name, bool a_readOnly = false)
	{
		ToolDescriptor descriptor;
		descriptor.name = std::move(a_name);
		descriptor.description = "recording test tool";
		descriptor.readOnly = a_readOnly;
		return descriptor;
	}

	json RecordingDocument(json a_meta = json::object(), json a_steps = json::array())
	{
		a_meta["format"] = "devbench-recording-3";
		a_meta["game"] = "fo4";
		return json{
			{ "meta", std::move(a_meta) },
			{ "samples", json::array() },
			{ "activityEvents", json::array() },
			{ "steps", std::move(a_steps) },
		};
	}

	RecordingBackend Backend()
	{
		return {
			.snapshot = [] {
				return json{
					{ "playerLoaded", true },
					{ "frame", 1 },
					{ "pose", json::array({ 0, 0, 0, 0, 0 }) },
					{ "scene", json::object() },
				};
			},
		};
	}
}

TEST_CASE("recording storage confines paths and reports corrupt entries independently")
{
	TempRoot      root;
	RecordingStore store(root.path);
	const auto validFile = store.WriteUnique(RecordingDocument());
	{
		std::ofstream corrupt(root.path / "corrupt.json", std::ios::binary);
		corrupt << "{broken";
	}

	const auto list = store.List();
	CHECK(list.at("count") == 2);
	bool sawValid = false;
	bool sawCorrupt = false;
	for (const auto& item : list.at("recordings"))
	{
		if (item.value("file", std::string{}) == validFile)
			sawValid = !item.contains("error");
		if (item.value("file", std::string{}) == "corrupt.json")
			sawCorrupt = item.contains("error");
	}
	CHECK(sawValid);
	CHECK(sawCorrupt);
	CHECK_THROWS(store.Load("../outside.json"));
	CHECK_THROWS(store.Delete("nested/file.json"));

	store.Delete(validFile);
	CHECK(!fs::exists(root.path / validFile));
	CHECK(fs::exists(root.path / "corrupt.json"));
}

TEST_CASE("foreign recording capabilities require an explicit downgrade")
{
	auto foreign = RecordingDocument();
	foreign["meta"]["game"] = "skyrim";
	foreign["meta"]["runtime"] =
		json{ { "recordedOnVR", true } };
	CHECK_THROWS(ParseRecording(foreign));
	const auto downgraded = ParseRecording(std::move(foreign), true);
	CHECK(downgraded.foreignCapability);
	CHECK(!downgraded.warnings.empty());
}

TEST_CASE("recording compatibility distinguishes Skyrim desktop and VR")
{
	auto desktop = RecordingDocument();
	desktop["meta"]["game"] = "se";
	desktop["meta"]["runtime"] = json{
		{ "recordedOnVR", false },
		{ "compat", json::array({ "se", "ae" }) },
	};
	CHECK_NOTHROW(ParseRecording(
		desktop,
		dvb::tools::recording::RecordingCompatibilityForProfile(
			dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimAE))));
	CHECK_THROWS(ParseRecording(
		desktop,
		dvb::tools::recording::RecordingCompatibilityForProfile(
			dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimVR))));

	auto vr = RecordingDocument();
	vr["meta"]["game"] = "vr";
	vr["meta"]["runtime"] = json{
		{ "recordedOnVR", true },
		{ "compat", json::array({ "vr" }) },
	};
	vr["trackingSamples"] = json::array(
		{ json{ { "tMs", 0 }, { "devices", json::array() } } });
	CHECK_NOTHROW(ParseRecording(
		vr,
		dvb::tools::recording::RecordingCompatibilityForProfile(
			dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimVR))));
	CHECK_THROWS(ParseRecording(
		vr,
		dvb::tools::recording::RecordingCompatibilityForProfile(
			dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimSE))));
}

TEST_CASE("recording atomic publication failure leaves no temporary artifact")
{
	TempRoot root;
	const auto blockedRoot = root.path / "not-a-directory";
	{
		std::ofstream file(blockedRoot, std::ios::binary);
		file << "occupied";
	}
	RecordingStore store(blockedRoot);
	CHECK_THROWS(store.WriteUnique(RecordingDocument()));
	CHECK(fs::is_regular_file(blockedRoot));
	for (const auto& entry : fs::directory_iterator(root.path))
		CHECK(!entry.path().filename().string().contains(".tmp."));
}

TEST_CASE("exterior recording transitions use numeric COW through console")
{
	TempRoot root;
	ToolRegistry registry;
	EventBus events;
	ScenarioService scenarios(registry, events);
	scenarios.Register();

	std::string command;
	int nativeTransitions = 0;
	registry.Register(
		Descriptor("console"),
		[&](const json& a_args, const ToolContext&) {
			command = a_args.at("command").get<std::string>();
			return json{ { "queued", true } };
		});

	auto backend = Backend();
	backend.transition =
		[&](const json&) -> dvb::tools::recording::RecordingTransitionReceipt {
		++nativeTransitions;
		return { .queued = true };
	};
	RecordingConfiguration configuration;
	configuration.root = root.path;
	RecordingService recordings(
		registry, events, scenarios, true, configuration, std::move(backend));
	recordings.Register();

	ToolContext context;
	context.internal = true;
	const auto transitioned = registry.Invoke(
		"record",
		json{
			{ "action", "transition" },
			{ "target",
				json{
					{ "interior", false },
					{ "worldspace", json{ { "formId", 0x3C } } },
					{ "anchor",
						json{
							{ "x", 8192.0 },
							{ "y", -1.0 },
						} },
				} },
		},
		context);
	CHECK_MESSAGE(transitioned.ok, transitioned.errorMessage);
	CHECK(command == "COW 0000003C 2 -1");
	CHECK(nativeTransitions == 0);
	CHECK(transitioned.value.at("mode") == "worldspaceConsole");
}

TEST_CASE("replay accepts runtime FO4 null form identity fields")
{
	TempRoot root;
	RecordingStore store(root.path);
	auto document = RecordingDocument(
		json{
			{ "activityCapture",
				json{
					{ "cell", true },
					{ "console", true },
					{ "input", false },
					{ "lifecycle", true },
					{ "menu", true },
				} },
			{ "anchor",
				json{
					{ "pitch", 4.2736907716988215 },
					{ "x", 8407.4677734375 },
					{ "y", 5980.46484375 },
					{ "yaw", 23.6638682757843 },
					{ "z", 557.7318725585938 },
				} },
			{ "cell",
				json{
					{ "editorId", nullptr },
					{ "formId", 56320 },
					{ "formIdHex", "0x0000DC00" },
					{ "formType", "CELL" },
					{ "name", nullptr },
				} },
			{ "cellFormID", 56320 },
			{ "entryPoint",
				json{
					{ "ageMs", 4061 },
					{ "kind", "save" },
					{ "target", json::object() },
					{ "value", "DevBenchQueuedLoad_20260915_005251_ecde81d9" },
				} },
			{ "interior", false },
			{ "recordedMs", 261 },
			{ "startState", "playerLoaded" },
			{ "weather",
				json{
					{ "editorId", nullptr },
					{ "formId", 177450 },
					{ "formIdHex", "0x0002B52A" },
					{ "formType", "WTHR" },
					{ "name", nullptr },
				} },
			{ "worldspace",
				json{
					{ "editorId", "Commonwealth" },
					{ "formId", 60 },
					{ "formIdHex", "0x0000003C" },
					{ "formType", "WRLD" },
					{ "name", "Commonwealth" },
				} },
			{ "worldspaceFormID", 60 },
		},
		json::array({
			json{
				{ "atMs", 52 },
				{ "pose",
					json::array({
						8407.4677734375,
						5980.46484375,
						557.7318725585938,
						23.6638682757843,
						4.2736907716988215,
					}) },
				{ "wait", 52 },
			},
		}));
	document["samples"] = json::array({
		json{
			{ "frame", 1428 },
			{ "pose", document.at("steps")[0].at("pose") },
			{ "tMs", 52 },
		},
	});
	const auto file = store.WriteUnique(document);

	ToolRegistry registry;
	EventBus events;
	registry.Register(
		Descriptor("console"),
		[](const json&, const ToolContext&) {
			return json{ { "queued", true } };
		});
	registry.Register(
		Descriptor("inspect", true),
		[](const json& a_args, const ToolContext&) {
			if (a_args.value("kind", std::string{}) == "scene")
				return json{
					{ "playerLoaded", true },
					{ "cell", json{ { "formId", 56320 } } },
					{ "worldspace", json{ { "formId", 60 } } },
				};
			return json{
				{ "menus",
					json{
						{ "open", json::array({ "HUDMenu" }) },
					} },
			};
		});
	ScenarioService scenarios(registry, events);
	scenarios.Register();
	RecordingConfiguration configuration;
	configuration.root = root.path;
	RecordingService recordings(
		registry, events, scenarios, true, configuration, Backend());
	recordings.Register();

	const auto replay = registry.Invoke(
		"record",
		json{
			{ "action", "replay" },
			{ "file", file },
			{ "restoreScene", false },
			{ "async", true },
		},
		ToolContext{});
	CHECK_MESSAGE(replay.ok, replay.errorMessage);
	if (!replay.ok)
		return;

	const auto runId = replay.value.at("runId").get<std::uint64_t>();
	json status;
	for (int attempt = 0; attempt < 500; ++attempt)
	{
		status = scenarios.Status(runId);
		if (status.at("done").get<bool>())
			break;
		std::this_thread::sleep_for(1ms);
	}
	CHECK(status.at("done").get<bool>());
	CHECK(status.at("result").at("ok") == true);
	CHECK(status.at("result")
		      .at("results")[0]
		      .at("expected")
		      .at("cell") == "0x0000DC00");
}

TEST_CASE("missing named replay fixture never falls back to the latest save")
{
	TempRoot root;
	RecordingStore store(root.path);
	const auto file = store.WriteUnique(RecordingDocument(
		json{
			{ "startState", "noPlayer" },
			{ "entryPoint",
				json{
					{ "kind", "save" },
					{ "value", "RequiredFixture" },
				} },
		}));

	ToolRegistry registry;
	EventBus     events;
	int          loads = 0;
	int          loadLasts = 0;
	registry.Register(
		Descriptor("game"),
		[&](const json& a_args, const ToolContext&) {
			const auto action = a_args.at("action").get<std::string>();
			if (action == "list")
				return json{
					{ "saves",
						json::array({
							json{ { "name", "DifferentSave" } },
						}) },
				};
			if (action == "load")
				++loads;
			if (action == "loadLast")
				++loadLasts;
			return json::object();
		});
	ScenarioService scenarios(registry, events);
	scenarios.Register();
	RecordingConfiguration configuration;
	configuration.root = root.path;
	RecordingService recordings(
		registry, events, scenarios, true, configuration, Backend());
	recordings.Register();

	const auto replay = registry.Invoke(
		"record",
		json{
			{ "action", "replay" },
			{ "file", file },
			{ "restoreScene", true },
		},
		ToolContext{});
	CHECK(!replay.ok);
	CHECK(replay.errorCode == 404);
	CHECK(loads == 0);
	CHECK(loadLasts == 0);
}

TEST_CASE("rolling replay resolves the latest save once and loads that exact name")
{
	TempRoot      root;
	RecordingStore store(root.path);
	const auto file = store.WriteUnique(RecordingDocument(
		json{
			{ "startState", "noPlayer" },
			{ "entryPoint",
				json{
					{ "kind", "save" },
					{ "value", "Autosave1" },
				} },
		}));

	ToolRegistry registry;
	EventBus     events;
	int          lists = 0;
	int          loads = 0;
	std::string  loadedName;
	registry.Register(
		Descriptor("game"),
		[&](const json& a_args, const ToolContext&) {
			const auto action = a_args.at("action").get<std::string>();
			if (action == "list")
			{
				++lists;
				return json{
					{ "saves",
						json::array({
							json{ { "name", "AutosaveNewest" } },
							json{ { "name", "AutosaveOlder" } },
						}) },
				};
			}
			if (action == "load")
			{
				++loads;
				loadedName = a_args.at("name").get<std::string>();
				const auto cursor = events.HeadSeq();
				events.Publish(
					"lifecycle",
					json{
						{ "event", "postLoadGame" },
						{ "success", true },
					});
				return json{
					{ "queued", true },
					{ "operationId", 7 },
					{ "eventCursor", cursor },
				};
			}
			if (action == "status")
				return json{
					{ "operation",
						json{
							{ "operationId", 7 },
							{ "success", true },
						} },
				};
			return json::object();
		});
	registry.Register(
		Descriptor("inspect", true),
		[](const json&, const ToolContext&) {
			return json{
				{ "playerLoaded", true },
				{ "menus", json{ { "open", json::array() } } },
			};
		});
	ScenarioService scenarios(registry, events);
	scenarios.Register();
	RecordingConfiguration configuration;
	configuration.root = root.path;
	RecordingService recordings(
		registry, events, scenarios, true, configuration, Backend());
	recordings.Register();

	const auto replay = registry.Invoke(
		"record",
		json{
			{ "action", "replay" },
			{ "file", file },
			{ "restoreScene", true },
			{ "settleMs", 0 },
			{ "async", false },
		},
		ToolContext{});
	CHECK_MESSAGE(replay.ok, replay.errorMessage);
	if (!replay.ok)
		return;
	CHECK(replay.value.at("ok") == true);
	CHECK(lists == 1);
	CHECK(loads == 1);
	CHECK(loadedName == "AutosaveNewest");
	CHECK(replay.value.at("restoration").at("rollingResolved") == true);
}

TEST_CASE("active replay commands are not fed back into an active recording")
{
	TempRoot      root;
	RecordingStore store(root.path);
	const auto replayFile = store.WriteUnique(RecordingDocument(
		json{ { "startState", "noPlayer" } },
		json::array({ json{ { "wait", 200 } } })));

	ToolRegistry registry;
	EventBus     events;
	ScenarioService scenarios(registry, events);
	scenarios.Register();
	RecordingConfiguration configuration;
	configuration.root = root.path;
	configuration.sampleInterval = 100ms;
	RecordingService recordings(
		registry, events, scenarios, false, configuration, Backend());
	recordings.Register();

	const auto started = registry.Invoke(
		"record", json{ { "action", "start" } }, ToolContext{});
	CHECK(started.ok);
	const auto replay = registry.Invoke(
		"record",
		json{
			{ "action", "replay" },
			{ "file", replayFile },
			{ "async", true },
		},
		ToolContext{});
	CHECK_MESSAGE(replay.ok, replay.errorMessage);
	if (!replay.ok)
		return;
	events.Publish(
		"console.command",
		json{
			{ "command", "player.getpos x" },
			{ "source", "broker" },
		});
	std::this_thread::sleep_for(30ms);
	const auto runId = replay.value.at("runId").get<std::uint64_t>();
	bool       done = false;
	for (int attempt = 0; attempt < 500; ++attempt)
	{
		if (scenarios.Status(runId).at("done").get<bool>())
		{
			done = true;
			break;
		}
		std::this_thread::sleep_for(1ms);
	}
	CHECK(done);
	const auto stopped = registry.Invoke(
		"record", json{ { "action", "stop" } }, ToolContext{});
	CHECK_MESSAGE(stopped.ok, stopped.errorMessage);
	if (!stopped.ok)
		return;
	const auto document =
		store.Load(stopped.value.at("file").get<std::string>());
	CHECK(document.at("activityEvents").empty());
}

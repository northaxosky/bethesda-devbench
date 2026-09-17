#include "test_framework.h"

#include "game/skyrimse/recording/RecordingActivity.h"
#include "game/skyrimse/vr/VRInputState.h"

#include <algorithm>

using dvb::json;
using dvb::skyrimse::recording::BuildVRTrackedSetReplay;
using dvb::skyrimse::recording::InterleaveReplayableActivity;
using dvb::skyrimse::vrinput::kMaximumVRTrackedDurationMs;
using dvb::skyrimse::vrinput::kMaximumVRTrackedFrames;

namespace
{
	json Button(std::int64_t a_ms, std::uint64_t a_seq, const char* a_device,
		const char* a_state, int a_id)
	{
		return json{ { "kind", "input" }, { "eventType", "button" },
			{ "device", a_device }, { "state", a_state }, { "idCode", a_id },
			{ "tMs", a_ms }, { "seq", a_seq } };
	}

	json Pose(int a_index)
	{
		return json{ { "available", true }, { "connected", true }, { "valid", true },
			{ "index", a_index }, { "trackingResult", 200 },
			{ "matrix", json::array({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 }) },
			{ "velocity", json::array({ 0, 0, 0 }) },
			{ "angularVelocity", json::array({ 0, 0, 0 }) } };
	}

	json TrackingSample(std::int64_t a_ms)
	{
		return json{ { "tMs", a_ms }, { "originCode", 1 }, { "hmd", Pose(0) },
			{ "left", Pose(1) }, { "right", Pose(2) } };
	}
}

TEST_CASE("Skyrim recording preserves short VR presses and role provenance")
{
	const json samples = json::array({ TrackingSample(20), TrackingSample(100) });
	json       down = Button(40, 1, "oculusPrimary", "down", 33);
	down["wandIndex"] = 2;
	json up = Button(60, 2, "oculusPrimary", "up", 33);
	up["wandIndex"] = 2;

	const json plan = BuildVRTrackedSetReplay(
		samples, json::array({ down, up }), "recording:test", true);
	CHECK(plan["step"]["tool"] == "input");
	CHECK(plan["step"]["args"]["device"] == "vrTrackedSet");
	CHECK(plan["step"]["args"]["owner"] == "recording:test");
	CHECK(plan["step"]["args"]["surviveLifecycle"] == true);
	CHECK(plan["step"]["args"]["tailMs"] == 50);
	const auto& frames = plan["step"]["args"]["frames"];
	CHECK(frames.size() == 5);
	CHECK(frames[0]["tMs"] == 0);
	CHECK(frames[2]["tMs"] == 40);
	CHECK(frames[2]["right"]["controller"]["pressed"].get<std::uint64_t>() ==
		  (std::uint64_t{ 1 } << 33));
	CHECK(frames[3]["tMs"] == 60);
	CHECK(frames[3]["right"]["controller"]["pressed"] == 0);
	CHECK(plan["durationMs"] == 150);
	CHECK(plan["report"]["convertedControllerEvents"] == 2);
	CHECK(plan["report"]["roleFallbackEvents"] == 0);

	const json fallbackPlan = BuildVRTrackedSetReplay(samples,
		json::array({ Button(40, 3, "oculusPrimary", "down", 33) }),
		"recording:test", true);
	CHECK(fallbackPlan["report"]["roleFallbackEvents"] == 1);
	CHECK(fallbackPlan["step"]["args"]["frames"][2]["right"]["controller"]["pressed"]
			  .get<std::uint64_t>() == (std::uint64_t{ 1 } << 33));
}

TEST_CASE("Skyrim recording keeps same-millisecond controller transitions distinct")
{
	const json samples = json::array({ TrackingSample(10), TrackingSample(20) });
	json       down = Button(10, 1, "oculusPrimary", "down", 33);
	down["wandIndex"] = 2;
	json up = Button(10, 2, "oculusPrimary", "up", 33);
	up["wandIndex"] = 2;

	const json plan = BuildVRTrackedSetReplay(
		samples, json::array({ down, up }), "recording:test", true);
	const auto& frames = plan["step"]["args"]["frames"];
	CHECK(frames[2]["tMs"] == 11);
	CHECK(frames[2]["right"]["controller"]["pressed"].get<std::uint64_t>() ==
		  (std::uint64_t{ 1 } << 33));
	CHECK(frames[3]["tMs"] == 12);
	CHECK(frames[3]["right"]["controller"]["pressed"] == 0);
	CHECK(plan["report"]["timestampAdjustedControllerEvents"] == 2);
}

TEST_CASE("Skyrim recording rejects malformed and over-budget VR provenance")
{
	CHECK_THROWS(BuildVRTrackedSetReplay(
		json::array({ json{ { "tMs", 20 }, { "hmd", json::object() } } }),
		json::array(), "recording:test", true));
	CHECK_THROWS(BuildVRTrackedSetReplay(
		json::array({ json{ { "tMs", 20 } }, json{ { "tMs", 20 } } }),
		json::array(), "recording:test", true));
	CHECK_THROWS(BuildVRTrackedSetReplay(
		json::array({ json{ { "tMs", kMaximumVRTrackedDurationMs + 1 } } }),
		json::array(), "recording:test", true));

	const json atLimit =
		json::array({ json{ { "tMs", kMaximumVRTrackedDurationMs } } });
	const json collision =
		Button(kMaximumVRTrackedDurationMs, 1, "oculusPrimary", "down", 33);
	CHECK_THROWS(BuildVRTrackedSetReplay(
		atLimit, json::array({ collision }), "recording:test", true));

	json samples = json::array();
	for (std::int64_t i = 1; i <= static_cast<std::int64_t>(kMaximumVRTrackedFrames); ++i)
		samples.push_back(json{ { "tMs", i } });
	CHECK_THROWS(BuildVRTrackedSetReplay(
		samples, json::array(), "recording:test", true));
}

TEST_CASE("Skyrim recording interleaves keyboard input without changing trajectory time")
{
	const json steps = json::array({
		json{ { "atMs", 125 }, { "pose", json::array({ 1, 2, 3, 4, 5 }) }, { "wait", 25 } },
	});
	const json events = json::array({
		Button(25, 1, "keyboard", "down", 17),
		Button(50, 2, "keyboard", "held", 17),
		Button(60, 3, "oculusSecondary", "down", 33),
		Button(75, 4, "keyboard", "up", 17),
	});
	const json plan =
		InterleaveReplayableActivity(steps, events, "recording:test", true);
	CHECK(plan["report"]["replayedKeyboardTransitions"] == 2);
	CHECK(plan["report"]["skippedInput"] == 2);
	CHECK(plan["report"]["partial"] == true);
	CHECK(plan["inputOwner"] == "recording:test");

	std::int64_t totalWait = 0;
	json         transitions = json::array();
	for (const auto& step : plan["steps"])
	{
		if (step.contains("wait"))
			totalWait += step["wait"].get<std::int64_t>();
		if (step.value("tool", std::string{}) == "input")
			transitions.push_back(step["args"]["action"]);
	}
	CHECK(totalWait == 150);
	CHECK(transitions == json::array({ "down", "up" }));

	const auto downStep = std::find_if(
		plan["steps"].begin(), plan["steps"].end(), [](const json& a_step) {
			return a_step.value("tool", std::string{}) == "input" &&
			       a_step["args"].value("action", std::string{}) == "down";
		});
	CHECK(downStep != plan["steps"].end());
	if (downStep != plan["steps"].end())
		CHECK((*downStep)["args"]["maxHoldMs"] == 2050);
}

TEST_CASE("Skyrim recording rejects malformed ordering and unsafe keyboard holds")
{
	const json badTime = json::array({
		json{ { "kind", "input" }, { "eventType", "button" }, { "device", "keyboard" },
			{ "state", "down" }, { "idCode", 30 }, { "tMs", "now" }, { "seq", 1 } },
	});
	CHECK_THROWS(InterleaveReplayableActivity(
		json::array(), badTime, "recording:test", true));

	const json badSequence = json::array({
		json{ { "kind", "input" }, { "eventType", "button" }, { "device", "keyboard" },
			{ "state", "down" }, { "idCode", 30 }, { "tMs", 1 }, { "seq", "first" } },
		json{ { "kind", "input" }, { "eventType", "button" }, { "device", "keyboard" },
			{ "state", "up" }, { "idCode", 30 }, { "tMs", 1 }, { "seq", 2 } },
	});
	CHECK_THROWS(InterleaveReplayableActivity(
		json::array(), badSequence, "recording:test", true));

	const json atLimit = json::array({
		Button(0, 1, "keyboard", "down", 17),
		Button(58000, 2, "keyboard", "up", 17),
	});
	const json safe =
		InterleaveReplayableActivity(json::array(), atLimit, "recording:test", true);
	CHECK(safe["steps"][0]["args"]["maxHoldMs"] == 60000);

	const json overLimit = json::array({
		Button(0, 1, "keyboard", "down", 17),
		Button(58001, 2, "keyboard", "up", 17),
	});
	CHECK_THROWS(InterleaveReplayableActivity(
		json::array(), overLimit, "recording:test", true));
}

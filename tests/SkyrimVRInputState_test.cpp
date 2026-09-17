#include "test_framework.h"

#include "game/skyrimse/vr/VRInputState.h"

using dvb::json;
using dvb::skyrimse::vrinput::ParseVRTrackedInputFrames;
using dvb::skyrimse::vrinput::VRSequenceFinishAction;
using dvb::skyrimse::vrinput::VRSequenceStopAccess;
using dvb::skyrimse::vrinput::VRSequenceTransaction;
using dvb::skyrimse::vrinput::VRTrackedInputFrameJson;

namespace
{
	json Pose(int a_index)
	{
		return json{ { "available", true }, { "connected", true }, { "valid", true },
			{ "index", a_index }, { "trackingResult", 200 },
			{ "matrix", json::array({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 }) },
			{ "velocity", json::array({ 0, 0, 0 }) },
			{ "angularVelocity", json::array({ 0, 0, 0 }) } };
	}

	json Controller(int a_index)
	{
		json out = Pose(a_index);
		out["controller"] = json{ { "packetNumber", 4 }, { "pressed", 8 }, { "touched", 8 },
			{ "axes", json::array({ json::array({ 0.25, -0.5 }), json::array({ 0, 0 }),
						  json::array({ 0, 0 }), json::array({ 0, 0 }), json::array({ 0, 0 }) }) } };
		return out;
	}

	json Frame(std::int64_t a_tMs)
	{
		return json{ { "tMs", a_tMs }, { "originCode", 1 }, { "hmd", Pose(0) },
			{ "left", Controller(1) }, { "right", Controller(2) } };
	}
}

TEST_CASE("Skyrim VR validates complete atomic pose sets before activation")
{
	const auto frames = ParseVRTrackedInputFrames(json::array({ Frame(0), Frame(20) }));
	CHECK(frames.size() == 2);
	CHECK(frames[1].tMs == 20);
	CHECK(frames[0].right.controller.pressed == 8);
	const json encoded = VRTrackedInputFrameJson(frames[0]);
	CHECK(encoded["hmd"]["index"] == 0);
	CHECK(encoded["left"]["controller"]["axes"][0][0] == 0.25f);

	json missing = Frame(0);
	missing.erase("right");
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ missing })));
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(10) })));
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), Frame(20), Frame(10) })));
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), Frame(0) })));

	json duplicate = Frame(0);
	duplicate["right"]["index"] = 1;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ duplicate })));

	json hmdAlias = Frame(0);
	hmdAlias["left"]["index"] = 0;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ hmdAlias })));

	json changedIdentity = Frame(20);
	changedIdentity["right"]["index"] = 3;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), changedIdentity })));

	json changedOrigin = Frame(20);
	changedOrigin["originCode"] = 0;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), changedOrigin })));

	json badAxis = Frame(0);
	badAxis["left"]["controller"]["axes"][0][0] = 2.0;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ badAxis })));

	json badBoolean = Frame(0);
	badBoolean["hmd"]["available"] = "yes";
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ badBoolean })));

	json overflow = Frame(0);
	overflow["left"]["controller"]["packetNumber"] = 4294967296ULL;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ overflow })));
}

TEST_CASE("Skyrim VR keeps an applied override reserved until restoration succeeds")
{
	VRSequenceTransaction state;
	CHECK(state.Begin("owner", "secret", false));
	const auto generation = state.Generation();
	CHECK(state.MarkIndicesApplied(generation));
	CHECK(state.Commit(generation));

	CHECK(state.AuthorizeStop("owner", "wrong", false, false) ==
		  VRSequenceStopAccess::kControlTokenMismatch);
	CHECK(state.AuthorizeStop("other", "secret", false, false) ==
		  VRSequenceStopAccess::kOwnerMismatch);
	CHECK(state.AuthorizeStop("owner", "secret", true, false) ==
		  VRSequenceStopAccess::kForceRequiresInternal);
	CHECK(state.AuthorizeStop("other", "", true, true) ==
		  VRSequenceStopAccess::kAllowed);

	const auto decision = state.CancelForLifecycle();
	CHECK(decision.present);
	CHECK(!decision.preserved);
	CHECK(decision.owner == "owner");
	CHECK(decision.generation == generation);
	CHECK(state.Restoring());
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kRestore);
	CHECK(state.RestoreAttemptActive());
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kNone);

	state.RestoreFailed(generation);
	CHECK(state.Restoring());
	CHECK(!state.RestoreAttemptActive());
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kRestore);
	CHECK(state.RestoreSucceeded(generation));
	CHECK(!state.Busy());
	CHECK(!state.IndicesApplied());
}

TEST_CASE("Skyrim VR returns to pass-through when no native override was applied")
{
	VRSequenceTransaction cancelled;
	CHECK(cancelled.Begin("owner", "secret", false));
	const auto generation = cancelled.Generation();
	const auto decision = cancelled.CancelForLifecycle();
	CHECK(decision.present);
	CHECK(!decision.preserved);
	CHECK(!cancelled.CanApplyIndices(generation));
	CHECK(cancelled.ClaimFinish(generation) == VRSequenceFinishAction::kPublish);
	CHECK(!cancelled.Busy());

	VRSequenceTransaction replay;
	CHECK(replay.Begin("recording", "secret", true));
	const auto preserved = replay.CancelForLifecycle();
	CHECK(preserved.present);
	CHECK(preserved.preserved);
	CHECK(replay.Starting());
}

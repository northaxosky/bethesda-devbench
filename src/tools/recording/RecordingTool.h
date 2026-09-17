#pragma once

#include "EventBus.h"
#include "ToolRegistry.h"
#include "tools/recording/RecordingModel.h"
#include "tools/scenario/ScenarioTool.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace dvb
{
	struct Config;
}

namespace dvb::tools::recording
{
	struct RecordingConfiguration
	{
		std::filesystem::path     root =
			"Data/F4SE/Plugins/devbench/recordings";
		std::chrono::milliseconds sampleInterval{ 10 };
		std::chrono::milliseconds loadSettle{ 3000 };
		std::chrono::milliseconds captureSettle{ 500 };
		std::chrono::milliseconds couplingAnchor{ 10000 };
		std::chrono::milliseconds couplingCell{ 60000 };
		bool                      cleanTransition = true;
		std::string               cleanTransitionCell = "QASmoke";
	};

	RecordingConfiguration RecordingConfigurationFromConfig(const Config& a_config);

	struct RecordingTransitionReceipt
	{
		bool          queued = false;
		std::uint64_t actionCursor = 0;
		std::string   mode;
	};

	struct RecordingBackend
	{
		std::string              gameId = "fo4";
		std::string              gameName = "Fallout 4";
		std::vector<std::string> compatibleGameIds{
			"fo4", "fallout4", "fallout 4"
		};
		std::vector<std::string> compatibleRuntimeVariants{ "ae" };
		bool                     recordedOnVR = false;

		// Main-thread-safe snapshot containing playerLoaded, frame, pose
		// [x,y,z,yawDeg,pitchDeg], and scene identities.
		std::function<json()> snapshot;

		// Native CELL replay primitive. The service calls it only for interior
		// targets after checking game-action permission. Exterior targets are
		// lowered to numeric WRLD COW through the registered, independently
		// permission-checked console tool. A numeric CELL FormID is never
		// reinterpreted as a COC editor-id token.
		std::function<RecordingTransitionReceipt(const json&)> transition;

		// Optional native activity seams. Input adapters publish normalized events
		// on EventBus topic "input.activity". VR adapters may additionally provide
		// a synchronized tracked-set snapshot and a replay-plan builder; the shared
		// RecordingService remains the sole record/replay tool implementation.
		std::function<json()> trackingSnapshot;
		std::function<json()> activityCaptureContract;
		std::function<json(
			const json&, const json&, const std::string&, bool)>
			buildTrackedInputReplay;
	};

	ToolDescriptor BuildRecordDescriptor(const RecordingBackend* a_backend = nullptr);
	ToolDescriptor BuildRecordingsDescriptor();

	class RecordingService
	{
	public:
		RecordingService(
			ToolRegistry&                      a_registry,
			EventBus&                          a_events,
			scenario::ScenarioService&         a_scenarios,
			bool                               a_allowGameActions,
			RecordingConfiguration             a_configuration,
			RecordingBackend                   a_backend);
		~RecordingService();

		RecordingService(const RecordingService&) = delete;
		RecordingService& operator=(const RecordingService&) = delete;

		bool Register();
		void Shutdown() noexcept;

	private:
		struct State;
		std::shared_ptr<State> state_;
	};
}

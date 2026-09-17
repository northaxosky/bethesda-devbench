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
		// Main-thread-safe snapshot containing playerLoaded, frame, pose
		// [x,y,z,yawDeg,pitchDeg], and scene identities.
		std::function<json()> snapshot;

		// Native CELL replay primitive. The service calls it only for interior
		// targets after checking game-action permission. Exterior targets are
		// lowered to numeric WRLD COW through the registered, independently
		// permission-checked console tool. A numeric CELL FormID is never
		// reinterpreted as a COC editor-id token.
		std::function<RecordingTransitionReceipt(const json&)> transition;
	};

	ToolDescriptor BuildRecordDescriptor();
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

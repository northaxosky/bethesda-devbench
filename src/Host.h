#pragma once

#include "Config.h"
#include "GameProfile.h"
#include "tools/ConsoleTool.h"
#include "tools/GameTool.h"
#include "tools/InspectTool.h"
#include "tools/camera/CameraTool.h"
#include "tools/capture/CaptureTool.h"
#include "tools/input/InputTool.h"
#include "tools/menu/MenuTool.h"
#include "tools/papyrus/PapyrusTool.h"
#include "tools/recording/RecordingTool.h"
#include "tools/rest/RestTool.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dvb
{
	class EventBus;
	class ToolRegistry;

	struct ConsoleBinding
	{
		tools::ConsoleBackend backend;
		std::shared_ptr<void> lifetime;
	};

	struct HostAdapter
	{
		GameProfile profile;
		std::string runtimeVersion;

		std::function<ConsoleBinding(EventBus&)> console;
		tools::GameBackend                       game;
		tools::InspectBackend                    inspect;
		tools::input::InputBackend               input;
		tools::MenuBackend                       menu;
		tools::CameraBackend                     camera;
		tools::papyrus::PapyrusBackend           papyrus;
		tools::capture::CaptureBackend           capture;
		tools::recording::RecordingBackend       recording;
		tools::rest::RestBackend                 rest;
		tools::rest::RestOperationCoordinator    restOperations;

		std::function<void(EventBus&)> initializeLifecycle;
		std::function<void()>          resetLifecycle;
		std::function<bool()>          ready;
		std::function<std::vector<std::string>()> openMenus;
		std::function<void(
			ToolRegistry&, EventBus&, const Config&)> onStarted;
		std::function<void()> shutdown;
	};

	// Owns the one game-agnostic 15-tool/service stack. Native entrypoints only
	// initialize RuntimeContext, assemble HostAdapter, and forward lifecycle edges.
	class Host
	{
	public:
		Host();
		~Host();

		Host(const Host&) = delete;
		Host& operator=(const Host&) = delete;

		bool Start(HostAdapter a_adapter);
		void Stop() noexcept;

		void SetInputReady(bool a_ready) noexcept;
		void ReleaseInputForLifecycle(std::string a_reason) noexcept;

		ToolRegistry* Tools() noexcept;
		EventBus*     Events() noexcept;
		const Config* Configuration() const noexcept;
		bool          Running() const noexcept;

	private:
		struct Runtime;
		std::unique_ptr<Runtime> runtime_;
	};
}

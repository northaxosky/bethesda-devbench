#include "pch.h"

#include "Config.h"
#include "GameState.h"
#include "RuntimePaths.h"
#include "Server.h"
#include "ToolExtensions.h"
#include "Version.h"
#include "game/fallout4/ConsoleBackend.h"
#include "game/fallout4/GameBackend.h"
#include "game/fallout4/InspectBackend.h"
#include "game/fallout4/Lifecycle.h"
#include "game/fallout4/camera/CameraBackend.h"
#include "game/fallout4/capture/CaptureBackend.h"
#include "game/fallout4/input/InputBackend.h"
#include "game/fallout4/menu/MenuBackend.h"
#include "game/fallout4/papyrus/PapyrusBackend.h"
#include "game/fallout4/recording/RecordingBackend.h"
#include "game/fallout4/rest/RestBackend.h"
#include "tools/BridgeSetupTool.h"
#include "tools/ConsoleTool.h"
#include "tools/ExtensionDescriptorRefresh.h"
#include "tools/GameTool.h"
#include "tools/InspectTool.h"
#include "tools/camera/CameraTool.h"
#include "tools/capture/CaptureTool.h"
#include "tools/health/StallWatchdog.h"
#include "tools/input/InputTool.h"
#include "tools/menu/MenuTool.h"
#include "tools/papyrus/PapyrusTool.h"
#include "tools/recording/RecordingTool.h"
#include "tools/rest/RestTool.h"
#include "tools/scenario/ScenarioTool.h"
#include "xse/HostApi.h"

#include <spdlog/spdlog.h>

namespace
{
	struct Runtime
	{
		dvb::Server                                              server;
		dvb::tools::scenario::ScenarioService                    scenarios;
		std::shared_ptr<dvb::tools::input::InputService>         input;
		std::unique_ptr<dvb::tools::health::StallWatchdog>       liveness;
		std::unique_ptr<dvb::fallout4::ConsoleBackendLifetime>   console;
		std::unique_ptr<dvb::tools::recording::RecordingService> recording;
		std::shared_ptr<dvb::tools::MenuService>                 menu;
		std::shared_ptr<dvb::tools::CameraService>               camera;
		std::shared_ptr<dvb::tools::capture::CaptureService>     capture;
		std::shared_ptr<dvb::tools::rest::RestService>           rest;
		std::optional<dvb::EventBus::SubId>                      restReadiness;
		std::unique_ptr<dvb::tools::ExtensionDescriptorRefresh>  extensions;

		explicit Runtime(int a_port) :
			server("127.0.0.1", a_port),
			scenarios(server.Tools(), server.Events())
		{}

		~Runtime()
		{
			extensions.reset();
			liveness.reset();
			if (restReadiness)
				server.Events().Unsubscribe(*restReadiness);
			if (rest)
				rest->Shutdown();
			if (recording)
				recording->Shutdown();
			// Join scenario work and drain HTTP handlers before their service or registry is destroyed.
			scenarios.Shutdown();
			if (input)
				input->Shutdown();
			server.Stop();
			console.reset();
			dvb::HostApi::Reset();
			dvb::fallout4::Lifecycle::Reset();
			dvb::ToolExtensions::SetChangeListener({});
		}
	};

	std::unique_ptr<Runtime> g_runtime;
	std::string              g_runtimeVersion;

	void LogCallbackError(std::string_view a_context, const char* a_detail = nullptr) noexcept
	{
		try
		{
			if (a_detail)
				REX::ERROR("devbench: {}: {}", a_context, a_detail);
			else
				REX::ERROR("devbench: {}", a_context);
		}
		catch (...)
		{}
	}

	void ApplyLogLevel(const std::string& a_level)
	{
		auto level = spdlog::level::from_str(a_level);
		if (level == spdlog::level::off && a_level != "off")
		{
			REX::WARN("devbench: unknown logLevel '{}'; keeping info", a_level);
			level = spdlog::level::info;
		}
		spdlog::set_level(level);
		spdlog::flush_on(level);
	}

	void StartServer() noexcept
	{
		try
		{
			if (g_runtime)
				return;

			const dvb::Config config = dvb::LoadConfig();
			ApplyLogLevel(config.logLevel);
			if (!config.enabled)
			{
				REX::INFO("devbench: server disabled via config; not starting");
				return;
			}

			auto  runtime = std::make_unique<Runtime>(config.port);
			auto& server = runtime->server;
			server.Events().SetFrameProvider(&dvb::game::CurrentFrame);
			dvb::HostApi::Init(server.Tools(), server.Events());
			runtime->console = std::make_unique<dvb::fallout4::ConsoleBackendLifetime>(server.Events());
			dvb::tools::RegisterConsoleTool(
				server.Tools(), config.allowConsoleCommands, runtime->console->Backend());
			dvb::tools::RegisterGameTool(
				server.Tools(), config.allowGameActions, dvb::fallout4::MakeGameBackend());
			auto inspection = dvb::fallout4::MakeInspectBackend(g_runtimeVersion);
			dvb::tools::RegisterInspectTool(server.Tools(), inspection, false);
			dvb::tools::RegisterBridgeSetupTool(server.Tools(), &dvb::BridgeDiscoveryInfo);
			dvb::tools::papyrus::RegisterPapyrusTool(
				server.Tools(), config.allowPapyrusCalls, dvb::fallout4::papyrus::MakePapyrusBackend());
			runtime->input = dvb::tools::input::RegisterInputTool(
				server.Tools(), server.Events(), config.allowControlActions, dvb::fallout4::MakeInputBackend());
			runtime->menu = dvb::tools::RegisterMenuTool(
				server.Tools(), config.allowControlActions, dvb::fallout4::MakeMenuBackend(g_runtimeVersion));
			runtime->camera = dvb::tools::RegisterCameraTool(
				server.Tools(), config.allowControlActions, dvb::fallout4::MakeCameraBackend(g_runtimeVersion));
			runtime->capture = dvb::tools::capture::RegisterCaptureTool(
				server.Tools(), server.Events(), dvb::tools::capture::CaptureConfigurationFromConfig(config),
				dvb::fallout4::capture::MakeCaptureBackend(inspection.scene));
			dvb::fallout4::Lifecycle::Initialize(server.Events());
			runtime->rest = dvb::tools::rest::RegisterRestTools(
				server.Tools(), config.allowGameActions, dvb::fallout4::MakeRestBackend(),
				dvb::fallout4::MakeRestOperationCoordinator());
			runtime->restReadiness = server.Events().Subscribe(
				[weak = std::weak_ptr(runtime->rest)](const dvb::EventBus::Event& a_event) {
					if (a_event.topic != "lifecycle" && a_event.topic != "menu")
						return;
					try
					{
						if (const auto service = weak.lock())
						{
							const auto snapshot = dvb::fallout4::Lifecycle::GetSnapshot();
							service->SetReady(snapshot.gameDataReady && snapshot.gameLoaded &&
											  !snapshot.inMainMenu && !snapshot.inLoadingMenu &&
											  !snapshot.loadInProgress && snapshot.postLoadSucceeded != false);
						}
					}
					catch (const std::exception& a_error)
					{
						LogCallbackError("rest readiness update failed", a_error.what());
					}
					catch (...)
					{
						LogCallbackError("rest readiness update failed");
					}
				});
			if (config.stallWatchdogMs > 0)
			{
				runtime->liveness = std::make_unique<dvb::tools::health::StallWatchdog>(
					server.Events(), std::chrono::milliseconds(config.stallWatchdogMs),
					&dvb::game::CurrentFrame,
					[] { return dvb::fallout4::Lifecycle::GetSnapshot().openMenus; });
			}
			runtime->scenarios.Register();
			auto recordingConfig = dvb::tools::recording::RecordingConfigurationFromConfig(config);
			if (recordingConfig.root.is_relative())
				recordingConfig.root = dvb::RuntimeGameDirectory() / recordingConfig.root;
			runtime->recording = std::make_unique<dvb::tools::recording::RecordingService>(
				server.Tools(), server.Events(), runtime->scenarios, config.allowGameActions,
				std::move(recordingConfig), dvb::fallout4::MakeRecordingBackend());
			runtime->recording->Register();
			runtime->extensions = std::make_unique<dvb::tools::ExtensionDescriptorRefresh>(
				std::vector<dvb::tools::ExtensionDescriptorRefresh::Callback>{
					{ "inspect", [registry = &server.Tools(), backend = std::move(inspection)] {
						 dvb::tools::RegisterInspectTool(*registry, backend, false);
					 } },
					{ "menu", [service = runtime->menu] { service->RefreshDescriptor(); } },
					{ "capture", [registry = &server.Tools(), service = runtime->capture] {
						 service->RefreshDescriptor(*registry);
					 } },
				});
			dvb::tools::capture::RegisterScreenshotInspectionExtension(runtime->capture);
			if (!server.Start())
			{
				LogCallbackError("startup aborted because the MCP/REST server failed to start");
				return;
			}
			g_runtime = std::move(runtime);
		}
		catch (const std::exception& a_exception)
		{
			g_runtime.reset();
			dvb::HostApi::Reset();
			dvb::fallout4::Lifecycle::Reset();
			dvb::ToolExtensions::SetChangeListener({});
			LogCallbackError("startup aborted", a_exception.what());
		}
		catch (...)
		{
			g_runtime.reset();
			dvb::HostApi::Reset();
			dvb::fallout4::Lifecycle::Reset();
			dvb::ToolExtensions::SetChangeListener({});
			LogCallbackError("startup aborted: unknown exception");
		}
	}

	void F4SEAPI OnInterfaceMessage(F4SE::MessagingInterface::Message* a_message) noexcept
	{
		try
		{
			dvb::HostApi::OnInterfaceRequest(a_message);
		}
		catch (const std::exception& a_exception)
		{
			LogCallbackError("interface message failed", a_exception.what());
		}
		catch (...)
		{
			LogCallbackError("interface message failed");
		}
	}

	void F4SEAPI OnF4SEMessage(F4SE::MessagingInterface::Message* a_message) noexcept
	{
		if (!a_message)
			return;

		try
		{
			if (a_message->type == F4SE::MessagingInterface::kPostLoad)
				StartServer();
			if (g_runtime && g_runtime->input)
			{
				// AE can finish device initialization without emitting the legacy inputLoaded message.
				if (a_message->type == F4SE::MessagingInterface::kInputLoaded ||
					a_message->type == F4SE::MessagingInterface::kGameDataReady)
					g_runtime->input->SetReady(true);
				if (a_message->type == F4SE::MessagingInterface::kPreLoadGame ||
					a_message->type == F4SE::MessagingInterface::kNewGame)
				{
					g_runtime->input->ReleaseForLifecycle(
						a_message->type == F4SE::MessagingInterface::kPreLoadGame ? "preLoadGame" : "newGame");
				}
			}
			dvb::fallout4::Lifecycle::OnF4SEMessage(
				a_message->type, a_message->dataLen, a_message->data);
		}
		catch (const std::exception& a_exception)
		{
			LogCallbackError("F4SE lifecycle message failed", a_exception.what());
		}
		catch (...)
		{
			LogCallbackError("F4SE lifecycle message failed");
		}
	}

	bool RegisterAllSenders(const F4SE::LoadInterface* a_f4se)
	{
		// CommonLibF4's string_view wrapper cannot pass nullptr, but F4SE uses nullptr
		// to subscribe to every third-party sender. Call the underlying ABI explicitly.
		const auto messaging = static_cast<F4SE::Impl::F4SEMessagingInterface*>(
			a_f4se->QueryInterface(F4SE::LoadInterface::kMessaging));
		return messaging &&
		       messaging->RegisterListener(
				   F4SE::GetPluginHandle(), nullptr, reinterpret_cast<void*>(&OnInterfaceMessage));
	}

	bool InitPlugin(const F4SE::LoadInterface* a_f4se)
	{
		F4SE::Init(a_f4se, F4SE::InitInfo{});
		g_runtimeVersion = a_f4se->RuntimeVersion().string(".");
		REX::INFO("devbench {} loaded", DEVBENCH_VERSION_STRING);

		const auto messaging = F4SE::GetMessagingInterface();
		if (!messaging || !messaging->RegisterListener(&OnF4SEMessage))
		{
			REX::ERROR("F4SE lifecycle listener registration failed");
			return false;
		}
		if (!RegisterAllSenders(a_f4se))
		{
			REX::ERROR("F4SE all-sender listener registration failed; cross-plugin API unavailable");
			return false;
		}
		return true;
	}
}

F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	if (const auto data = F4SE::PluginVersionData::GetSingleton())
	{
		a_info->infoVersion = F4SE::PluginInfo::kVersion;
		a_info->name = data->GetPluginName().data();
		a_info->version = data->GetPluginVersion().pack();
	}

	return !a_f4se->IsEditor();
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	try
	{
		return InitPlugin(a_f4se);
	}
	catch (const std::exception& a_exception)
	{
		LogCallbackError("plugin load failed", a_exception.what());
		return false;
	}
	catch (...)
	{
		LogCallbackError("plugin load failed");
		return false;
	}
}

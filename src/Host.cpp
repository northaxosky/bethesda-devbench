#include "Host.h"

#include "GameState.h"
#include "HostApi.h"
#include "Log.h"
#include "RuntimePaths.h"
#include "Server.h"
#include "ToolExtensions.h"
#include "tools/BridgeSetupTool.h"
#include "tools/ExtensionDescriptorRefresh.h"
#include "tools/health/StallWatchdog.h"
#include "tools/scenario/ScenarioTool.h"

#include <spdlog/spdlog.h>

namespace dvb
{
	namespace
	{
		void ApplyLogLevel(const std::string& a_level)
		{
			auto level = spdlog::level::from_str(a_level);
			if (level == spdlog::level::off && a_level != "off")
			{
				logs::warn(
					"devbench: unknown logLevel '{}'; keeping info", a_level);
				level = spdlog::level::info;
			}
			spdlog::set_level(level);
			spdlog::flush_on(level);
		}
	}

	struct Host::Runtime
	{
		HostAdapter                                             adapter;
		Config                                                  config;
		Server                                                  server;
		tools::scenario::ScenarioService                        scenarios;
		std::shared_ptr<tools::input::InputService>             input;
		std::unique_ptr<tools::health::StallWatchdog>           liveness;
		std::shared_ptr<void>                                   consoleLifetime;
		std::unique_ptr<tools::recording::RecordingService>     recording;
		std::shared_ptr<tools::MenuService>                     menu;
		std::shared_ptr<tools::CameraService>                   camera;
		std::shared_ptr<tools::capture::CaptureService>         capture;
		std::shared_ptr<tools::rest::RestService>               rest;
		std::optional<EventBus::SubId>                          restReadiness;
		std::unique_ptr<tools::ExtensionDescriptorRefresh>      extensions;

		Runtime(HostAdapter a_adapter, Config a_config) :
			adapter(std::move(a_adapter)),
			config(std::move(a_config)),
			server("127.0.0.1", config.port),
			scenarios(server.Tools(), server.Events())
		{}

		~Runtime()
		{
			Shutdown();
		}

		void Shutdown() noexcept
		{
			if (adapter.shutdown)
			{
				try
				{
					adapter.shutdown();
				}
				catch (...)
				{}
				adapter.shutdown = {};
			}
			extensions.reset();
			liveness.reset();
			if (restReadiness)
			{
				server.Events().Unsubscribe(*restReadiness);
				restReadiness.reset();
			}
			if (rest)
				rest->Shutdown();
			if (recording)
				recording->Shutdown();
			scenarios.Shutdown();
			if (input)
				input->Shutdown();
			server.Stop();
			consoleLifetime.reset();
			HostApi::Reset();
			if (adapter.resetLifecycle)
				adapter.resetLifecycle();
			ToolExtensions::SetChangeListener({});
		}
	};

	Host::Host() = default;

	Host::~Host()
	{
		Stop();
	}

	bool Host::Start(HostAdapter a_adapter)
	{
		if (runtime_)
			return true;
		if (a_adapter.profile.id.empty())
			throw std::invalid_argument("host adapter requires a game profile");
		if (!a_adapter.console || !a_adapter.initializeLifecycle ||
			!a_adapter.resetLifecycle || !a_adapter.ready ||
			!a_adapter.openMenus)
			throw std::invalid_argument(
				"host adapter is missing lifecycle or console providers");

		auto config = LoadConfig(a_adapter.profile);
		ApplyLogLevel(config.logLevel);
		if (!config.enabled)
		{
			logs::info("{}", "devbench: server disabled via config; not starting");
			return false;
		}

		auto runtime =
			std::make_unique<Runtime>(std::move(a_adapter), std::move(config));
		auto& server = runtime->server;
		server.Events().SetFrameProvider(&game::CurrentFrame);
		HostApi::Init(server.Tools(), server.Events());

		auto console = runtime->adapter.console(server.Events());
		runtime->consoleLifetime = std::move(console.lifetime);
		tools::RegisterConsoleTool(
			server.Tools(), runtime->config.allowConsoleCommands,
			std::move(console.backend));
		tools::RegisterGameTool(
			server.Tools(), runtime->config.allowGameActions,
			runtime->adapter.game);
		auto inspection = runtime->adapter.inspect;
		tools::RegisterInspectTool(server.Tools(), inspection, false);
		tools::RegisterBridgeSetupTool(server.Tools(), &BridgeDiscoveryInfo);
		tools::papyrus::RegisterPapyrusTool(
			server.Tools(), runtime->config.allowPapyrusCalls,
			runtime->adapter.papyrus);
		runtime->input = tools::input::RegisterInputTool(
			server.Tools(), server.Events(),
			runtime->config.allowControlActions, runtime->adapter.input);
		runtime->menu = tools::RegisterMenuTool(
			server.Tools(), runtime->config.allowControlActions,
			runtime->adapter.menu);
		runtime->camera = tools::RegisterCameraTool(
			server.Tools(), runtime->config.allowControlActions,
			runtime->adapter.camera);
		runtime->capture = tools::capture::RegisterCaptureTool(
			server.Tools(), server.Events(),
			tools::capture::CaptureConfigurationFromConfig(runtime->config),
			runtime->adapter.capture);

		runtime->adapter.initializeLifecycle(server.Events());
		runtime->rest = tools::rest::RegisterRestTools(
			server.Tools(), runtime->config.allowGameActions,
			runtime->adapter.rest, runtime->adapter.restOperations);
		runtime->restReadiness = server.Events().Subscribe(
			[weak = std::weak_ptr(runtime->rest),
			 ready = runtime->adapter.ready](const EventBus::Event& a_event) {
				if (a_event.topic != "lifecycle" && a_event.topic != "menu")
					return;
				try
				{
					if (const auto service = weak.lock())
						service->SetReady(ready());
				}
				catch (const std::exception& a_error)
				{
					logs::error(
						"devbench: rest readiness update failed: {}",
						a_error.what());
				}
				catch (...)
				{
					logs::error(
						"{}", "devbench: rest readiness update failed");
				}
			});
		if (runtime->config.stallWatchdogMs > 0)
		{
			runtime->liveness =
				std::make_unique<tools::health::StallWatchdog>(
					server.Events(),
					std::chrono::milliseconds(
						runtime->config.stallWatchdogMs),
					&game::CurrentFrame, runtime->adapter.openMenus);
		}
		runtime->scenarios.Register();
		auto recordingConfig =
			tools::recording::RecordingConfigurationFromConfig(runtime->config);
		recordingConfig.root = runtime->adapter.profile.recordingsDirectory;
		if (recordingConfig.root.is_relative())
			recordingConfig.root =
				RuntimeGameDirectory() / recordingConfig.root;
		runtime->recording =
			std::make_unique<tools::recording::RecordingService>(
				server.Tools(), server.Events(), runtime->scenarios,
				runtime->config.allowGameActions, std::move(recordingConfig),
				runtime->adapter.recording);
		runtime->recording->Register();
		runtime->extensions =
			std::make_unique<tools::ExtensionDescriptorRefresh>(
				std::vector<tools::ExtensionDescriptorRefresh::Callback>{
					{ "inspect",
						[registry = &server.Tools(),
						 backend = std::move(inspection)] {
							tools::RegisterInspectTool(
								*registry, backend, false);
						} },
					{ "menu",
						[service = runtime->menu] {
							service->RefreshDescriptor();
						} },
					{ "capture",
						[registry = &server.Tools(),
						 service = runtime->capture] {
							service->RefreshDescriptor(*registry);
						} },
				});
		tools::capture::RegisterScreenshotInspectionExtension(runtime->capture);
		if (runtime->adapter.onStarted)
			runtime->adapter.onStarted(
				server.Tools(), server.Events(), runtime->config);
		if (!server.Start())
			return false;
		runtime_ = std::move(runtime);
		return true;
	}

	void Host::Stop() noexcept
	{
		runtime_.reset();
	}

	void Host::SetInputReady(bool a_ready) noexcept
	{
		if (runtime_ && runtime_->input)
			runtime_->input->SetReady(a_ready);
	}

	void Host::ReleaseInputForLifecycle(std::string a_reason) noexcept
	{
		if (runtime_ && runtime_->input)
			runtime_->input->ReleaseForLifecycle(std::move(a_reason));
	}

	ToolRegistry* Host::Tools() noexcept
	{
		return runtime_ ? std::addressof(runtime_->server.Tools()) : nullptr;
	}

	EventBus* Host::Events() noexcept
	{
		return runtime_ ? std::addressof(runtime_->server.Events()) : nullptr;
	}

	const Config* Host::Configuration() const noexcept
	{
		return runtime_ ? std::addressof(runtime_->config) : nullptr;
	}

	bool Host::Running() const noexcept
	{
		return runtime_ && runtime_->server.Running();
	}
}

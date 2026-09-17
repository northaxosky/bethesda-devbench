#include "pch.h"

#include "Adapter.h"
#include "HostApi.h"
#include "RuntimeContext.h"
#include "Version.h"
#include "Lifecycle.h"
#include "camera/CameraBackend.h"
#include "ui/Frontends.h"

#include <spdlog/sinks/basic_file_sink.h>

namespace
{
	dvb::Host g_host;

	void InitLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path)
			return;
		*path /= std::format(
			"{}.log", SKSE::PluginDeclaration::GetSingleton()->GetName());
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
			path->string(), true);
		auto log =
			std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%^%L%$] %v");
	}

	void LogCallbackError(
		std::string_view a_context, const char* a_detail = nullptr) noexcept
	{
		try
		{
			if (a_detail)
				logs::error("devbench: {}: {}", a_context, a_detail);
			else
				logs::error("devbench: {}", a_context);
		}
		catch (...)
		{}
	}

	void OnInterfaceMessage(
		SKSE::MessagingInterface::Message* a_message) noexcept
	{
		if (!a_message)
			return;
		try
		{
			dvb::HostApi::OnInterfaceRequest({
				.type = a_message->type,
				.dataLength = a_message->dataLen,
				.data = a_message->data,
				.sender = a_message->sender,
			});
		}
		catch (const std::exception& a_error)
		{
			LogCallbackError("interface message failed", a_error.what());
		}
		catch (...)
		{
			LogCallbackError("interface message failed");
		}
	}

	void StartHost(std::string a_runtimeVersion) noexcept
	{
		try
		{
			if (!g_host.Start(
					dvb::skyrimse::MakeHostAdapter(
						std::move(a_runtimeVersion))))
				logs::info(
					"{}", "devbench: native host did not start (disabled or unavailable)");
		}
		catch (const std::exception& a_error)
		{
			g_host.Stop();
			LogCallbackError("startup aborted", a_error.what());
		}
		catch (...)
		{
			g_host.Stop();
			LogCallbackError("startup aborted: unknown exception");
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message) noexcept
	{
		if (!a_message)
			return;
		try
		{
			if (a_message->type == SKSE::MessagingInterface::kPreLoadGame)
				dvb::skyrimse::CameraPreLoad();
			else if (
				a_message->type == SKSE::MessagingInterface::kNewGame ||
				a_message->type ==
					SKSE::MessagingInterface::kPostLoadGame)
				dvb::skyrimse::CameraPostLoad();

			if (a_message->type == SKSE::MessagingInterface::kPostLoad)
			{
				StartHost(REL::Module::get().version().string("."));
				if (g_host.Running())
				if (auto* messaging = SKSE::GetMessagingInterface())
					messaging->RegisterListener(
						nullptr, OnInterfaceMessage);
			}

			if (!g_host.Running())
				return;
			if (a_message->type == SKSE::MessagingInterface::kPreLoadGame ||
				a_message->type == SKSE::MessagingInterface::kNewGame)
				g_host.ReleaseInputForLifecycle(
					a_message->type ==
							SKSE::MessagingInterface::kPreLoadGame ?
						"preLoadGame" :
						"newGame");

			dvb::skyrimse::Lifecycle::OnSKSEMessage(
				a_message->type, a_message->dataLen, a_message->data);
			if (a_message->type == SKSE::MessagingInterface::kInputLoaded)
			{
				g_host.SetInputReady(true);
				dvb::skyrimse::ui::FrontendsInputLoaded();
			}
			if (a_message->type == SKSE::MessagingInterface::kDataLoaded)
				dvb::skyrimse::ui::FrontendsDataLoaded();
			if (a_message->type ==
				SKSE::MessagingInterface::kPostLoadGame)
				dvb::skyrimse::ui::FrontendsPostLoadGame();
		}
		catch (const std::exception& a_error)
		{
			LogCallbackError("SKSE lifecycle message failed", a_error.what());
		}
		catch (...)
		{
			LogCallbackError("SKSE lifecycle message failed");
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	try
	{
		SKSE::Init(a_skse);
		InitLogging();
		SKSE::AllocTrampoline(1 << 10);
		const auto runtimeVersion =
			REL::Module::get().version().string(".");
		dvb::InitializeRuntimeContext(dvb::MakeRuntimeContext(
			dvb::skyrimse::CurrentProfile(), runtimeVersion,
			reinterpret_cast<const void*>(&SKSEPlugin_Load),
			&dvb::skyrimse::CurrentFrame,
			[](std::function<void()> a_task) {
				const auto tasks = SKSE::GetTaskInterface();
				if (!tasks)
					return false;
				tasks->AddTask(std::move(a_task));
				return true;
			}));
		logs::info("devbench {} loaded", DEVBENCH_VERSION_STRING);
		if (auto* messaging = SKSE::GetMessagingInterface())
			return messaging->RegisterListener(OnMessage);
		return false;
	}
	catch (const std::exception& a_error)
	{
		LogCallbackError("plugin load failed", a_error.what());
		return false;
	}
	catch (...)
	{
		LogCallbackError("plugin load failed");
		return false;
	}
}

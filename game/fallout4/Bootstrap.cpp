#include "pch.h"

#include "Adapter.h"
#include "HostApi.h"
#include "Lifecycle.h"
#include "RuntimeContext.h"
#include "Version.h"

namespace
{
	dvb::Host   g_host;
	std::string g_runtimeVersion;

	void LogCallbackError(
		std::string_view a_context, const char* a_detail = nullptr) noexcept
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

	void StartServer() noexcept
	{
		try
		{
			if (!g_host.Start(
					dvb::fallout4::MakeHostAdapter(g_runtimeVersion)))
				REX::INFO(
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

	void F4SEAPI OnInterfaceMessage(
		F4SE::MessagingInterface::Message* a_message) noexcept
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

	void F4SEAPI OnF4SEMessage(
		F4SE::MessagingInterface::Message* a_message) noexcept
	{
		if (!a_message)
			return;
		try
		{
			if (a_message->type == F4SE::MessagingInterface::kPostLoad)
				StartServer();
			if (g_host.Running())
			{
				if (a_message->type ==
						F4SE::MessagingInterface::kInputLoaded ||
					a_message->type ==
						F4SE::MessagingInterface::kGameDataReady)
					g_host.SetInputReady(true);
				if (a_message->type ==
						F4SE::MessagingInterface::kPreLoadGame ||
					a_message->type == F4SE::MessagingInterface::kNewGame)
					g_host.ReleaseInputForLifecycle(
						a_message->type ==
								F4SE::MessagingInterface::kPreLoadGame ?
							"preLoadGame" :
							"newGame");
			}
			dvb::fallout4::Lifecycle::OnF4SEMessage(
				a_message->type, a_message->dataLen, a_message->data);
		}
		catch (const std::exception& a_error)
		{
			LogCallbackError("F4SE lifecycle message failed", a_error.what());
		}
		catch (...)
		{
			LogCallbackError("F4SE lifecycle message failed");
		}
	}

	bool RegisterAllSenders(const F4SE::LoadInterface* a_f4se)
	{
		const auto messaging =
			static_cast<F4SE::Impl::F4SEMessagingInterface*>(
				a_f4se->QueryInterface(F4SE::LoadInterface::kMessaging));
		return messaging &&
		       messaging->RegisterListener(
				   F4SE::GetPluginHandle(), nullptr,
				   reinterpret_cast<void*>(&OnInterfaceMessage));
	}

	bool InitPlugin(const F4SE::LoadInterface* a_f4se)
	{
		F4SE::Init(a_f4se, F4SE::InitInfo{});
		g_runtimeVersion = a_f4se->RuntimeVersion().string(".");
		dvb::InitializeRuntimeContext(dvb::MakeRuntimeContext(
			dvb::Fallout4Profile(), g_runtimeVersion,
			reinterpret_cast<const void*>(&InitPlugin),
			&dvb::fallout4::CurrentFrame,
			[](std::function<void()> a_task) {
				const auto tasks = F4SE::GetTaskInterface();
				if (!tasks)
					return false;
				tasks->AddTask(std::move(a_task));
				return true;
			}));
		REX::INFO("devbench {} loaded", DEVBENCH_VERSION_STRING);

		const auto messaging = F4SE::GetMessagingInterface();
		if (!messaging || !messaging->RegisterListener(&OnF4SEMessage))
		{
			REX::ERROR("F4SE lifecycle listener registration failed");
			return false;
		}
		if (!RegisterAllSenders(a_f4se))
		{
			REX::ERROR(
				"F4SE all-sender listener registration failed; cross-plugin API "
				"unavailable");
			return false;
		}
		return true;
	}
}

F4SE_PLUGIN_QUERY(
	const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
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

#include "WindowFocus.h"

#include "RuntimeContext.h"
#include "platform/ProcessWindow.h"

#include <atomic>

namespace dvb::fallout4::WindowFocus
{
	namespace
	{
		enum class Result : int
		{
			kSkipped,
			kPending,
			kOk,
			kFailed,
		};

		std::atomic<bool>   g_foregroundEnabled{ false };
		std::atomic<bool>   g_requested{ false };
		std::atomic<bool>   g_queued{ false };
		std::atomic<bool>   g_throttlingDisabled{ false };
		std::atomic<Result> g_result{ Result::kSkipped };

		const char* ToString(Result a_result)
		{
			switch (a_result)
			{
				case Result::kSkipped:
					return "skipped";
				case Result::kPending:
					return "pending";
				case Result::kOk:
					return "ok";
				case Result::kFailed:
					return "failed";
			}
			return "unknown";
		}

		void* EngineWindow()
		{
			const auto main = RE::Main::GetSingleton();
			void*      window = main ? reinterpret_cast<void*>(main->hwnd) : nullptr;
			return window ? window : platform::FindVisibleTopLevelWindow();
		}

		void RequestForeground()
		{
			void* const window = EngineWindow();
			if (!platform::IsWindowVisible(window))
			{
				g_queued = false;
				return;
			}
			const auto result = platform::BringToForeground(window);
			g_requested = true;
			g_result = result.ok() ? Result::kOk : Result::kFailed;
			if (result.ok())
				REX::INFO("devbench: foreground request: {}", platform::ToString(result.outcome));
			else
				REX::WARN("devbench: foreground request: {} (error {})",
					platform::ToString(result.outcome), result.error);
		}
	}

	void Configure(const Config& a_config)
	{
		if (a_config.disablePowerThrottling)
		{
			const auto result = platform::DisablePowerThrottling();
			g_throttlingDisabled = result.ok;
			if (result.ok)
				REX::INFO("{}", "devbench: process power throttling disabled");
			else
				REX::WARN("devbench: disabling power throttling failed (error {})", result.error);
		}
		g_foregroundEnabled = a_config.foregroundOnLaunch;
		g_result = a_config.foregroundOnLaunch ? Result::kPending : Result::kSkipped;
	}

	void OnF4SEMessage(std::uint32_t a_type)
	{
		using Messaging = F4SE::MessagingInterface;
		if (a_type != Messaging::kInputLoaded && a_type != Messaging::kGameDataReady &&
			a_type != Messaging::kGameLoaded)
			return;
		if (!g_foregroundEnabled || g_requested || g_queued.exchange(true))
			return;
		const auto& runtime = GetRuntimeContext();
		if (!runtime.enqueueTask || !runtime.enqueueTask(&RequestForeground))
			g_queued = false;
	}

	json Snapshot()
	{
		return json{
			{ "foreground", platform::IsForegroundOwnedByCurrentProcess() },
			{ "foregroundRequested", g_requested.load() },
			{ "foregroundResult", ToString(g_result.load()) },
			{ "powerThrottlingDisabled", g_throttlingDisabled.load() },
		};
	}
}

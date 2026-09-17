#include "Autorun.h"

#include "Json.h"
#include "ToolActionWorker.h"
#include "ToolRegistry.h"

#include <atomic>
#include <utility>

namespace dvb::skyrimse::ui
{
	namespace
	{
		ToolRegistry*     g_registry = nullptr;
		std::string       g_path;
		bool              g_restore = true;
		std::atomic<bool> g_fired{ false };
	}

	void ArmAutoRun(ToolRegistry& a_registry, std::string a_path, bool a_restoreScene)
	{
		if (a_path.empty())
			return;
		g_registry = &a_registry;
		g_path = std::move(a_path);
		g_restore = a_restoreScene;
		g_fired.store(false);
		logs::info("devbench: autorun armed ({}, restoreScene={})", g_path, g_restore);
	}

	void OnPostLoadGame()
	{
		if (!g_registry)
			return;
		bool expected = false;
		if (!g_fired.compare_exchange_strong(expected, true))
			return;  // already fired this session — don't re-trigger on the replay's own reload

		// The lifecycle callback runs on Skyrim's main thread. Queue the action to the owned
		// worker so a tool that marshals through RunAndWait can never deadlock this callback.
		if (!QueueToolAction("record",
				json{ { "action", "replay" }, { "path", g_path },
					{ "restoreScene", g_restore } },
				"autorun"))
			logs::warn("devbench: autorun replay could not be queued");
	}
}

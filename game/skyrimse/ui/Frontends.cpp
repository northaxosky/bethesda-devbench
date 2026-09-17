#include "Frontends.h"

#include "Autorun.h"
#include "Config.h"
#include "InputHotkeys.h"
#include "RecordingsMenu.h"
#include "ToolActionWorker.h"

#include <mutex>
#include <optional>

namespace dvb::skyrimse::ui
{
	namespace
	{
		std::mutex            g_mutex;
		ToolRegistry*         g_registry = nullptr;
		std::optional<Config> g_config;
	}

	void ConfigureFrontends(ToolRegistry& a_registry, const Config& a_config)
	{
		const std::lock_guard lock{ g_mutex };
		g_registry = std::addressof(a_registry);
		g_config = a_config;
		StartToolActionWorker(a_registry);
		ArmAutoRun(a_registry, a_config.autoRunPath, a_config.autoRunRestoreScene);
	}

	void FrontendsInputLoaded()
	{
		const std::lock_guard lock{ g_mutex };
		if (g_registry && g_config)
			InstallInputHotkeys(*g_registry, *g_config);
	}

	void FrontendsDataLoaded()
	{
		Register();
		RegisterFuck();
	}

	void FrontendsPostLoadGame()
	{
		OnPostLoadGame();
	}

	void ShutdownFrontends() noexcept
	{
		ShutdownInputHotkeys();
		ShutdownToolActionWorker();
		const std::lock_guard lock{ g_mutex };
		g_registry = nullptr;
		g_config.reset();
	}
}

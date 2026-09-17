#pragma once

namespace dvb
{
	class ToolRegistry;
	struct Config;
}

namespace dvb::skyrimse::ui
{
	// Bootstrap-facing lifecycle hooks. Configuration is retained only for the interval between
	// post-load setup and kInputLoaded; framework menus remain optional at runtime.
	void ConfigureFrontends(ToolRegistry& a_registry, const Config& a_config);
	void FrontendsInputLoaded();
	void FrontendsDataLoaded();
	void FrontendsPostLoadGame();
	void ShutdownFrontends() noexcept;
}

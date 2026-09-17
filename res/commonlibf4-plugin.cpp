#include <F4SE/F4SE.h>

F4SE_PLUGIN_VERSION = []() noexcept {
	F4SE::PluginVersionData value{};
	value.PluginVersion({
		PLUGIN_VERSION_MAJOR,
		PLUGIN_VERSION_MINOR,
		PLUGIN_VERSION_PATCH,
		0,
	});
	value.PluginName(PLUGIN_NAME);
	value.AuthorName("Kuz");
	value.UsesAddressLibrary(true);
	value.UsesAddressLibraryNG(true);
	value.UsesSigScanning(false);
	value.HasNoStructUse(false);
	value.CompatibleVersions({ F4SE::RUNTIME_1_11_240 });
	return value;
}();

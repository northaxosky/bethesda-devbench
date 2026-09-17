#include <SKSE/SKSE.h>

SKSEPluginInfo(
	.Version = {
		PLUGIN_VERSION_MAJOR,
		PLUGIN_VERSION_MINOR,
		PLUGIN_VERSION_PATCH,
		0,
	},
	.Name = PLUGIN_NAME,
	.Author = "Kuz",
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary
)

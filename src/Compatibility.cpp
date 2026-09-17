#include "Compatibility.h"

#ifndef PLUGIN_VERSION
#	error "PLUGIN_VERSION must be supplied by the build"
#endif

namespace dvb
{
	std::string_view PackageVersion()
	{
		return PLUGIN_VERSION;
	}

	json CompatibilityMetadata()
	{
		return json{
			{ "packageVersion", PackageVersion() },
			{ "hostApi", json{ { "implementedCompatibilityFloor", kHostApiCompatibilityFloor } } },
			{ "upstreamBaseline", json{
									  { "version", kUpstreamBaselineVersion },
									  { "commit", kUpstreamBaselineCommit },
								  } },
		};
	}
}

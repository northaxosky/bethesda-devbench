#pragma once

#include "DevBenchAPIVersion.h"
#include "Json.h"

#include <string_view>

namespace dvb
{
	inline constexpr unsigned int kHostApiCompatibilityFloor =
		DevBenchAPI::kImplementedHostApiCompatibility;
	inline constexpr std::string_view kUpstreamBaselineVersion = "1.18.2";
	inline constexpr std::string_view kUpstreamBaselineCommit =
		"726fa8691db22a9e2f42c15406cf4bc504815b36";

	std::string_view PackageVersion();
	json             CompatibilityMetadata();
}

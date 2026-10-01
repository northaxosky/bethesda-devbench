#pragma once

#include <string_view>

namespace dvb::fallout4
{
	// The only executable these native bindings were validated against.
	inline constexpr REL::Version kSupportedRuntime{ 1, 11, 240, 0 };

	// Reads the loaded executable once; logs a single mismatch diagnostic.
	bool IsSupportedRuntime() noexcept;

	// Throws ToolError(503) naming a_feature when the runtime is unsupported.
	void RequireSupportedRuntime(std::string_view a_feature);
}

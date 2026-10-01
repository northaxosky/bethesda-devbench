#include "Runtime.h"

#include "RuntimeContext.h"
#include "ToolRegistry.h"

namespace dvb::fallout4
{
	bool IsSupportedRuntime() noexcept
	{
		static const bool supported = []() noexcept {
			try
			{
				const auto runtime =
					REX::FModule::GetExecutingModule().GetFileVersion();
				if (REX::FModule::IsRuntimeAE() && runtime == kSupportedRuntime)
					return true;
				REX::ERROR(
					"devbench: native Fallout 4 bindings disabled for runtime {}; only "
					"AE {} is supported",
					runtime.string("."), kSupportedRuntime.string("."));
			}
			catch (const std::exception& a_error)
			{
				REX::ERROR(
					"devbench: Fallout 4 runtime validation failed: {}", a_error.what());
			}
			catch (...)
			{
				REX::ERROR("{}", "devbench: Fallout 4 runtime validation failed");
			}
			return false;
		}();
		return supported;
	}

	void RequireSupportedRuntime(std::string_view a_feature)
	{
		if (!IsSupportedRuntime())
			throw ToolError(503,
				std::format(
					"{} unavailable: requires Fallout 4 AE {}, current runtime is {}",
					a_feature, kSupportedRuntime.string("."),
					GetRuntimeContext().runtimeVersion));
	}
}

#include "BackgroundRun.h"

namespace dvb::fallout4::BackgroundRun
{
	namespace
	{
		RE::Setting* Find()
		{
			return RE::GetINISetting("bAlwaysActive:General");
		}
	}

	std::optional<bool> AlwaysActive()
	{
		if (const auto* setting = Find())
			return setting->GetBinary();
		return std::nullopt;
	}

	bool SetAlwaysActive(bool a_enabled)
	{
		auto* setting = Find();
		if (setting)
			setting->SetBinary(a_enabled);
		return setting != nullptr;
	}
}

#include "BackgroundRun.h"

namespace dvb::skyrimse::BackgroundRun
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
			return setting->GetBool();
		return std::nullopt;
	}

	bool SetAlwaysActive(bool a_enabled)
	{
		auto* setting = Find();
		if (setting)
			setting->data.b = a_enabled;
		return setting != nullptr;
	}
}

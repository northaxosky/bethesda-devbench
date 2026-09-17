#pragma once

#include "Json.h"

#include <string_view>

namespace RE
{
	class TESForm;
}

namespace dvb::skyrimse::inspection
{
	// Main-thread-only. The returned form is engine-owned and must not escape the
	// current game-thread task without a handle.
	RE::TESForm* ResolveForm(std::string_view a_identifier);

	json SerializeForm(const RE::TESForm* a_form);
}

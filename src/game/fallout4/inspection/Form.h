#pragma once

#include "Json.h"

#include <string_view>

namespace RE
{
	class TESForm;
}

namespace dvb::fallout4::inspection
{
	// Main-thread-only. ResolveForm returns an engine-owned pointer for immediate use
	// inside the current game-thread task; callers must serialize or acquire a handle
	// before leaving that task.
	RE::TESForm* ResolveForm(std::string_view a_identifier);

	// Canonical Fallout 4 form projection shared by inspection and future Papyrus
	// marshalling. Keeps numeric formId for the existing local contract and adds the
	// hexadecimal/type/name/editor identity used by the wider tool surface.
	json SerializeForm(const RE::TESForm* a_form);
}

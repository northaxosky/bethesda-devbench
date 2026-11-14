#pragma once

#include "tools/inspection/InspectionQuery.h"

namespace dvb::fallout4::inspection
{
	// Shared by the readers; main thread only.
	void               RequireStableWorld(std::string_view a_kind);
	RE::TESObjectREFR* ResolveReferenceTarget(
		std::string_view a_identifier, RE::TESObjectREFR* a_default, std::string_view a_kind);

	json ReadVm(const tools::inspection::Request& a_request);
	json ReadInventory(const tools::inspection::Request& a_request);
	json ReadQuests(const tools::inspection::Request& a_request);
	json ReadEffects(const tools::inspection::Request& a_request);
	json ReadReferences(const tools::inspection::Request& a_request);
	json ReadRegistrants(const tools::inspection::Request& a_request);
	json ReadLights(const tools::inspection::Request& a_request);
}

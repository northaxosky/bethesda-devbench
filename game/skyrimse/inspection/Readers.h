#pragma once

#include "tools/inspection/InspectionQuery.h"

namespace dvb::skyrimse::inspection
{
	json ReadVm(const tools::inspection::Request& a_request);
	json ReadInventory(const tools::inspection::Request& a_request);
	json ReadQuests(const tools::inspection::Request& a_request);
	json ReadEffects(const tools::inspection::Request& a_request);
	json ReadReferences(const tools::inspection::Request& a_request);
	json ReadRegistrants(const tools::inspection::Request& a_request);
}

#pragma once

#include "Config.h"
#include "Json.h"

#include <functional>

namespace dvb
{
	Config ParseConfig(const json& a_document);
	json   ConfigDefaults();

	struct ConfigLoadResult
	{
		Config      config;
		std::string backfillWarning;
	};

	ConfigLoadResult LoadConfigDocument(
		json a_document, bool a_exists, const std::function<void(const json&)>& a_write);
}

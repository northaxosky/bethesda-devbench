#pragma once

#include "Config.h"
#include "Json.h"

#include <functional>

namespace dvb
{
	Config ParseConfig(const json& a_document);
	Config ParseConfig(const json& a_document, Config a_defaults);
	json   ConfigDefaults();
	json   ConfigDefaults(const Config& a_defaults);

	struct ConfigLoadResult
	{
		Config      config;
		std::string backfillWarning;
	};

	ConfigLoadResult LoadConfigDocument(
		json a_document, bool a_exists, const std::function<void(const json&)>& a_write);
	ConfigLoadResult LoadConfigDocument(
		json a_document, bool a_exists, Config a_defaults,
		const std::function<void(const json&)>& a_write);
}

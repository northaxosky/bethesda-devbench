#include "ConfigPolicy.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace
{
	[[noreturn]] void Invalid(std::string_view a_key, std::string_view a_expected)
	{
		throw std::invalid_argument("config '" + std::string(a_key) + "' must be " + std::string(a_expected));
	}

	void Read(const dvb::json& a_document, const char* a_key, bool& a_value)
	{
		const auto it = a_document.find(a_key);
		if (it == a_document.end())
			return;
		if (!it->is_boolean())
			Invalid(a_key, "a boolean");
		a_value = it->get<bool>();
	}

	void Read(const dvb::json& a_document, const char* a_key, std::string& a_value)
	{
		const auto it = a_document.find(a_key);
		if (it == a_document.end())
			return;
		if (!it->is_string())
			Invalid(a_key, "a string");
		a_value = it->get<std::string>();
	}

	void Read(const dvb::json& a_document, const char* a_key, int& a_value,
		int a_min = 0, int a_max = std::numeric_limits<int>::max())
	{
		const auto it = a_document.find(a_key);
		if (it == a_document.end())
			return;
		if (!it->is_number_integer())
			Invalid(a_key, "an integer");
		if (it->is_number_unsigned() && it->get<std::uint64_t>() > static_cast<std::uint64_t>(a_max))
			Invalid(a_key, "within [" + std::to_string(a_min) + ", " + std::to_string(a_max) + "]");
		const auto number = it->get<std::int64_t>();
		if (number < a_min || number > a_max)
			Invalid(a_key, "within [" + std::to_string(a_min) + ", " + std::to_string(a_max) + "]");
		a_value = static_cast<int>(number);
	}
}

namespace dvb
{
	Config ParseConfig(const json& a_document)
	{
		return ParseConfig(a_document, Config{});
	}

	Config ParseConfig(const json& a_document, Config a_defaults)
	{
		if (!a_document.is_object())
			throw std::invalid_argument("config must be a JSON object");

		Config config = std::move(a_defaults);
		Read(a_document, "enabled", config.enabled);
		Read(a_document, "allowConsoleCommands", config.allowConsoleCommands);
		Read(a_document, "allowGameActions", config.allowGameActions);
		Read(a_document, "allowControlActions", config.allowControlActions);
		Read(a_document, "allowPapyrusCalls", config.allowPapyrusCalls);
		Read(a_document, "port", config.port, 1, 65535);
		Read(a_document, "logLevel", config.logLevel);
		constexpr std::string_view kLevels[] = { "trace", "debug", "info", "warn", "error", "critical", "off" };
		if (std::ranges::find(kLevels, config.logLevel) == std::end(kLevels))
			Invalid("logLevel", "trace, debug, info, warn, error, critical, or off");

		Read(a_document, "recordHotkey", config.recordHotkey);
		Read(a_document, "replayHotkey", config.replayHotkey);
		Read(a_document, "recordHotkeyShift", config.recordHotkeyShift);
		Read(a_document, "replayHotkeyShift", config.replayHotkeyShift);
		Read(a_document, "replayPath", config.replayPath);
		Read(a_document, "replayRestoreScene", config.replayRestoreScene);
		Read(a_document, "recordIntervalMs", config.recordIntervalMs, 10);
		Read(a_document, "autoRunPath", config.autoRunPath);
		Read(a_document, "autoRunRestoreScene", config.autoRunRestoreScene);
		Read(a_document, "loadSettleMs", config.loadSettleMs);
		Read(a_document, "couplingAnchorMs", config.couplingAnchorMs);
		Read(a_document, "couplingCellMs", config.couplingCellMs);
		Read(a_document, "cleanTransition", config.cleanTransition);
		Read(a_document, "cleanTransitionCell", config.cleanTransitionCell);
		Read(a_document, "captureDir", config.captureDir);
		Read(a_document, "captureTimeoutMs", config.captureTimeoutMs, 1);
		Read(a_document, "captureSettleMs", config.captureSettleMs);
		Read(a_document, "stallWatchdogMs", config.stallWatchdogMs);

		if (const auto it = a_document.find("captureScanDirs"); it != a_document.end())
		{
			if (!it->is_array() || !std::ranges::all_of(*it, [](const json& a_path) { return a_path.is_string(); }))
				Invalid("captureScanDirs", "an array of strings");
			config.captureScanDirs = it->get<std::vector<std::string>>();
		}
		return config;
	}

	json ConfigDefaults()
	{
		return ConfigDefaults(Config{});
	}

	json ConfigDefaults(const Config& config)
	{
		return json{
			{ "enabled", config.enabled },
			{ "allowConsoleCommands", config.allowConsoleCommands },
			{ "allowGameActions", config.allowGameActions },
			{ "allowControlActions", config.allowControlActions },
			{ "allowPapyrusCalls", config.allowPapyrusCalls },
			{ "port", config.port },
			{ "logLevel", config.logLevel },
			{ "recordHotkey", config.recordHotkey },
			{ "replayHotkey", config.replayHotkey },
			{ "recordHotkeyShift", config.recordHotkeyShift },
			{ "replayHotkeyShift", config.replayHotkeyShift },
			{ "replayPath", config.replayPath },
			{ "replayRestoreScene", config.replayRestoreScene },
			{ "recordIntervalMs", config.recordIntervalMs },
			{ "autoRunPath", config.autoRunPath },
			{ "autoRunRestoreScene", config.autoRunRestoreScene },
			{ "loadSettleMs", config.loadSettleMs },
			{ "couplingAnchorMs", config.couplingAnchorMs },
			{ "couplingCellMs", config.couplingCellMs },
			{ "cleanTransition", config.cleanTransition },
			{ "cleanTransitionCell", config.cleanTransitionCell },
			{ "captureDir", config.captureDir },
			{ "captureScanDirs", config.captureScanDirs },
			{ "captureTimeoutMs", config.captureTimeoutMs },
			{ "captureSettleMs", config.captureSettleMs },
			{ "stallWatchdogMs", config.stallWatchdogMs },
		};
	}

	ConfigLoadResult LoadConfigDocument(
		json a_document, bool a_exists, const std::function<void(const json&)>& a_write)
	{
		return LoadConfigDocument(
			std::move(a_document), a_exists, Config{}, a_write);
	}

	ConfigLoadResult LoadConfigDocument(
		json a_document, bool a_exists, Config a_defaults,
		const std::function<void(const json&)>& a_write)
	{
		ConfigLoadResult result{
			ParseConfig(a_document, a_defaults), {}
		};
		bool             changed = !a_exists;
		const auto       defaults = ConfigDefaults(a_defaults);
		for (const auto& [key, value] : defaults.items())
		{
			if (!a_document.contains(key))
			{
				a_document[key] = value;
				changed = true;
			}
		}
		if (!changed)
			return result;

		if (!a_exists)
		{
			a_write(a_document);
			return result;
		}

		// Backfilling defaults is optional once the existing document has passed validation.
		try
		{
			a_write(a_document);
		}
		catch (const std::system_error& a_error)
		{
			result.backfillWarning = a_error.what();
		}
		return result;
	}
}

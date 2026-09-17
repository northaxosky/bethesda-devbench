#include "Config.h"

#include "ConfigPolicy.h"
#include "Log.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace
{
	std::mutex                  g_configMutex;

	std::filesystem::path ConfigPath(const dvb::GameProfile& a_profile)
	{
		return a_profile.pluginDataDirectory / "config.json";
	}

	dvb::json ReadDocument(const std::filesystem::path& a_path)
	{
		std::ifstream in(a_path);
		if (!in)
			throw std::runtime_error("cannot read " + a_path.string());
		return dvb::json::parse(in, nullptr, true, true);
	}

	void WriteDocument(
		const std::filesystem::path& a_path, const dvb::json& a_document)
	{
		std::filesystem::create_directories(a_path.parent_path());
		const auto temporary = std::filesystem::path(a_path.wstring() + L".tmp");
		{
			std::ofstream out;
			out.exceptions(std::ios::failbit | std::ios::badbit);
			out.open(temporary, std::ios::trunc);
			out << a_document.dump(2) << '\n';
			out.close();
		}
		// Windows filesystem::rename does not replace an existing destination.
		if (!::MoveFileExW(
				temporary.c_str(), a_path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "cannot replace config.json");
	}
}

namespace dvb
{
	Config DefaultConfig(const GameProfile& a_profile)
	{
		Config config;
		config.port = a_profile.defaultPort;
		config.captureDir = a_profile.captureDirectory.generic_string();
		config.captureScanDirs.clear();
		for (const auto& path : a_profile.captureScanDirectories)
			config.captureScanDirs.push_back(path.generic_string());
		return config;
	}

	void SaveHotkeys(const GameProfile& a_profile, int a_recordKey, bool a_recordShift,
		int a_replayKey, bool a_replayShift)
	{
		std::lock_guard lock(g_configMutex);
		const auto      path = ConfigPath(a_profile);
		const auto      defaults = DefaultConfig(a_profile);
		auto            document = std::filesystem::exists(path) ?
		                               ReadDocument(path) :
		                               ConfigDefaults(defaults);
		ParseConfig(document, defaults);
		document["recordHotkey"] = a_recordKey;
		document["recordHotkeyShift"] = a_recordShift;
		document["replayHotkey"] = a_replayKey;
		document["replayHotkeyShift"] = a_replayShift;
		ParseConfig(document, defaults);
		WriteDocument(path, document);
	}

	Config LoadConfig(const GameProfile& a_profile)
	{
		std::lock_guard lock(g_configMutex);
		const auto      path = ConfigPath(a_profile);
		const auto      defaults = DefaultConfig(a_profile);
		const bool      exists = std::filesystem::exists(path);
		auto            document = exists ? ReadDocument(path) : ConfigDefaults(defaults);
		const auto      result = LoadConfigDocument(
			std::move(document), exists, defaults,
			[&](const json& a_document) { WriteDocument(path, a_document); });
		const auto&     config = result.config;
		if (!result.backfillWarning.empty())
			logs::warn(
				"devbench: using validated config; could not backfill default keys: {}",
				result.backfillWarning);

		logs::info(
			"devbench: config enabled={} port={} logLevel={} allowConsoleCommands={} allowGameActions={} "
			"allowControlActions={} allowPapyrusCalls={}",
			config.enabled, config.port, config.logLevel, config.allowConsoleCommands,
			config.allowGameActions, config.allowControlActions, config.allowPapyrusCalls);
		return config;
	}
}

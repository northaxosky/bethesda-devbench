#include "Config.h"

#include "ConfigPolicy.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace
{
	const std::filesystem::path kConfigPath = LR"(Data\F4SE\Plugins\devbench\config.json)";
	std::mutex                  g_configMutex;

	dvb::json ReadDocument()
	{
		std::ifstream in(kConfigPath);
		if (!in)
			throw std::runtime_error("cannot read " + kConfigPath.string());
		return dvb::json::parse(in, nullptr, true, true);
	}

	void WriteDocument(const dvb::json& a_document)
	{
		std::filesystem::create_directories(kConfigPath.parent_path());
		const auto temporary = std::filesystem::path(kConfigPath.wstring() + L".tmp");
		{
			std::ofstream out;
			out.exceptions(std::ios::failbit | std::ios::badbit);
			out.open(temporary, std::ios::trunc);
			out << a_document.dump(2) << '\n';
			out.close();
		}
		// Windows filesystem::rename does not replace an existing destination.
		if (!::MoveFileExW(temporary.c_str(), kConfigPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "cannot replace config.json");
	}
}

namespace dvb
{
	void SaveHotkeys(int a_recordKey, bool a_recordShift, int a_replayKey, bool a_replayShift)
	{
		std::lock_guard lock(g_configMutex);
		auto            document = std::filesystem::exists(kConfigPath) ? ReadDocument() : ConfigDefaults();
		ParseConfig(document);
		document["recordHotkey"] = a_recordKey;
		document["recordHotkeyShift"] = a_recordShift;
		document["replayHotkey"] = a_replayKey;
		document["replayHotkeyShift"] = a_replayShift;
		ParseConfig(document);
		WriteDocument(document);
	}

	Config LoadConfig()
	{
		std::lock_guard lock(g_configMutex);
		const bool      exists = std::filesystem::exists(kConfigPath);
		auto            document = exists ? ReadDocument() : ConfigDefaults();
		const auto      result = LoadConfigDocument(std::move(document), exists, WriteDocument);
		const auto&     config = result.config;
		if (!result.backfillWarning.empty())
			REX::WARN("devbench: using validated config; could not backfill default keys: {}", result.backfillWarning);

		REX::INFO(
			"devbench: config enabled={} port={} logLevel={} allowConsoleCommands={} allowGameActions={} "
			"allowControlActions={} allowPapyrusCalls={}",
			config.enabled, config.port, config.logLevel, config.allowConsoleCommands,
			config.allowGameActions, config.allowControlActions, config.allowPapyrusCalls);
		return config;
	}
}

#include "test_framework.h"

#include "ConfigPolicy.h"

#include <cstdint>
#include <limits>
#include <system_error>

using dvb::json;

TEST_CASE("console execution requires an explicit config opt-in")
{
	CHECK(dvb::ParseConfig(json::object()).enabled);
	CHECK(!dvb::ParseConfig(json::object()).allowConsoleCommands);
	CHECK(!dvb::ParseConfig(json::object()).allowGameActions);
	CHECK(!dvb::ParseConfig(json::object()).allowControlActions);
	CHECK(!dvb::ParseConfig(json::object()).allowPapyrusCalls);
	CHECK(!dvb::ConfigDefaults().at("allowConsoleCommands").get<bool>());
	CHECK(!dvb::ConfigDefaults().at("allowGameActions").get<bool>());
	CHECK(!dvb::ConfigDefaults().at("allowControlActions").get<bool>());
	CHECK(!dvb::ConfigDefaults().at("allowPapyrusCalls").get<bool>());
	CHECK(dvb::ParseConfig(json{ { "allowConsoleCommands", true } }).allowConsoleCommands);
	CHECK(!dvb::ParseConfig(json{ { "allowConsoleCommands", false } }).allowConsoleCommands);
	CHECK(dvb::ParseConfig(json{ { "allowGameActions", true } }).allowGameActions);
	CHECK(dvb::ParseConfig(json{ { "allowControlActions", true } }).allowControlActions);
	CHECK(dvb::ParseConfig(json{ { "allowPapyrusCalls", true } }).allowPapyrusCalls);
	CHECK(!dvb::ParseConfig(json{ { "allowConsoleCommands", true } }).allowGameActions);
	CHECK(!dvb::ParseConfig(json{ { "allowGameActions", true } }).allowConsoleCommands);
	CHECK(!dvb::ParseConfig(json{ { "allowControlActions", true } }).allowPapyrusCalls);
	CHECK(!dvb::ParseConfig(json{ { "allowPapyrusCalls", true } }).allowControlActions);
}

TEST_CASE("malformed config does not silently enable the server or console")
{
	for (const auto& value : { json(nullptr), json::array(), json(true), json("config") })
		CHECK_THROWS(dvb::ParseConfig(value));
	for (const auto& value : { json(nullptr), json(1), json("true"), json::array() })
	{
		CHECK_THROWS(dvb::ParseConfig(json{ { "enabled", value } }));
		CHECK_THROWS(dvb::ParseConfig(json{ { "allowConsoleCommands", value } }));
		CHECK_THROWS(dvb::ParseConfig(json{ { "allowGameActions", value } }));
		CHECK_THROWS(dvb::ParseConfig(json{ { "allowControlActions", value } }));
		CHECK_THROWS(dvb::ParseConfig(json{ { "allowPapyrusCalls", value } }));
	}
	CHECK_THROWS(json::parse("{"));
}

TEST_CASE("config validates numeric ranges before narrowing")
{
	CHECK(dvb::ParseConfig(json{ { "port", 1 } }).port == 1);
	CHECK(dvb::ParseConfig(json{ { "port", 65535 } }).port == 65535);
	for (const auto& value : { json(0), json(-1), json(65536), json(8930.0), json("8930"),
			 json(std::numeric_limits<std::uint64_t>::max()), json(nullptr) })
		CHECK_THROWS(dvb::ParseConfig(json{ { "port", value } }));
	CHECK_THROWS(dvb::ParseConfig(json{ { "recordIntervalMs", 9 } }));
	CHECK_THROWS(dvb::ParseConfig(json{ { "loadSettleMs", -1 } }));
	CHECK_THROWS(dvb::ParseConfig(json{ { "captureTimeoutMs", 0 } }));
}

TEST_CASE("config validates logging and string-array types")
{
	CHECK(dvb::ParseConfig(json{ { "logLevel", "debug" } }).logLevel == "debug");
	CHECK_THROWS(dvb::ParseConfig(json{ { "logLevel", "verbose" } }));
	CHECK_THROWS(dvb::ParseConfig(json{ { "logLevel", false } }));
	CHECK_THROWS(dvb::ParseConfig(json{ { "captureScanDirs", "Screenshots" } }));
	CHECK_THROWS(dvb::ParseConfig(json{ { "captureScanDirs", json::array({ "Screenshots", 42 }) } }));
	CHECK(dvb::ParseConfig(json{ { "captureScanDirs", json::array({ "", "Screenshots" }) } }).captureScanDirs.size() == 2);
}

TEST_CASE("defaults and existing read-only configs round-trip without enabling console")
{
	const auto defaults = dvb::ConfigDefaults();
	const auto parsed = dvb::ParseConfig(defaults);
	CHECK(parsed.port == 8930);
	CHECK(!parsed.allowConsoleCommands);
	CHECK(parsed.recordIntervalMs == 10);

	auto existing = defaults;
	existing.erase("allowConsoleCommands");
	existing.erase("allowGameActions");
	existing.erase("allowControlActions");
	existing.erase("allowPapyrusCalls");
	existing["enabled"] = false;
	existing["port"] = 8932;
	const auto migrated = dvb::ParseConfig(existing);
	CHECK(!migrated.enabled);
	CHECK(!migrated.allowConsoleCommands);
	CHECK(!migrated.allowGameActions);
	CHECK(!migrated.allowControlActions);
	CHECK(!migrated.allowPapyrusCalls);
	CHECK(migrated.port == 8932);
}

TEST_CASE("an unwritable valid existing config preserves its parsed permissions")
{
	const auto denied = [](const json&) {
		throw std::system_error(std::make_error_code(std::errc::permission_denied));
	};
	const auto readOnly = dvb::LoadConfigDocument(json{ { "enabled", false } }, true, denied);
	CHECK(!readOnly.config.enabled);
	CHECK(!readOnly.config.allowConsoleCommands);
	CHECK(!readOnly.backfillWarning.empty());
	const auto optedIn = dvb::LoadConfigDocument(json{ { "allowConsoleCommands", true } }, true, denied);
	CHECK(optedIn.config.allowConsoleCommands);
	CHECK(!optedIn.backfillWarning.empty());
	CHECK_THROWS(dvb::LoadConfigDocument(dvb::ConfigDefaults(), false, denied));
}

TEST_CASE("config backfill preserves existing and extension keys and skips complete files")
{
	json       written;
	const auto record = [&](const json& a_document) { written = a_document; };
	const auto result = dvb::LoadConfigDocument(
		json{ { "port", 9000 }, { "enabled", false }, { "futureSetting", "keep" } }, true, record);
	CHECK(result.backfillWarning.empty());
	CHECK(written.at("port") == 9000);
	CHECK(written.at("enabled") == false);
	CHECK(written.at("allowConsoleCommands") == false);
	CHECK(written.at("allowGameActions") == false);
	CHECK(written.at("allowControlActions") == false);
	CHECK(written.at("allowPapyrusCalls") == false);
	CHECK(written.at("futureSetting") == "keep");

	written = nullptr;
	dvb::LoadConfigDocument(dvb::ConfigDefaults(), true, record);
	CHECK(written.is_null());
	CHECK_THROWS(dvb::LoadConfigDocument(json{ { "allowConsoleCommands", "true" } }, true, record));
	CHECK(written.is_null());
}

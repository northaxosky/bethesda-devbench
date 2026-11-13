#include "test_framework.h"

#include "Compatibility.h"
#include "ToolRegistry.h"
#include "tools/BridgeSetupTool.h"
#include "tools/PingTool.h"
#include "tools/ToolPermissions.h"

using dvb::json;
using dvb::ToolContext;
using dvb::ToolError;
using dvb::ToolRegistry;

TEST_CASE("host API compatibility remains independent from the package version")
{
	const auto metadata = dvb::CompatibilityMetadata();
	CHECK(metadata.at("packageVersion") == "0.1.0");
	CHECK(metadata.at("hostApi").at("implementedCompatibilityFloor") >= 10400);
	CHECK(metadata.at("hostApi").at("implementedCompatibilityFloor") >= 10500);
	CHECK(metadata.at("hostApi").at("implementedCompatibilityFloor") != 100);
	CHECK(metadata.at("upstreamBaseline").at("version") == "1.18.2");
	CHECK(metadata.at("upstreamBaseline").at("commit") ==
		  "726fa8691db22a9e2f42c15406cf4bc504815b36");
}

TEST_CASE("future control and Papyrus permissions fail closed with actionable errors")
{
	CHECK_NOTHROW(dvb::tools::RequireToolPermission(
		true, dvb::tools::ToolPermission::kControlActions));

	for (const auto [permission, setting] : {
			 std::pair{ dvb::tools::ToolPermission::kControlActions, "allowControlActions" },
			 std::pair{ dvb::tools::ToolPermission::kPapyrusCalls, "allowPapyrusCalls" },
		 })
	{
		try
		{
			dvb::tools::RequireToolPermission(false, permission);
			CHECK(false);
		}
		catch (const ToolError& error)
		{
			CHECK(error.code == 403);
			CHECK(std::string(error.what()).find(setting) != std::string::npos);
		}
	}
}

TEST_CASE("current ping descriptor preserves the public self-test contract")
{
	const auto descriptor = dvb::tools::BuildPingDescriptor();
	CHECK(descriptor.name == "ping");
	CHECK(descriptor.readOnly);
	CHECK(descriptor.inputSchema == dvb::DefaultInputSchema());
	CHECK(descriptor.description.find("C-ABI self-test") != std::string::npos);
}

TEST_CASE("bridge setup registration exposes its factory schema and backend response")
{
	const json expected{
		{ "available", false },
		{ "exePath", R"(C:\physical\devbench-bridge.exe)" },
		{ "args", json::array({ "--game", "fo4" }) },
	};
	int          calls = 0;
	ToolRegistry registry;
	dvb::tools::RegisterBridgeSetupTool(registry, [&] {
		++calls;
		return expected;
	});

	const auto registered = registry.Describe("mcp_bridge_setup");
	const auto factory = dvb::tools::BuildBridgeSetupDescriptor();
	CHECK(registered.has_value());
	CHECK(registered->description == factory.description);
	CHECK(registered->inputSchema == factory.inputSchema);
	CHECK(registered->readOnly == factory.readOnly);

	const auto result =
		registry.Invoke("mcp_bridge_setup", json::object(), ToolContext{});
	CHECK(result.ok);
	CHECK(result.value == expected);
	CHECK(calls == 1);
	CHECK(registry.Invoke("mcp_bridge_setup", json::array(), ToolContext{}).errorCode == 400);
	CHECK(registry.Invoke(
					  "mcp_bridge_setup", json{ { "install", true } }, ToolContext{})
			  .errorCode == 400);
	CHECK(calls == 1);
}

TEST_CASE("bridge setup reports an unavailable backend instead of fabricating discovery")
{
	ToolRegistry registry;
	dvb::tools::RegisterBridgeSetupTool(registry, {});
	const auto result =
		registry.Invoke("mcp_bridge_setup", json::object(), ToolContext{});
	CHECK(!result.ok);
	CHECK(result.errorCode == 503);
}

#include "test_framework.h"

#include "GameProfile.h"
#include "tools/ToolCatalog.h"

using dvb::json;

TEST_CASE("game profiles keep paths ports and save formats isolated")
{
	const auto fallout = dvb::Fallout4Profile();
	const auto skyrim = dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimAE);
	const auto vr = dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimVR);
	const std::vector<std::string> skyrimAliases{ "skyrimse", "sse" };

	CHECK(fallout.id == "fo4");
	CHECK(fallout.executableName == "Fallout4.exe");
	CHECK(fallout.aliases == std::vector<std::string>{ "fallout4" });
	CHECK(fallout.defaultPort == 8930);
	CHECK(fallout.pluginDataDirectory.generic_string() ==
		  "Data/F4SE/Plugins/devbench");
	CHECK(fallout.saveExtension == ".fos");
	CHECK(fallout.coSaveExtension == ".f4se");

	CHECK(skyrim.id == "se");
	CHECK(skyrim.executableName == "SkyrimSE.exe");
	CHECK(skyrim.aliases == skyrimAliases);
	CHECK(skyrim.runtimeVariant == "ae");
	CHECK(skyrim.defaultPort == 8920);
	CHECK(skyrim.pluginDataDirectory.generic_string() ==
		  "Data/SKSE/Plugins/devbench");
	CHECK(skyrim.saveExtension == ".ess");
	CHECK(skyrim.coSaveExtension == ".skse");
	CHECK(!skyrim.vr);

	CHECK(vr.id == "vr");
	CHECK(vr.executableName == "SkyrimVR.exe");
	CHECK(vr.aliases == std::vector<std::string>{ "skyrimvr" });
	CHECK(vr.runtimeVariant == "vr");
	CHECK(vr.defaultPort == 8921);
	CHECK(vr.vr);
}

TEST_CASE("game profiles enforce recording and context-free menu boundaries")
{
	const auto fallout = dvb::Fallout4Profile();
	const auto skyrim = dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimSE);

	CHECK(dvb::IsRecordingGameCompatible(fallout, "Fallout 4"));
	CHECK(!dvb::IsRecordingGameCompatible(fallout, "skyrimse"));
	CHECK(dvb::IsRecordingGameCompatible(skyrim, "skyrim"));
	CHECK(!dvb::IsRecordingGameCompatible(skyrim, "fo4"));
	CHECK(dvb::IsContextFreeMenu(fallout, "PauseMenu"));
	CHECK(!dvb::IsContextFreeMenu(fallout, "Journal Menu"));
	CHECK(dvb::IsContextFreeMenu(skyrim, "Journal Menu"));
}

TEST_CASE("generated catalogs expose adapter identity without changing tool count")
{
	const auto fallout =
		dvb::tools::BuildCoreToolCatalog(dvb::Fallout4Profile());
	const auto vr = dvb::tools::BuildCoreToolCatalog(
		dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimVR));
	const json falloutProfile{
		{ "id", "fo4" },
		{ "displayName", "Fallout 4" },
		{ "extender", "F4SE" },
		{ "executable", "Fallout4.exe" },
		{ "aliases", json::array({ "fallout4" }) },
	};

	CHECK(fallout.at("game") == "fo4");
	CHECK(fallout.at("profile") == falloutProfile);
	CHECK(fallout.at("runtimeVariant") == "ae");
	CHECK(fallout.at("vr") == false);
	CHECK(vr.at("game") == "vr");
	CHECK(vr.at("profile").at("id") == vr.at("game"));
	CHECK(vr.at("profile").at("executable") == "SkyrimVR.exe");
	CHECK(vr.at("runtimeVariant") == "vr");
	CHECK(vr.at("vr") == true);
	CHECK(fallout.at("tools").size() == 15);
	CHECK(vr.at("tools").size() == fallout.at("tools").size());
}

TEST_CASE("offline catalog bundle carries every canonical runtime schema")
{
	const auto bundle = dvb::tools::BuildCoreToolCatalogBundle();

	CHECK(bundle.at("format") == "devbench.core-tools-2");
	const auto& catalogs = bundle.at("catalogs");
	CHECK(catalogs.size() == 3);
	CHECK(catalogs[0].at("format") == "devbench.core-tools-1");
	CHECK(catalogs[0].at("game") == "fo4");
	CHECK(catalogs[1].at("game") == "se");
	CHECK(catalogs[2].at("game") == "vr");
	CHECK(catalogs[0].at("profile").at("id") == catalogs[0].at("game"));
	CHECK(catalogs[1].at("profile").at("id") == catalogs[1].at("game"));
	CHECK(catalogs[2].at("profile").at("id") == catalogs[2].at("game"));
	CHECK(catalogs[0].at("profile").at("executable") == "Fallout4.exe");
	CHECK(catalogs[1].at("profile").at("executable") == "SkyrimSE.exe");
	CHECK(catalogs[2].at("profile").at("executable") == "SkyrimVR.exe");
	CHECK(catalogs[1].at("profile").at("aliases")[0] == "skyrimse");
	CHECK(catalogs[1].at("profile").at("aliases")[1] == "sse");
	CHECK(catalogs[0].at("tools").size() == 15);
	CHECK(catalogs[1].at("tools").size() == 15);
	CHECK(catalogs[2].at("tools").size() == 15);
}

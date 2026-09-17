#include "test_framework.h"

#include "ToolExtensions.h"
#include "tools/ExtensionDescriptorRefresh.h"

TEST_CASE("one extension listener refreshes each bound base without replacing another base")
{
	int        menu = 0;
	int        inspect = 0;
	int        capture = 0;
	const auto handler = [](const dvb::json&, const dvb::ToolContext&) { return dvb::json::object(); };
	{
		dvb::tools::ExtensionDescriptorRefresh refresh({
			{ "refresh.menu", [&] { ++menu; } },
			{ "refresh.inspect", [&] { ++inspect; } },
			{ "refresh.capture", [&] { ++capture; } },
		});
		dvb::ToolExtensions::Register("REFRESH.MENU", "probe", dvb::json::object(), handler);
		dvb::ToolExtensions::Register("refresh.inspect", "probe", dvb::json::object(), handler);
		dvb::ToolExtensions::Register("refresh.capture", "probe", dvb::json::object(), handler);
		dvb::ToolExtensions::Register("refresh.unbound", "probe", dvb::json::object(), handler);
		CHECK(menu == 1);
		CHECK(inspect == 1);
		CHECK(capture == 1);
	}
	dvb::ToolExtensions::Register("refresh.menu", "late", dvb::json::object(), handler);
	CHECK(menu == 1);
}

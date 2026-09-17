#include "test_framework.h"

#include "tools/inspection/InspectionQuery.h"

using dvb::json;
using dvb::tools::inspection::MatchesFormType;
using dvb::tools::inspection::NormalizeFormType;
using dvb::tools::inspection::ParseRequest;
using dvb::tools::inspection::ResultWindow;

TEST_CASE("inspection query validates bounded numeric inputs and mutually exclusive targets")
{
	CHECK(ParseRequest(json::object()).kind == "state");
	CHECK(ParseRequest(json{ { "radius", 12.5 }, { "limit", 0 } }).limit == 0);
	CHECK_THROWS(ParseRequest(json{ { "radius", -1 } }));
	CHECK_THROWS(ParseRequest(json{ { "radius", "near" } }));
	CHECK_THROWS(ParseRequest(json{ { "limit", 501 } }));
	CHECK_THROWS(ParseRequest(json{ { "limit", 1.5 } }));
	CHECK_THROWS(ParseRequest(json{ { "kind", "refs" }, { "selected", true }, { "formId", "0x14" } }));
}

TEST_CASE("inspection form-type filters normalize FO4 record aliases")
{
	CHECK(NormalizeFormType("Actor") == "achr");
	CHECK(NormalizeFormType("CHEM") == "alch");
	CHECK(NormalizeFormType("WEA") == "wea");
	CHECK(MatchesFormType("ACHR", NormalizeFormType("actor")));
	CHECK(MatchesFormType("WEAP", NormalizeFormType("wea")));
	CHECK(!MatchesFormType("ARMO", NormalizeFormType("weapon")));
	CHECK(MatchesFormType("ARMO", ""));
}

TEST_CASE("inspection result windows expose truncation without pretending the count is exact")
{
	ResultWindow window(2);
	CHECK(window.Observe(json{ { "id", 1 } }));
	CHECK(window.Observe(json{ { "id", 2 } }));
	CHECK(!window.Observe(json{ { "id", 3 } }));

	const auto result = window.Build("items");
	CHECK(result.at("count") == 3);
	CHECK(result.at("returned") == 2);
	CHECK(result.at("truncated") == true);
	CHECK(result.at("countExact") == false);
	CHECK(result.at("items").size() == 2);

	ResultWindow complete(2);
	CHECK(complete.Observe(json{ { "id", 1 } }));
	complete.MarkComplete();
	const auto completeResult = complete.Build("refs");
	CHECK(completeResult.at("count") == 1);
	CHECK(completeResult.at("countExact") == true);
	CHECK(completeResult.at("truncated") == false);

	ResultWindow empty(0);
	empty.MarkComplete();
	const auto emptyResult = empty.Build("quests");
	CHECK(emptyResult.at("count") == 0);
	CHECK(emptyResult.at("returned") == 0);
	CHECK(emptyResult.at("countExact") == true);
	CHECK(emptyResult.at("quests").empty());
}

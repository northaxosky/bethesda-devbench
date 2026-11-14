#include "test_framework.h"

#include "tools/inspection/InspectLogic.h"

using namespace dvb::tools::inspection;

TEST_CASE("lights scope accepts ref, scene and empty, and nothing else")
{
	CHECK(ParseLightsScope("") == LightsScope::kRef);
	CHECK(ParseLightsScope("ref") == LightsScope::kRef);
	CHECK(ParseLightsScope("scene") == LightsScope::kScene);
	CHECK(!ParseLightsScope("Scene").has_value());
	CHECK(!ParseLightsScope("world").has_value());
}

TEST_CASE("a traversal budget stops each kind of work at its limit and says which")
{
	TraversalBudget b(2, 3, 1);
	CHECK(b.VisitNode());
	CHECK(b.VisitNode());
	CHECK(!b.VisitNode());
	CHECK(b.Nodes() == 2);
	CHECK(b.Truncated());
	CHECK(b.TruncatedBy() == std::vector<std::string>{ "nodes" });

	CHECK(b.Emit());
	CHECK(!b.Emit());
	CHECK(b.StepParent());
	CHECK(b.StepParent());
	CHECK(b.StepParent());
	CHECK(!b.StepParent());
	CHECK((b.TruncatedBy() == std::vector<std::string>{ "nodes", "parentSteps", "output" }));
}

TEST_CASE("a traversal budget that is never hit is not truncated")
{
	TraversalBudget b(5, 5, 5);
	b.VisitNode();
	b.StepParent();
	b.Emit();
	CHECK(!b.Truncated());
	CHECK(b.TruncatedBy().empty());
}

TEST_CASE("scene lights order nearest first, unknown distances last, ties stable")
{
	std::vector<SceneEntry> e{
		{ std::nullopt, "a", "p", 0, 0, 0, 0 },
		{ 5.0f, "b", "p", 0, 0, 0, 1 },
		{ 5.0f, "a", "q", 0, 0, 0, 2 },
		{ 5.0f, "a", "p", 1, 0, 0, 3 },
		{ 5.0f, "a", "p", 0, 0, 0, 4 },
		{ 1.0f, "z", "z", 0, 0, 0, 5 },
	};
	OrderScene(e);
	std::vector<std::size_t> order;
	for (const auto& x : e)
		order.push_back(x.index);
	CHECK((order == std::vector<std::size_t>{ 5, 4, 3, 2, 1, 0 }));

	std::vector<SceneEntry> reversed(e.rbegin(), e.rend());
	OrderScene(reversed);
	for (std::size_t i = 0; i < e.size(); ++i)
		CHECK(reversed[i].index == e[i].index);
}

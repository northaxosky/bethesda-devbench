#include "test_framework.h"

#include "tools/inspection/InspectLogic.h"

using namespace dvb::tools::inspection;

TEST_CASE("a traversal budget stops each kind of work at its limit and says which")
{
	TraversalBudget b(2, 1);
	CHECK(b.VisitNode());
	CHECK(b.VisitNode());
	CHECK(!b.VisitNode());
	CHECK(b.Truncated());
	CHECK(b.TruncatedBy() == std::vector<std::string>{ "nodes" });

	CHECK(b.Emit());
	CHECK(!b.Emit());
	CHECK((b.TruncatedBy() == std::vector<std::string>{ "nodes", "output" }));
}

TEST_CASE("scene lights order nearest first, unknown distances last, ties stable")
{
	std::vector<SceneEntry> e{
		{ std::nullopt, "a", 0, 0, 0, 0 },
		{ 5.0f, "b", 0, 0, 0, 1 },
		{ 5.0f, "a", 2, 0, 0, 2 },
		{ 5.0f, "a", 1, 0, 0, 3 },
		{ 5.0f, "a", 0, 0, 0, 4 },
		{ 1.0f, "z", 0, 0, 0, 5 },
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

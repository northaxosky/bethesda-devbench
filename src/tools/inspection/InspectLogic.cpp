#include "InspectLogic.h"

#include <algorithm>
#include <tuple>

namespace dvb::tools::inspection
{
	std::optional<LightsScope> ParseLightsScope(std::string_view a_scope)
	{
		if (a_scope.empty() || a_scope == "ref")
			return LightsScope::kRef;
		if (a_scope == "scene")
			return LightsScope::kScene;
		return std::nullopt;
	}

	std::vector<std::string> TraversalBudget::TruncatedBy() const
	{
		std::vector<std::string> out;
		if (m_nodesExhausted)
			out.emplace_back("nodes");
		if (m_parentsExhausted)
			out.emplace_back("parentSteps");
		if (m_outputExhausted)
			out.emplace_back("output");
		return out;
	}

	void OrderScene(std::vector<SceneEntry>& a_entries)
	{
		std::stable_sort(a_entries.begin(), a_entries.end(), [](const SceneEntry& a, const SceneEntry& b) {
			if (a.distance.has_value() != b.distance.has_value())
				return a.distance.has_value();
			if (a.distance && *a.distance != *b.distance)
				return *a.distance < *b.distance;
			return std::tie(a.name, a.path, a.x, a.y, a.z) < std::tie(b.name, b.path, b.x, b.y, b.z);
		});
	}
}

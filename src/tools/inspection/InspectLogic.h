#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::tools::inspection
{
	enum class LightsScope
	{
		kRef,
		kScene,
	};

	// Graph nodes visited and lights emitted; a spent limit truncates the read.
	class TraversalBudget
	{
	public:
		TraversalBudget(std::size_t a_maxNodes, std::size_t a_maxOutput) :
			m_maxNodes(a_maxNodes), m_maxOutput(a_maxOutput)
		{}

		bool VisitNode() { return Spend(m_nodes, m_maxNodes, m_nodesExhausted); }
		bool Emit() { return Spend(m_output, m_maxOutput, m_outputExhausted); }

		bool                     Truncated() const { return m_nodesExhausted || m_outputExhausted; }
		std::vector<std::string> TruncatedBy() const;

	private:
		static bool Spend(std::size_t& a_used, std::size_t a_max, bool& a_exhausted)
		{
			if (a_used >= a_max)
			{
				a_exhausted = true;
				return false;
			}
			++a_used;
			return true;
		}

		std::size_t m_maxNodes;
		std::size_t m_maxOutput;
		std::size_t m_nodes = 0;
		std::size_t m_output = 0;
		bool        m_nodesExhausted = false;
		bool        m_outputExhausted = false;
	};

	struct SceneEntry
	{
		std::optional<float> distance;  // nullopt when the player's position is unknown
		std::string          name;
		float                x = 0.0f;
		float                y = 0.0f;
		float                z = 0.0f;
		std::size_t          index = 0;  // the caller's own slot for this entry
	};

	// Nearest first, unknown last; ties by name then position for a stable limit.
	void OrderScene(std::vector<SceneEntry>& a_entries);
}

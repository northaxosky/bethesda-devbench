#pragma once

#include "ToolRegistry.h"

#include <string>

namespace dvb::tools::scenario
{
	enum class Comparison
	{
		kEqual,
		kNotEqual,
		kLess,
		kLessEqual,
		kGreater,
		kGreaterEqual,
		kExists,
	};

	struct ValuePredicate
	{
		std::string tool;
		json        args = json::object();
		std::string path;
		Comparison  comparison = Comparison::kEqual;
		json        expected;
	};

	struct PredicateObservation
	{
		bool        observed = false;
		bool        satisfied = false;
		bool        available = false;
		bool        retryable = false;
		int         status = 200;
		std::string error;
		json        actual;
		json        source;
	};

	/// Parse and preflight a VALUE predicate. The target must already be registered
	/// and declared read-only; malformed predicates fail before a scenario starts.
	ValuePredicate ParseValuePredicate(const json& a_value, const ToolRegistry& a_registry);

	/// Invoke the predicate's read-only source and compare the JSON Pointer value.
	/// Transport failures are represented in the observation rather than thrown.
	PredicateObservation ObserveValue(
		const ValuePredicate& a_predicate,
		const ToolRegistry&   a_registry,
		const ToolContext&    a_context);

	json DescribeValuePredicate(const ValuePredicate& a_predicate);
}

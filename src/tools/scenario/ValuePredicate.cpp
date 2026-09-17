#include "tools/scenario/ValuePredicate.h"

#include "ScenarioPolicy.h"

#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <set>

namespace dvb::tools::scenario
{
	namespace
	{
		constexpr std::array<std::string_view, 7> kComparisonKeys{
			"eq", "ne", "lt", "le", "gt", "ge", "exists"
		};

		void RejectUnknownKeys(const json& a_value, const std::set<std::string_view>& a_allowed)
		{
			for (const auto& [key, unused] : a_value.items())
			{
				(void)unused;
				if (!a_allowed.contains(key))
					throw ToolError(400, std::format("unknown predicate field '{}'", key));
			}
		}

		bool IsNumber(const json& a_value)
		{
			return a_value.is_number_integer() || a_value.is_number_unsigned() ||
			       a_value.is_number_float();
		}

		bool SameComparableType(const json& a_actual, const json& a_expected)
		{
			if (IsNumber(a_actual) && IsNumber(a_expected))
				return true;
			return a_actual.type() == a_expected.type();
		}

		enum class NumericOrder
		{
			kLess,
			kEqual,
			kGreater,
			kUnordered,
		};

		NumericOrder Reverse(NumericOrder a_order)
		{
			if (a_order == NumericOrder::kLess)
				return NumericOrder::kGreater;
			if (a_order == NumericOrder::kGreater)
				return NumericOrder::kLess;
			return a_order;
		}

		NumericOrder CompareSignedUnsigned(std::int64_t a_signed, std::uint64_t a_unsigned)
		{
			if (a_signed < 0)
				return NumericOrder::kLess;
			const auto value = static_cast<std::uint64_t>(a_signed);
			if (value < a_unsigned)
				return NumericOrder::kLess;
			if (value > a_unsigned)
				return NumericOrder::kGreater;
			return NumericOrder::kEqual;
		}

		NumericOrder CompareSignedDouble(std::int64_t a_integer, double a_float)
		{
			if (!std::isfinite(a_float))
				return NumericOrder::kUnordered;
			const double minSigned = -std::ldexp(1.0, 63);
			const double maxSignedExclusive = std::ldexp(1.0, 63);
			if (a_float < minSigned)
				return NumericOrder::kGreater;
			if (a_float >= maxSignedExclusive)
				return NumericOrder::kLess;

			const double floorValue = std::floor(a_float);
			const auto   integerFloor = static_cast<std::int64_t>(floorValue);
			if (a_integer < integerFloor)
				return NumericOrder::kLess;
			if (a_integer > integerFloor)
				return NumericOrder::kGreater;
			return a_float == floorValue ? NumericOrder::kEqual : NumericOrder::kLess;
		}

		NumericOrder CompareUnsignedDouble(std::uint64_t a_integer, double a_float)
		{
			if (!std::isfinite(a_float))
				return NumericOrder::kUnordered;
			if (a_float < 0.0)
				return NumericOrder::kGreater;
			const double maxUnsignedExclusive = std::ldexp(1.0, 64);
			if (a_float >= maxUnsignedExclusive)
				return NumericOrder::kLess;

			const double floorValue = std::floor(a_float);
			const auto   integerFloor = static_cast<std::uint64_t>(floorValue);
			if (a_integer < integerFloor)
				return NumericOrder::kLess;
			if (a_integer > integerFloor)
				return NumericOrder::kGreater;
			return a_float == floorValue ? NumericOrder::kEqual : NumericOrder::kLess;
		}

		NumericOrder CompareNumbers(const json& a_actual, const json& a_expected)
		{
			if (a_actual.is_number_unsigned())
			{
				const auto actual = a_actual.get<std::uint64_t>();
				if (a_expected.is_number_unsigned())
				{
					const auto expected = a_expected.get<std::uint64_t>();
					if (actual < expected)
						return NumericOrder::kLess;
					if (actual > expected)
						return NumericOrder::kGreater;
					return NumericOrder::kEqual;
				}
				if (a_expected.is_number_float())
					return CompareUnsignedDouble(actual, a_expected.get<double>());
				return Reverse(CompareSignedUnsigned(
					a_expected.get<std::int64_t>(), actual));
			}

			if (a_actual.is_number_float())
			{
				const double actual = a_actual.get<double>();
				if (a_expected.is_number_float())
				{
					const double expected = a_expected.get<double>();
					if (!std::isfinite(actual) || !std::isfinite(expected))
						return NumericOrder::kUnordered;
					if (actual < expected)
						return NumericOrder::kLess;
					if (actual > expected)
						return NumericOrder::kGreater;
					return NumericOrder::kEqual;
				}
				if (a_expected.is_number_unsigned())
					return Reverse(CompareUnsignedDouble(
						a_expected.get<std::uint64_t>(), actual));
				return Reverse(CompareSignedDouble(
					a_expected.get<std::int64_t>(), actual));
			}

			const auto actual = a_actual.get<std::int64_t>();
			if (a_expected.is_number_unsigned())
				return CompareSignedUnsigned(
					actual, a_expected.get<std::uint64_t>());
			if (a_expected.is_number_float())
				return CompareSignedDouble(actual, a_expected.get<double>());
			const auto expected = a_expected.get<std::int64_t>();
			if (actual < expected)
				return NumericOrder::kLess;
			if (actual > expected)
				return NumericOrder::kGreater;
			return NumericOrder::kEqual;
		}

		bool ValuesEqual(const json& a_actual, const json& a_expected)
		{
			if (IsNumber(a_actual) && IsNumber(a_expected))
				return CompareNumbers(a_actual, a_expected) == NumericOrder::kEqual;
			return a_actual == a_expected;
		}

		std::string ComparisonName(Comparison a_comparison)
		{
			switch (a_comparison)
			{
				case Comparison::kEqual:
					return "eq";
				case Comparison::kNotEqual:
					return "ne";
				case Comparison::kLess:
					return "lt";
				case Comparison::kLessEqual:
					return "le";
				case Comparison::kGreater:
					return "gt";
				case Comparison::kGreaterEqual:
					return "ge";
				case Comparison::kExists:
					return "exists";
			}
			return "unknown";
		}

		Comparison ParseComparison(std::string_view a_key)
		{
			if (a_key == "eq")
				return Comparison::kEqual;
			if (a_key == "ne")
				return Comparison::kNotEqual;
			if (a_key == "lt")
				return Comparison::kLess;
			if (a_key == "le")
				return Comparison::kLessEqual;
			if (a_key == "gt")
				return Comparison::kGreater;
			if (a_key == "ge")
				return Comparison::kGreaterEqual;
			return Comparison::kExists;
		}

		bool Compare(
			const ValuePredicate& a_predicate,
			const json&           a_actual,
			std::string&          a_error)
		{
			if (!SameComparableType(a_actual, a_predicate.expected))
			{
				a_error = std::format(
					"predicate type mismatch at '{}': actual {} vs expected {}",
					a_predicate.path,
					a_actual.type_name(),
					a_predicate.expected.type_name());
				return false;
			}

			switch (a_predicate.comparison)
			{
				case Comparison::kEqual:
					if (IsNumber(a_actual) &&
						CompareNumbers(a_actual, a_predicate.expected) ==
							NumericOrder::kUnordered)
					{
						a_error = "predicate cannot compare non-finite numeric values";
						return false;
					}
					return ValuesEqual(a_actual, a_predicate.expected);
				case Comparison::kNotEqual:
					if (IsNumber(a_actual) &&
						CompareNumbers(a_actual, a_predicate.expected) ==
							NumericOrder::kUnordered)
					{
						a_error = "predicate cannot compare non-finite numeric values";
						return false;
					}
					return !ValuesEqual(a_actual, a_predicate.expected);
				case Comparison::kLess:
				case Comparison::kLessEqual:
				case Comparison::kGreater:
				case Comparison::kGreaterEqual:
				{
					if (!IsNumber(a_actual) || !IsNumber(a_predicate.expected))
					{
						a_error = std::format(
							"predicate '{}' requires numeric actual and expected values",
							ComparisonName(a_predicate.comparison));
						return false;
					}
					const auto order = CompareNumbers(a_actual, a_predicate.expected);
					if (order == NumericOrder::kUnordered)
					{
						a_error = "predicate cannot compare non-finite numeric values";
						return false;
					}
					if (a_predicate.comparison == Comparison::kLess)
						return order == NumericOrder::kLess;
					if (a_predicate.comparison == Comparison::kLessEqual)
						return order == NumericOrder::kLess || order == NumericOrder::kEqual;
					if (a_predicate.comparison == Comparison::kGreater)
						return order == NumericOrder::kGreater;
					return order == NumericOrder::kGreater || order == NumericOrder::kEqual;
				}
				case Comparison::kExists:
					break;
			}
			return false;
		}
	}

	ValuePredicate ParseValuePredicate(const json& a_value, const ToolRegistry& a_registry)
	{
		if (!a_value.is_object())
			throw ToolError(400, "VALUE predicate must be an object");

		RejectUnknownKeys(
			a_value,
			{ "tool", "args", "path", "eq", "ne", "lt", "le", "gt", "ge", "exists" });

		const auto toolIt = a_value.find("tool");
		if (toolIt == a_value.end() || !toolIt->is_string() ||
			toolIt->get_ref<const std::string&>().empty())
			throw ToolError(400, "VALUE predicate requires a non-empty string 'tool'");
		const std::string tool = toolIt->get<std::string>();

		const auto descriptor = a_registry.Describe(tool);
		if (!descriptor)
			throw ToolError(404, std::format("unknown predicate tool '{}'", tool));
		if (!descriptor->readOnly)
			throw ToolError(
				400,
				std::format("predicate tool '{}' is not declared readOnly", tool));
		if (tool == "scenario")
			throw ToolError(400, "scenario predicates cannot target the scenario tool");

		json args = json::object();
		if (const auto argsIt = a_value.find("args"); argsIt != a_value.end())
		{
			if (!argsIt->is_object())
				throw ToolError(400, "predicate 'args' must be an object");
			args = *argsIt;
		}

		const auto pathIt = a_value.find("path");
		if (pathIt == a_value.end() || !pathIt->is_string())
			throw ToolError(400, "VALUE predicate requires string JSON Pointer 'path'");
		const std::string path = pathIt->get<std::string>();
		try
		{
			(void)json::json_pointer(path);
		}
		catch (const json::parse_error& a_error)
		{
			throw ToolError(
				400,
				std::format("invalid predicate JSON Pointer '{}': {}", path, a_error.what()));
		}

		const json* comparisonValue = nullptr;
		std::string comparisonKey;
		for (const auto key : kComparisonKeys)
		{
			if (const auto it = a_value.find(key); it != a_value.end())
			{
				if (comparisonValue)
					throw ToolError(400, "VALUE predicate requires exactly one comparison");
				comparisonValue = &*it;
				comparisonKey.assign(key);
			}
		}
		if (!comparisonValue)
			throw ToolError(400, "VALUE predicate requires exactly one comparison");

		const Comparison comparison = ParseComparison(comparisonKey);
		if (comparison == Comparison::kExists)
		{
			if (!comparisonValue->is_boolean())
				throw ToolError(400, "predicate 'exists' must be a boolean");
		}
		else if ((comparison == Comparison::kLess ||
					 comparison == Comparison::kLessEqual ||
					 comparison == Comparison::kGreater ||
					 comparison == Comparison::kGreaterEqual) &&
				 !IsNumber(*comparisonValue))
		{
			throw ToolError(
				400,
				std::format("predicate '{}' expected value must be numeric", comparisonKey));
		}

		return {
			.tool = std::move(tool),
			.args = std::move(args),
			.path = path,
			.comparison = comparison,
			.expected = *comparisonValue,
		};
	}

	PredicateObservation ObserveValue(
		const ValuePredicate& a_predicate,
		const ToolRegistry&   a_registry,
		const ToolContext&    a_context)
	{
		PredicateObservation observation;
		ToolContext          context = a_context;
		context.internal = true;
		const ToolResult result =
			a_registry.Invoke(a_predicate.tool, a_predicate.args, context);
		if (!result.ok)
		{
			observation.status = result.errorCode;
			observation.error = result.errorMessage;
			observation.retryable = result.errorCode == 503 || result.errorCode == 504;
			return observation;
		}
		if (ScenarioPolicy::IsEmbeddedToolFailure(result.value))
		{
			observation.status = ScenarioPolicy::kEmbeddedToolFallbackErrorCode;
			const json code = ScenarioPolicy::EmbeddedToolErrorCode(result.value);
			if (code.is_number_unsigned() &&
				code.get<std::uint64_t>() <=
					static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
			{
				observation.status = static_cast<int>(code.get<std::uint64_t>());
			}
			else if (code.is_number_integer())
			{
				const auto value = code.get<std::int64_t>();
				if (value >= 0 && value <= std::numeric_limits<int>::max())
					observation.status = static_cast<int>(value);
			}
			observation.error = ScenarioPolicy::EmbeddedToolErrorMessage(result.value);
			observation.source = result.value;
			return observation;
		}
		if (result.value.is_object())
		{
			const auto queued = result.value.find("queued");
			if (queued != result.value.end() && queued->is_boolean() && queued->get<bool>())
			{
				observation.status = 502;
				observation.error =
					"predicate tool returned an asynchronous receipt rather than an observation";
				observation.source = result.value;
				return observation;
			}
		}

		observation.observed = true;
		observation.source = result.value;
		const json::json_pointer pointer(a_predicate.path);
		const json*              actual = nullptr;
		try
		{
			actual = &result.value.at(pointer);
		}
		catch (const json::out_of_range&)
		{}
		catch (const json::type_error&)
		{}

		observation.available = actual != nullptr;
		if (a_predicate.comparison == Comparison::kExists)
		{
			observation.actual = observation.available;
			observation.satisfied =
				observation.available == a_predicate.expected.get<bool>();
			return observation;
		}
		if (!actual)
		{
			observation.actual = nullptr;
			return observation;
		}

		observation.actual = *actual;
		observation.satisfied = Compare(a_predicate, *actual, observation.error);
		if (!observation.error.empty())
			observation.status = 422;
		return observation;
	}

	json DescribeValuePredicate(const ValuePredicate& a_predicate)
	{
		json out{
			{ "tool", a_predicate.tool },
			{ "args", a_predicate.args },
			{ "path", a_predicate.path },
		};
		out[ComparisonName(a_predicate.comparison)] = a_predicate.expected;
		return out;
	}
}

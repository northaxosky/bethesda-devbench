#pragma once

#include "ToolRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace dvb::tools::inspection
{
	inline constexpr std::size_t kDefaultListLimit = 100;
	inline constexpr std::size_t kMaxListLimit = 500;

	struct Request
	{
		std::string kind = "state";
		std::string formId;
		std::string formType;
		bool        selected = false;
		double      radius = 0.0;
		std::size_t limit = kDefaultListLimit;
	};

	inline std::string LowerAscii(std::string a_value)
	{
		std::ranges::transform(a_value, a_value.begin(), [](unsigned char a_ch) {
			return static_cast<char>(a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
		});
		return a_value;
	}

	inline std::string NormalizeFormType(std::string a_filter)
	{
		a_filter = LowerAscii(std::move(a_filter));
		static const std::unordered_map<std::string, std::string> kAliases{
			{ "actor", "achr" },
			{ "npc", "npc_" },
			{ "container", "cont" },
			{ "door", "door" },
			{ "weapon", "weap" },
			{ "armor", "armo" },
			{ "book", "book" },
			{ "ingredient", "ingr" },
			{ "potion", "alch" },
			{ "chem", "alch" },
			{ "misc", "misc" },
			{ "light", "ligh" },
			{ "furniture", "furn" },
			{ "activator", "acti" },
			{ "flora", "flor" },
			{ "tree", "tree" },
			{ "static", "stat" },
			{ "key", "keym" },
			{ "ammo", "ammo" },
			{ "note", "note" },
		};
		if (const auto it = kAliases.find(a_filter); it != kAliases.end())
			return it->second;
		return a_filter;
	}

	inline bool MatchesFormType(std::string_view a_formType, std::string_view a_normalizedFilter)
	{
		if (a_normalizedFilter.empty())
			return true;
		return LowerAscii(std::string(a_formType)).find(a_normalizedFilter) != std::string::npos;
	}

	inline std::string ReadKind(const json& a_args)
	{
		if (!a_args.is_object())
			throw ToolError(400, "inspect arguments must be an object");

		if (const auto it = a_args.find("kind"); it != a_args.end())
		{
			if (!it->is_string())
				throw ToolError(400, "'kind' must be a string");
			auto kind = LowerAscii(it->get<std::string>());
			if (kind.empty())
				throw ToolError(400, "'kind' must not be empty");
			return kind;
		}
		return "state";
	}

	inline Request ParseRequest(const json& a_args)
	{
		Request request;
		request.kind = ReadKind(a_args);
		if (const auto it = a_args.find("formId"); it != a_args.end())
		{
			if (!it->is_string())
				throw ToolError(400, "'formId' must be a string");
			request.formId = it->get<std::string>();
		}
		if (const auto it = a_args.find("formType"); it != a_args.end())
		{
			if (!it->is_string())
				throw ToolError(400, "'formType' must be a string");
			request.formType = NormalizeFormType(it->get<std::string>());
		}
		if (const auto it = a_args.find("selected"); it != a_args.end())
		{
			if (!it->is_boolean())
				throw ToolError(400, "'selected' must be a boolean");
			request.selected = it->get<bool>();
		}
		if (const auto it = a_args.find("radius"); it != a_args.end())
		{
			if (!it->is_number())
				throw ToolError(400, "'radius' must be a number");
			request.radius = it->get<double>();
			if (!std::isfinite(request.radius) || request.radius < 0.0)
				throw ToolError(400, "'radius' must be finite and >= 0");
		}
		if (const auto it = a_args.find("limit"); it != a_args.end())
		{
			if (!it->is_number_integer() && !it->is_number_unsigned())
				throw ToolError(400, "'limit' must be an integer");
			std::uint64_t limit = 0;
			if (it->is_number_unsigned())
			{
				limit = it->get<std::uint64_t>();
			}
			else
			{
				const auto signedLimit = it->get<std::int64_t>();
				if (signedLimit < 0)
					throw ToolError(
						400, std::format("'limit' must be between 0 and {}", kMaxListLimit));
				limit = static_cast<std::uint64_t>(signedLimit);
			}
			if (limit > kMaxListLimit)
				throw ToolError(
					400, std::format("'limit' must be between 0 and {}", kMaxListLimit));
			request.limit = static_cast<std::size_t>(limit);
		}
		if (request.kind == "refs" && !request.formId.empty() && request.selected)
			throw ToolError(400, "'formId' and 'selected' are mutually exclusive");
		return request;
	}

	class ResultWindow
	{
	public:
		explicit ResultWindow(std::size_t a_limit) :
			limit_(a_limit)
		{}

		// Returns false after observing the first value that cannot be returned. Native
		// readers can stop there instead of holding an engine lock while counting an
		// unbounded collection. In that case count is a lower bound and countExact=false.
		bool Observe(json a_value)
		{
			++matched_;
			if (values_.size() < limit_)
			{
				values_.push_back(std::move(a_value));
				return true;
			}
			truncated_ = true;
			return false;
		}

		void MarkComplete() noexcept { complete_ = true; }

		[[nodiscard]] bool Truncated() const noexcept { return truncated_; }

		json Build(std::string_view a_key)
		{
			return json{
				{ "count", matched_ },
				{ "countExact", complete_ },
				{ "returned", values_.size() },
				{ "truncated", truncated_ },
				{ std::string(a_key), std::move(values_) },
			};
		}

	private:
		std::size_t limit_ = 0;
		std::size_t matched_ = 0;
		bool        truncated_ = false;
		bool        complete_ = false;
		json        values_ = json::array();
	};
}

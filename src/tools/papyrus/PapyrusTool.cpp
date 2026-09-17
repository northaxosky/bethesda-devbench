#include "PapyrusTool.h"

#include "tools/ToolPermissions.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace dvb::tools::papyrus
{
	namespace
	{
		std::string LowerAscii(std::string_view a_value)
		{
			std::string out(a_value);
			std::ranges::transform(out, out.begin(), [](unsigned char a_ch) {
				return static_cast<char>(a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return out;
		}

		bool ContainsNul(std::string_view a_value)
		{
			return a_value.find('\0') != std::string_view::npos;
		}

		std::string ReadRequiredString(
			const json& a_args, std::string_view a_key, std::string_view a_context)
		{
			const auto key = std::string(a_key);
			const auto it = a_args.find(key);
			if (it == a_args.end() || !it->is_string())
				throw ToolError(400, std::format("{} requires string '{}'", a_context, a_key));
			auto value = it->get<std::string>();
			if (value.empty() || ContainsNul(value))
				throw ToolError(400, std::format("{} requires non-empty '{}'", a_context, a_key));
			if (value.size() > kMaxIdentifierBytes)
				throw ToolError(400, std::format(
										 "{} '{}' exceeds {} bytes", a_context, a_key, kMaxIdentifierBytes));
			return value;
		}

		std::string ReadOptionalString(
			const json& a_args, std::string_view a_key, std::string a_default = {})
		{
			const auto key = std::string(a_key);
			const auto it = a_args.find(key);
			if (it == a_args.end())
				return a_default;
			if (!it->is_string())
				throw ToolError(400, std::format("papyrus '{}' must be a string", a_key));
			auto value = it->get<std::string>();
			if (ContainsNul(value) || value.size() > kMaxIdentifierBytes)
				throw ToolError(400, std::format(
										 "papyrus '{}' exceeds {} bytes", a_key, kMaxIdentifierBytes));
			return value;
		}

		std::size_t ReadLimit(const json& a_args)
		{
			const auto it = a_args.find("limit");
			if (it == a_args.end())
				return kDefaultClassLimit;
			if (!it->is_number_unsigned() && !it->is_number_integer())
				throw ToolError(400, "papyrus 'limit' must be an integer");
			const bool inRange = it->is_number_unsigned() ?
			                         it->get<std::uint64_t>() <= kMaxClassLimit :
			                         it->get<std::int64_t>() >= 0 &&
			                             it->get<std::int64_t>() <=
			                                 static_cast<std::int64_t>(kMaxClassLimit);
			if (!inRange)
				throw ToolError(400, std::format(
										 "papyrus 'limit' must be between 0 and {}", kMaxClassLimit));
			return it->get<std::size_t>();
		}

		int ReadTimeout(const json& a_args)
		{
			const auto it = a_args.find("timeoutMs");
			if (it == a_args.end())
				return kDefaultTimeoutMs;
			if (!it->is_number_integer() && !it->is_number_unsigned())
				throw ToolError(400, "papyrus call: 'timeoutMs' must be an integer");
			const bool inRange = it->is_number_unsigned() ?
			                         it->get<std::uint64_t>() >= 1 &&
			                             it->get<std::uint64_t>() <=
			                                 static_cast<std::uint64_t>(kMaxTimeoutMs) :
			                         it->get<std::int64_t>() >= 1 &&
			                             it->get<std::int64_t>() <= kMaxTimeoutMs;
			if (!inRange)
				throw ToolError(400, std::format(
										 "papyrus call: 'timeoutMs' must be between 1 and {}", kMaxTimeoutMs));
			return it->get<int>();
		}

		PapyrusSelf ParseSelf(const json& a_value)
		{
			if (a_value.is_string())
			{
				const auto value = a_value.get<std::string>();
				if (LowerAscii(value) != "selected")
					throw ToolError(400,
						"papyrus call: string 'self' must be exactly 'selected'");
				return PapyrusSelf{ .kind = PapyrusSelf::Kind::kSelected, .form = {} };
			}
			if (!a_value.is_object())
				throw ToolError(400,
					"papyrus call: 'self' must be 'selected' or {\"form\":\"FormID|EditorID\"}");
			if (a_value.size() != 1 || !a_value.contains("form") || !a_value.at("form").is_string())
				throw ToolError(400,
					"papyrus call: object 'self' must contain only string field 'form'");
			auto form = a_value.at("form").get<std::string>();
			if (form.empty() || ContainsNul(form) || form.size() > kMaxIdentifierBytes)
				throw ToolError(400, "papyrus call: self.form is empty or too long");
			return PapyrusSelf{ .kind = PapyrusSelf::Kind::kForm, .form = std::move(form) };
		}

		const PapyrusStructDefinition* FindStruct(
			const PapyrusFunctionDefinition& a_function, std::string_view a_name)
		{
			const auto needle = LowerAscii(a_name);
			for (const auto& definition : a_function.structs)
				if (LowerAscii(definition.name) == needle)
					return std::addressof(definition);
			return nullptr;
		}

		void RequireSupportedType(
			const PapyrusType& a_type, const PapyrusFunctionDefinition& a_function,
			std::string_view a_path, bool a_allowNone, std::size_t a_depth,
			std::unordered_set<std::string>& a_structPath)
		{
			if (a_depth > kMaxValueDepth)
				throw ToolError(422, std::format(
										 "papyrus call: type metadata for {} exceeds depth {}", a_path,
										 kMaxValueDepth));
			if (a_type.array && a_type.kind == PapyrusTypeKind::kNone)
				throw ToolError(422, std::format(
										 "papyrus call: {} has unsupported type None[]", a_path));
			switch (a_type.kind)
			{
				case PapyrusTypeKind::kNone:
					if (!a_allowNone)
						throw ToolError(422, std::format(
												 "papyrus call: {} has unsupported parameter type None",
												 a_path));
					return;
				case PapyrusTypeKind::kBool:
				case PapyrusTypeKind::kInt:
				case PapyrusTypeKind::kFloat:
				case PapyrusTypeKind::kString:
					return;
				case PapyrusTypeKind::kObject:
					if (a_type.name.empty())
						throw ToolError(422, std::format(
												 "papyrus call: {} has object type metadata with no class name",
												 a_path));
					return;
				case PapyrusTypeKind::kStruct:
				{
					if (a_type.name.empty())
						throw ToolError(422, std::format(
												 "papyrus call: {} has struct type metadata with no name",
												 a_path));
					const auto* definition = FindStruct(a_function, a_type.name);
					if (!definition)
						throw ToolError(422, std::format(
												 "papyrus call: struct metadata for '{}' is unavailable",
												 a_type.name));
					const auto key = LowerAscii(a_type.name);
					if (!a_structPath.insert(key).second)
						return;
					for (const auto& field : definition->fields)
						RequireSupportedType(field.type, a_function,
							std::format("{}.{}", a_path, field.name), false,
							a_depth + 1, a_structPath);
					a_structPath.erase(key);
					return;
				}
				case PapyrusTypeKind::kVar:
					throw ToolError(422, std::format(
											 "papyrus call: {} has unsupported type Var; "
											 "untyped JSON-to-VM inference is not allowed",
											 a_path));
				case PapyrusTypeKind::kUnsupported:
					throw ToolError(422, std::format(
											 "papyrus call: {} has unsupported type '{}'", a_path,
											 FormatPapyrusType(a_type)));
			}
		}

		bool IsTypedNull(const json& a_value, const PapyrusType& a_type)
		{
			if (!a_value.is_object() || a_value.size() != 2 ||
				!a_value.contains("type") || !a_value.at("type").is_string() ||
				!a_value.contains("value") || !a_value.at("value").is_null())
				return false;
			return LowerAscii(a_value.at("type").get<std::string>()) ==
			       LowerAscii(FormatPapyrusType(a_type));
		}

		struct ValidationBudget
		{
			std::size_t nodes = 0;
		};

		void ConsumeNode(ValidationBudget& a_budget)
		{
			if (++a_budget.nodes > kMaxValueNodes)
				throw ToolError(400, std::format(
										 "papyrus call: values exceed the {} node limit", kMaxValueNodes));
		}

		void ValidateValue(
			const json& a_value, const PapyrusType& a_type,
			const PapyrusFunctionDefinition& a_function, std::size_t a_depth,
			ValidationBudget& a_budget, std::string_view a_path);

		void ValidateScalar(
			const json& a_value, const PapyrusType& a_type,
			const PapyrusFunctionDefinition& a_function, std::size_t a_depth,
			ValidationBudget& a_budget, std::string_view a_path)
		{
			switch (a_type.kind)
			{
				case PapyrusTypeKind::kBool:
					if (!a_value.is_boolean())
						throw ToolError(400, std::format(
												 "papyrus call: {} must be Bool", a_path));
					return;
				case PapyrusTypeKind::kInt:
				{
					const bool valid = a_value.is_number_unsigned() ?
					                       a_value.get<std::uint64_t>() <=
					                           static_cast<std::uint64_t>(
												   std::numeric_limits<std::int32_t>::max()) :
					                       a_value.is_number_integer() &&
					                           a_value.get<std::int64_t>() >=
					                               std::numeric_limits<std::int32_t>::min() &&
					                           a_value.get<std::int64_t>() <=
					                               std::numeric_limits<std::int32_t>::max();
					if (!valid)
						throw ToolError(400, std::format(
												 "papyrus call: {} must be a signed 32-bit Int", a_path));
					return;
				}
				case PapyrusTypeKind::kFloat:
					if (!a_value.is_number())
						throw ToolError(400, std::format(
												 "papyrus call: {} must be a finite Float", a_path));
					if (const auto value = a_value.get<double>();
						!std::isfinite(value) ||
						std::abs(value) > std::numeric_limits<float>::max())
						throw ToolError(400, std::format(
												 "papyrus call: {} is outside the finite Float range", a_path));
					return;
				case PapyrusTypeKind::kString:
					if (!a_value.is_string() ||
						ContainsNul(a_value.get_ref<const std::string&>()) ||
						a_value.get_ref<const std::string&>().size() > kMaxStringBytes)
						throw ToolError(400, std::format(
												 "papyrus call: {} must be a string no larger than {} bytes",
												 a_path, kMaxStringBytes));
					return;
				case PapyrusTypeKind::kObject:
					if (IsTypedNull(a_value, a_type))
						return;
					if (!a_value.is_object() || a_value.size() != 1 ||
						!a_value.contains("form") || !a_value.at("form").is_string() ||
						a_value.at("form").get_ref<const std::string&>().empty() ||
						ContainsNul(a_value.at("form").get_ref<const std::string&>()) ||
						a_value.at("form").get_ref<const std::string&>().size() >
							kMaxIdentifierBytes)
						throw ToolError(400, std::format(
												 "papyrus call: {} must be {{\"form\":\"FormID|EditorID\"}} "
												 "or typed null {{\"type\":\"{}\",\"value\":null}}",
												 a_path, FormatPapyrusType(a_type)));
					return;
				case PapyrusTypeKind::kStruct:
				{
					if (IsTypedNull(a_value, a_type))
						return;
					if (!a_value.is_object() || a_value.size() != 2 ||
						!a_value.contains("type") || !a_value.at("type").is_string() ||
						!a_value.contains("fields") || !a_value.at("fields").is_object())
						throw ToolError(400, std::format(
												 "papyrus call: {} must be a typed struct "
												 "{{\"type\":\"{}\",\"fields\":{{...}}}} or typed null",
												 a_path, a_type.name));
					if (LowerAscii(a_value.at("type").get<std::string>()) != LowerAscii(a_type.name))
						throw ToolError(400, std::format(
												 "papyrus call: {} declares struct '{}' but '{}' is required",
												 a_path, a_value.at("type").get<std::string>(), a_type.name));
					const auto* definition = FindStruct(a_function, a_type.name);
					if (!definition)
						throw ToolError(422, std::format(
												 "papyrus call: struct metadata for '{}' is unavailable",
												 a_type.name));

					std::unordered_map<std::string, const PapyrusStructField*> fields;
					for (const auto& field : definition->fields)
						fields.emplace(LowerAscii(field.name), std::addressof(field));
					std::unordered_set<std::string> supplied;
					for (const auto& [name, value] : a_value.at("fields").items())
					{
						const auto lowered = LowerAscii(name);
						if (!supplied.insert(lowered).second)
							throw ToolError(400, std::format(
													 "papyrus call: {} repeats field '{}' with different casing",
													 a_path, name));
						const auto field = fields.find(lowered);
						if (field == fields.end())
							throw ToolError(400, std::format(
													 "papyrus call: {} has unknown field '{}' for struct '{}'",
													 a_path, name, a_type.name));
						ValidateValue(value, field->second->type, a_function, a_depth + 1,
							a_budget, std::format("{}.{}", a_path, field->second->name));
					}
					return;
				}
				case PapyrusTypeKind::kNone:
					throw ToolError(422, std::format(
											 "papyrus call: {} has unsupported parameter type None", a_path));
				case PapyrusTypeKind::kVar:
					throw ToolError(422, std::format(
											 "papyrus call: {} has unsupported parameter type Var; "
											 "untyped JSON-to-VM inference is not allowed",
											 a_path));
				case PapyrusTypeKind::kUnsupported:
					throw ToolError(422, std::format(
											 "papyrus call: {} has unsupported parameter type '{}'",
											 a_path, FormatPapyrusType(a_type)));
			}
		}

		void ValidateValue(
			const json& a_value, const PapyrusType& a_type,
			const PapyrusFunctionDefinition& a_function, std::size_t a_depth,
			ValidationBudget& a_budget, std::string_view a_path)
		{
			if (a_depth > kMaxValueDepth)
				throw ToolError(400, std::format(
										 "papyrus call: {} exceeds the maximum nesting depth of {}",
										 a_path, kMaxValueDepth));
			ConsumeNode(a_budget);
			if (!a_type.array)
			{
				ValidateScalar(a_value, a_type, a_function, a_depth, a_budget, a_path);
				return;
			}
			if (IsTypedNull(a_value, a_type))
				return;
			if (!a_value.is_array())
				throw ToolError(400, std::format(
										 "papyrus call: {} must be an array or typed null "
										 "{{\"type\":\"{}\",\"value\":null}}",
										 a_path, FormatPapyrusType(a_type)));
			if (a_value.size() > kMaxArrayElements)
				throw ToolError(400, std::format(
										 "papyrus call: {} exceeds the {} element array limit",
										 a_path, kMaxArrayElements));
			auto elementType = a_type;
			elementType.array = false;
			for (std::size_t i = 0; i < a_value.size(); ++i)
				ValidateValue(a_value[i], elementType, a_function, a_depth + 1,
					a_budget, std::format("{}[{}]", a_path, i));
		}

		json HandlePapyrus(
			const json& a_args, bool a_allowCalls, const PapyrusBackend& a_backend)
		{
			if (!a_args.is_object())
				throw ToolError(400, "papyrus arguments must be an object");
			const auto action = LowerAscii(ReadOptionalString(a_args, "action", "list"));
			if (action == "list")
			{
				if (!a_backend.listClasses)
					throw ToolError(503, "papyrus class discovery is unavailable");
				const auto filter = ReadOptionalString(a_args, "filter");
				const auto limit = ReadLimit(a_args);
				auto       result = a_backend.listClasses(filter, limit);
				return json{
					{ "action", "list" },
					{ "total", result.total },
					{ "returned", result.scripts.size() },
					{ "truncated", result.total > result.scripts.size() },
					{ "scripts", std::move(result.scripts) },
				};
			}
			if (action == "describe")
			{
				if (!a_backend.describeScript)
					throw ToolError(503, "papyrus script description is unavailable");
				auto result = a_backend.describeScript(
					ReadRequiredString(a_args, "script", "papyrus describe"));
				if (!result.is_object())
					throw ToolError(500, "papyrus backend returned a non-object description");
				result["action"] = "describe";
				return result;
			}
			if (action != "call")
				throw ToolError(400, std::format(
										 "unknown papyrus action '{}' (list|describe|call)", action));

			RequireToolPermission(a_allowCalls, ToolPermission::kPapyrusCalls);
			if (!a_backend.resolveFunction || !a_backend.queueCall)
				throw ToolError(503, "papyrus call dispatch is unavailable");

			PapyrusCallRequest request;
			request.script = ReadRequiredString(a_args, "script", "papyrus call");
			request.function = ReadRequiredString(a_args, "function", "papyrus call");
			if (const auto self = a_args.find("self"); self != a_args.end())
				request.self = ParseSelf(*self);
			if (const auto arguments = a_args.find("args"); arguments != a_args.end())
				request.arguments = *arguments;
			if (!request.arguments.is_array())
				throw ToolError(400, "papyrus call: 'args' must be an array");
			if (request.arguments.size() > kMaxArguments)
				throw ToolError(400, std::format(
										 "papyrus call: no more than {} arguments are allowed", kMaxArguments));
			const int timeoutMs = ReadTimeout(a_args);

			const auto definition = a_backend.resolveFunction(request);
			if (definition.bound != request.self.has_value())
				throw ToolError(500, "papyrus backend returned mismatched function target metadata");
			ValidatePapyrusArguments(request.arguments, definition);

			auto state = std::make_shared<PapyrusCallState>();
			if (!a_backend.queueCall(std::move(request), state))
				throw ToolError(503,
					"papyrus call could not be queued on the game thread; it was not dispatched");

			const auto outcome = state->WaitFor(std::chrono::milliseconds(timeoutMs));
			if (!outcome)
				throw ToolError(504, std::format(
										 "papyrus call '{}.{}' did not complete within {}ms; "
										 "the accepted call may still complete later, timeout is not "
										 "cancellation, and the call must not be retried automatically",
										 definition.script, definition.function, timeoutMs));
			if (!outcome->ok)
				throw ToolError(outcome->errorCode, std::format(
														"papyrus call '{}.{}': {}",
														definition.script, definition.function, outcome->error));
			return json{
				{ "action", "call" },
				{ "called", true },
				{ "completed", true },
				{ "returned", outcome->returned },
				{ "returnedType", outcome->returnedType },
			};
		}
	}

	void PapyrusCallState::CompleteSuccess(json a_returned, std::string a_returnedType)
	{
		Complete(PapyrusCallOutcome{
			.ok = true,
			.errorCode = 0,
			.error = {},
			.returned = std::move(a_returned),
			.returnedType = std::move(a_returnedType),
		});
	}

	void PapyrusCallState::CompleteFailure(int a_errorCode, std::string a_error)
	{
		Complete(PapyrusCallOutcome{
			.ok = false,
			.errorCode = a_errorCode,
			.error = std::move(a_error),
			.returned = {},
			.returnedType = {},
		});
	}

	std::optional<PapyrusCallOutcome> PapyrusCallState::WaitFor(
		std::chrono::milliseconds a_timeout)
	{
		std::unique_lock lock(mutex_);
		if (!condition_.wait_for(lock, a_timeout, [this] { return outcome_.has_value(); }))
			return std::nullopt;
		return outcome_;
	}

	bool PapyrusCallState::IsComplete() const
	{
		const std::lock_guard lock(mutex_);
		return outcome_.has_value();
	}

	void PapyrusCallState::Complete(PapyrusCallOutcome a_outcome)
	{
		{
			const std::lock_guard lock(mutex_);
			if (outcome_)
				return;
			outcome_ = std::move(a_outcome);
		}
		condition_.notify_all();
	}

	std::string FormatPapyrusType(const PapyrusType& a_type)
	{
		std::string name;
		switch (a_type.kind)
		{
			case PapyrusTypeKind::kNone:
				name = "None";
				break;
			case PapyrusTypeKind::kBool:
				name = "Bool";
				break;
			case PapyrusTypeKind::kInt:
				name = "Int";
				break;
			case PapyrusTypeKind::kFloat:
				name = "Float";
				break;
			case PapyrusTypeKind::kString:
				name = "String";
				break;
			case PapyrusTypeKind::kObject:
			case PapyrusTypeKind::kStruct:
				name = a_type.name.empty() ? "<unknown>" : a_type.name;
				break;
			case PapyrusTypeKind::kVar:
				name = "Var";
				break;
			case PapyrusTypeKind::kUnsupported:
				name = a_type.name.empty() ? "<unsupported>" : a_type.name;
				break;
		}
		if (a_type.array)
			name += "[]";
		return name;
	}

	json PapyrusTypeToJson(const PapyrusType& a_type)
	{
		std::string kind;
		switch (a_type.kind)
		{
			case PapyrusTypeKind::kNone:
				kind = "none";
				break;
			case PapyrusTypeKind::kBool:
				kind = "bool";
				break;
			case PapyrusTypeKind::kInt:
				kind = "int";
				break;
			case PapyrusTypeKind::kFloat:
				kind = "float";
				break;
			case PapyrusTypeKind::kString:
				kind = "string";
				break;
			case PapyrusTypeKind::kObject:
				kind = "object";
				break;
			case PapyrusTypeKind::kStruct:
				kind = "struct";
				break;
			case PapyrusTypeKind::kVar:
				kind = "var";
				break;
			case PapyrusTypeKind::kUnsupported:
				kind = "unsupported";
				break;
		}
		json result{
			{ "display", FormatPapyrusType(a_type) },
			{ "kind", std::move(kind) },
			{ "array", a_type.array },
		};
		if (!a_type.name.empty())
			result["name"] = a_type.name;
		return result;
	}

	void ValidatePapyrusArguments(
		const json& a_arguments, const PapyrusFunctionDefinition& a_function)
	{
		if (!a_arguments.is_array())
			throw ToolError(400, "papyrus call: 'args' must be an array");
		std::unordered_set<std::string> structPath;
		RequireSupportedType(
			a_function.returnType, a_function, "return value", true, 0, structPath);
		for (std::size_t i = 0; i < a_function.parameters.size(); ++i)
		{
			const auto& parameter = a_function.parameters[i];
			RequireSupportedType(parameter.type, a_function,
				parameter.name.empty() ? std::format("args[{}]", i) :
										 std::format("args[{}] '{}'", i, parameter.name),
				false, 0, structPath);
		}
		if (a_arguments.size() != a_function.parameters.size())
			throw ToolError(400, std::format(
									 "papyrus call: '{}.{}' requires exactly {} arguments; {} supplied",
									 a_function.script, a_function.function,
									 a_function.parameters.size(), a_arguments.size()));
		ValidationBudget budget;
		for (std::size_t i = 0; i < a_arguments.size(); ++i)
		{
			const auto& parameter = a_function.parameters[i];
			ValidateValue(a_arguments[i], parameter.type, a_function, 0, budget,
				parameter.name.empty() ? std::format("args[{}]", i) :
										 std::format("args[{}] '{}'", i, parameter.name));
		}
	}

	ToolDescriptor BuildPapyrusDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "papyrus";
		descriptor.description =
			"Discover loaded Fallout 4 Papyrus script classes, describe actual VM function/property/"
			"struct type metadata, or dispatch one permission-gated global or bound-object call. "
			"Arguments are validated against exact VM metadata before dispatch. Supported values are "
			"Bool, Int, Float, String, form-backed objects, arrays, and typed Fallout 4 structs/"
			"struct arrays with recursively validated fields and explicit typed nulls. Var, unknown "
			"fields/types, raw VM pointers, and untyped JSON inference are rejected. A call timeout "
			"is not cancellation: an accepted latent call can complete later and is never retried.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{
												{ "type", "string" },
												{ "enum", json::array({ "list", "describe", "call" }) },
												{ "default", "list" },
											} },
								{ "filter", json{
												{ "type", "string" },
												{ "maxLength", kMaxIdentifierBytes },
												{ "description", "list: case-insensitive class-name substring" },
											} },
								{ "limit", json{
											   { "type", "integer" },
											   { "minimum", 0 },
											   { "maximum", kMaxClassLimit },
											   { "default", kDefaultClassLimit },
										   } },
								{ "script", json{
												{ "type", "string" },
												{ "minLength", 1 },
												{ "maxLength", kMaxIdentifierBytes },
											} },
								{ "function", json{
												  { "type", "string" },
												  { "minLength", 1 },
												  { "maxLength", kMaxIdentifierBytes },
											  } },
								{ "self", json{
											  { "description", "call: omit for a global/static call; use 'selected' or a form selector for a bound-object call" },
											  { "anyOf", json::array({
															 json{ { "type", "string" }, { "enum", json::array({ "selected" }) } },
															 json{
																 { "type", "object" },
																 { "properties", json{
																					 { "form", json{ { "type", "string" }, { "minLength", 1 }, { "maxLength", kMaxIdentifierBytes } } },
																				 } },
																 { "required", json::array({ "form" }) },
																 { "additionalProperties", false },
															 },
														 }) },
										  } },
								{ "args", json{
											  { "type", "array" },
											  { "maxItems", kMaxArguments },
											  { "default", json::array() },
											  { "description", "call: values validated against the resolved function signature; structs use {type,fields}, objects use {form}, nulls use {type,value:null}" },
										  } },
								{ "timeoutMs", json{
												   { "type", "integer" },
												   { "minimum", 1 },
												   { "maximum", kMaxTimeoutMs },
												   { "default", kDefaultTimeoutMs },
											   } },
							} },
			{ "additionalProperties", false },
		};
		return descriptor;
	}

	void RegisterPapyrusTool(
		ToolRegistry& a_registry, bool a_allowCalls, PapyrusBackend a_backend)
	{
		auto backend = std::make_shared<PapyrusBackend>(std::move(a_backend));
		a_registry.Register(BuildPapyrusDescriptor(),
			[a_allowCalls, backend = std::move(backend)](
				const json& a_args, const ToolContext&) {
				return HandlePapyrus(a_args, a_allowCalls, *backend);
			});
	}
}

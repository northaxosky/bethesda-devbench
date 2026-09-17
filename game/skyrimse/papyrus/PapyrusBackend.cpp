#include "PapyrusBackend.h"

#include "game/skyrimse/Lifecycle.h"

#include <RE/A/Array.h>
#include <RE/B/BSAtomic.h>
#include <RE/B/BSFixedString.h>
#include <RE/B/BSTSmartPointer.h>
#include <RE/C/Console.h>
#include <RE/F/FormTypes.h>
#include <RE/I/IFunction.h>
#include <RE/I/IFunctionArguments.h>
#include <RE/I/IObjectHandlePolicy.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/O/Object.h>
#include <RE/O/ObjectTypeInfo.h>
#include <RE/P/PackUnpack.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TypeInfo.h>
#include <RE/V/Variable.h>
#include <RE/V/VirtualMachine.h>
#include <SKSE/API.h>
#include <SKSE/Interfaces.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#undef GetObject

namespace dvb::skyrimse::papyrus
{
	namespace
	{
		namespace bss = RE::BSScript;
		using Vm = bss::Internal::VirtualMachine;
		using tools::papyrus::PapyrusCallRequest;
		using tools::papyrus::PapyrusFunctionDefinition;
		using tools::papyrus::PapyrusParameter;
		using tools::papyrus::PapyrusType;
		using tools::papyrus::PapyrusTypeKind;

		constexpr auto        kMainThreadTimeout = std::chrono::milliseconds(5000);
		constexpr std::size_t kSkyrimMaxArrayElements = 128;

		std::string Text(const char* a_value)
		{
			return a_value ? std::string(a_value) : std::string{};
		}

		std::string Text(const RE::BSFixedString& a_value)
		{
			return Text(a_value.c_str());
		}

		std::string LowerAscii(std::string_view a_value)
		{
			std::string out(a_value);
			std::ranges::transform(out, out.begin(), [](unsigned char a_ch) {
				return static_cast<char>(
					a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return out;
		}

		bool EqualName(std::string_view a_left, std::string_view a_right)
		{
			return LowerAscii(a_left) == LowerAscii(a_right);
		}

		template <class F>
		auto RunOnMainThread(F&& a_function) -> std::invoke_result_t<F>
		{
			using Result = std::invoke_result_t<F>;
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks)
				throw ToolError(503, "SKSE TaskInterface is unavailable");

			auto promise = std::make_shared<std::promise<Result>>();
			auto future = promise->get_future();
			try
			{
				tasks->AddTask(
					[function = std::forward<F>(a_function), promise]() mutable {
						try
						{
							promise->set_value(function());
						}
						catch (...)
						{
							try
							{
								promise->set_exception(std::current_exception());
							}
							catch (...)
							{
							}
						}
					});
			}
			catch (const std::exception& a_exception)
			{
				throw ToolError(503, std::format(
										 "could not queue Papyrus main-thread work: {}",
										 a_exception.what()));
			}
			if (future.wait_for(kMainThreadTimeout) != std::future_status::ready)
				throw ToolError(504, "Papyrus main-thread metadata task timed out");
			return future.get();
		}

		Vm* GetVm()
		{
			auto* vm = Vm::GetSingleton();
			if (!vm)
				throw ToolError(503, "Papyrus VM is unavailable");
			return vm;
		}

		struct CallReadinessFailure
		{
			int         code = 409;
			std::string message;
		};

		std::optional<CallReadinessFailure> CheckCallReadiness()
		{
			const auto lifecycle = Lifecycle::GetSnapshot();
			if (!lifecycle.gameDataReady)
				return CallReadinessFailure{
					.code = 503,
					.message =
						"Papyrus calls require initialized game data; the call was not dispatched",
				};
			if (lifecycle.inMainMenu || !lifecycle.gameLoaded)
				return CallReadinessFailure{
					.message =
						"Papyrus calls require a loaded game outside the main menu; "
						"the call was not dispatched",
				};
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				return CallReadinessFailure{
					.message =
						"Papyrus calls are unavailable while a load is in progress; "
						"the call was not dispatched",
				};
			return std::nullopt;
		}

		PapyrusType DescribeType(const bss::TypeInfo& a_type)
		{
			PapyrusType result;
			result.array = a_type.IsArray();
			switch (a_type.GetUnmangledRawType())
			{
				case bss::TypeInfo::RawType::kNone:
				case bss::TypeInfo::RawType::kNoneArray:
					result.kind = PapyrusTypeKind::kNone;
					break;
				case bss::TypeInfo::RawType::kBool:
				case bss::TypeInfo::RawType::kBoolArray:
					result.kind = PapyrusTypeKind::kBool;
					break;
				case bss::TypeInfo::RawType::kInt:
				case bss::TypeInfo::RawType::kIntArray:
					result.kind = PapyrusTypeKind::kInt;
					break;
				case bss::TypeInfo::RawType::kFloat:
				case bss::TypeInfo::RawType::kFloatArray:
					result.kind = PapyrusTypeKind::kFloat;
					break;
				case bss::TypeInfo::RawType::kString:
				case bss::TypeInfo::RawType::kStringArray:
					result.kind = PapyrusTypeKind::kString;
					break;
				case bss::TypeInfo::RawType::kObject:
				case bss::TypeInfo::RawType::kObjectArray:
					result.kind = PapyrusTypeKind::kObject;
					if (const auto* type = a_type.GetTypeInfo())
						result.name = Text(type->GetName());
					break;
				default:
					result.kind = PapyrusTypeKind::kUnsupported;
					result.name = std::format(
						"rawType:{}", std::to_underlying(a_type.GetUnmangledRawType()));
					break;
			}
			return result;
		}

		json DescribeFunction(const bss::IFunction& a_function)
		{
			json parameters = json::array();
			for (std::uint32_t i = 0; i < a_function.GetParamCount(); ++i)
			{
				RE::BSFixedString name;
				bss::TypeInfo     type;
				a_function.GetParam(i, name, type);
				parameters.push_back(json{
					{ "name", Text(name) },
					{ "type", tools::papyrus::PapyrusTypeToJson(DescribeType(type)) },
				});
			}
			return json{
				{ "name", Text(a_function.GetName()) },
				{ "objectType", Text(a_function.GetObjectTypeName()) },
				{ "state", Text(a_function.GetStateName()) },
				{ "returnType",
					tools::papyrus::PapyrusTypeToJson(DescribeType(a_function.GetReturnType())) },
				{ "parameters", std::move(parameters) },
				{ "native", a_function.GetIsNative() },
				{ "static", a_function.GetIsStatic() },
				{ "empty", a_function.GetIsEmpty() },
				{ "callableFromTasklets", a_function.CanBeCalledFromTasklets() },
				{ "docString", Text(a_function.GetDocString()) },
				{ "source", Text(a_function.GetSourceFilename()) },
			};
		}

		bss::ObjectTypeInfo* FindClassInHierarchy(
			bss::ObjectTypeInfo* a_type, std::string_view a_name)
		{
			for (auto* type = a_type; type; type = type->GetParent())
				if (EqualName(Text(type->GetName()), a_name))
					return type;
			return nullptr;
		}

		RE::BSTSmartPointer<bss::IFunction> FindGlobalFunction(
			bss::ObjectTypeInfo& a_type, std::string_view a_name)
		{
			for (std::uint32_t i = 0; i < a_type.GetNumGlobalFuncs(); ++i)
			{
				auto function = a_type.GetGlobalFuncIter()[i].func;
				if (function && EqualName(Text(function->GetName()), a_name))
					return function;
			}
			return {};
		}

		RE::BSTSmartPointer<bss::IFunction> FindMemberFunction(
			bss::ObjectTypeInfo& a_start, std::string_view a_name,
			std::string_view a_currentState)
		{
			for (auto* type = std::addressof(a_start); type; type = type->GetParent())
			{
				if (!a_currentState.empty())
				{
					for (std::uint32_t stateIndex = 0;
						stateIndex < type->GetNumNamedStates(); ++stateIndex)
					{
						const auto& state = type->GetNamedStateIter()[stateIndex];
						if (!EqualName(Text(state.name), a_currentState))
							continue;
						for (std::uint32_t i = 0; i < state.GetNumFuncs(); ++i)
						{
							auto function = state.GetFuncIter()[i].func;
							if (function && EqualName(Text(function->GetName()), a_name))
								return function;
						}
					}
				}
				for (std::uint32_t i = 0; i < type->GetNumMemberFuncs(); ++i)
				{
					auto function = type->GetMemberFuncIter()[i].func;
					if (function && EqualName(Text(function->GetName()), a_name))
						return function;
				}
			}
			return {};
		}

		PapyrusFunctionDefinition MakeDefinition(
			const PapyrusCallRequest& a_request, const bss::IFunction& a_function)
		{
			PapyrusFunctionDefinition definition{
				.script = a_request.script,
				.function = Text(a_function.GetName()),
				.bound = a_request.self.has_value(),
				.state = Text(a_function.GetStateName()),
				.returnType = DescribeType(a_function.GetReturnType()),
				.parameters = {},
				.structs = {},
			};
			for (std::uint32_t i = 0; i < a_function.GetParamCount(); ++i)
			{
				RE::BSFixedString name;
				bss::TypeInfo     type;
				a_function.GetParam(i, name, type);
				definition.parameters.push_back(PapyrusParameter{
					.name = Text(name),
					.type = DescribeType(type),
				});
			}
			return definition;
		}

		bool HasVisibleText(std::string_view a_text)
		{
			return a_text.find_first_not_of(" \t\r\n\f\v") != std::string_view::npos;
		}

		RE::TESForm* ResolveHex(std::string_view a_text)
		{
			if (a_text.empty())
				return nullptr;
			std::uint64_t value = 0;
			const auto [end, error] =
				std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
			if (error != std::errc{} || end != a_text.data() + a_text.size() ||
				value > std::numeric_limits<RE::FormID>::max())
				return nullptr;
			return RE::TESForm::LookupByID(static_cast<RE::FormID>(value));
		}

		RE::TESForm* ResolveForm(std::string_view a_identifier)
		{
			if (a_identifier.empty())
				return nullptr;
			if (a_identifier.starts_with("0x") || a_identifier.starts_with("0X"))
				return ResolveHex(a_identifier.substr(2));
			if (auto* form = RE::TESForm::LookupByEditorID(a_identifier))
				return form;
			return ResolveHex(a_identifier);
		}

		json SerializeForm(const RE::TESForm& a_form)
		{
			const auto formId = a_form.GetFormID();
			json       out{
				{ "formId", formId },
				{ "formIdHex", std::format("0x{:08X}", formId) },
				{ "formType", std::string(RE::FormTypeToString(a_form.GetFormType())) },
				{ "name", nullptr },
				{ "editorId", nullptr },
			};
			if (const auto* name = a_form.GetName(); name && HasVisibleText(name))
				out["name"] = std::string(name);
			if (const auto* editorId = a_form.GetFormEditorID();
				editorId && HasVisibleText(editorId))
				out["editorId"] = std::string(editorId);
			return out;
		}

		RE::BSTSmartPointer<bss::Object> CreateAndBindObject(
			Vm& a_vm, RE::TESForm& a_form, bss::ObjectTypeInfo& a_type,
			RE::VMHandle a_handle)
		{
			RE::BSTSmartPointer<bss::Object> object;
			if (!a_vm.CreateObject(RE::BSFixedString(a_type.GetName()), object) || !object)
				throw ToolError(503, std::format(
										 "papyrus call: could not create '{}' VM object",
										 Text(a_type.GetName())));

			bss::BindID(object, std::addressof(a_form),
				static_cast<RE::VMTypeID>(a_form.GetFormType()));

			object.reset();
			if (!a_vm.FindBoundObject(a_handle, a_type.GetName(), object) || !object)
				throw ToolError(503, std::format(
										 "papyrus call: could not bind '{}' VM object",
										 Text(a_type.GetName())));
			return object;
		}

		struct ResolvedSelf
		{
			RE::VMHandle                             handle = 0;
			RE::BSTSmartPointer<bss::Object>         object;
			RE::BSTSmartPointer<bss::ObjectTypeInfo> actualType;
			RE::BSTSmartPointer<bss::ObjectTypeInfo> requestedTypeOwner;
			bss::ObjectTypeInfo*                     requestedType = nullptr;
			std::string                              currentState;
		};

		ResolvedSelf ResolveSelf(
			Vm& a_vm, const PapyrusCallRequest& a_request, bool a_bindNative)
		{
			if (!a_request.self)
				throw ToolError(500, "internal Papyrus target error: bound call has no self");

			RE::NiPointer<RE::TESObjectREFR> selected;
			RE::TESForm*                     form = nullptr;
			if (a_request.self->kind == tools::papyrus::PapyrusSelf::Kind::kSelected)
			{
				selected = RE::Console::GetSelectedRef();
				form = selected.get();
				if (!form)
					throw ToolError(
						404, "papyrus call: self='selected' but no console reference is selected");
			}
			else
			{
				form = ResolveForm(a_request.self->form);
				if (!form)
					throw ToolError(404, std::format(
											 "papyrus call: self form '{}' was not found",
											 a_request.self->form));
			}

			auto* policy = a_vm.GetObjectHandlePolicy();
			if (!policy)
				throw ToolError(503, "Papyrus object handle policy is unavailable");
			const auto nativeTypeId = static_cast<RE::VMTypeID>(form->GetFormType());
			const auto handle = policy->GetHandleForObject(nativeTypeId, form);
			if (handle == policy->EmptyHandle())
				throw ToolError(422, "papyrus call: target form has no VM object handle");

			ResolvedSelf result{ .handle = handle };
			if (!a_vm.GetScriptObjectType(
					RE::BSFixedString(a_request.script), result.requestedTypeOwner) ||
				!result.requestedTypeOwner)
				throw ToolError(404, std::format(
										 "unknown Papyrus script class '{}'", a_request.script));

			if (a_vm.FindBoundObject(
					handle, a_request.script.c_str(), result.object) &&
				result.object)
			{
				result.actualType.reset(result.object->GetTypeInfo());
				result.requestedType =
					FindClassInHierarchy(result.actualType.get(), a_request.script);
				result.currentState = Text(result.object->currentState);
				if (!result.requestedType)
					throw ToolError(422, std::format(
											 "papyrus call: target object is not a '{}' script target",
											 a_request.script));
				return result;
			}

			RE::VMTypeID requestedTypeId{};
			const bool   requestedBindable =
				a_vm.GetTypeIDForScriptObject(
					RE::BSFixedString(a_request.script), requestedTypeId) &&
				policy->HandleIsType(requestedTypeId, handle);

			if (!a_vm.GetScriptObjectType(nativeTypeId, result.actualType) ||
				!result.actualType)
				throw ToolError(
					422, "papyrus call: target form has no native Papyrus object type");
			result.requestedType =
				FindClassInHierarchy(result.actualType.get(), a_request.script);
			if (!requestedBindable && !result.requestedType)
				throw ToolError(422, std::format(
										 "papyrus call: form cannot target script '{}'; no existing "
										 "binding or compatible native type was found",
										 a_request.script));
			if (requestedBindable)
				result.requestedType = result.requestedTypeOwner.get();

			if (a_bindNative)
			{
				auto& bindType =
					requestedBindable ? *result.requestedTypeOwner : *result.actualType;
				result.object = CreateAndBindObject(a_vm, *form, bindType, handle);
				result.actualType.reset(result.object->GetTypeInfo());
				result.requestedType =
					FindClassInHierarchy(result.actualType.get(), a_request.script);
				if (!result.requestedType)
					throw ToolError(422, std::format(
											 "papyrus call: bound object is not compatible with '{}'",
											 a_request.script));
				result.currentState = Text(result.object->currentState);
			}
			return result;
		}

		struct ResolvedCall
		{
			RE::BSTSmartPointer<bss::ObjectTypeInfo> scriptType;
			RE::BSTSmartPointer<bss::Object>         self;
			RE::BSTSmartPointer<bss::IFunction>      function;
			PapyrusFunctionDefinition                definition;
		};

		ResolvedCall ResolveCall(
			Vm& a_vm, const PapyrusCallRequest& a_request, bool a_bindNative)
		{
			ResolvedCall result;
			if (!a_request.self)
			{
				if (!a_vm.GetScriptObjectType(
						RE::BSFixedString(a_request.script), result.scriptType) ||
					!result.scriptType)
					throw ToolError(404, std::format(
											 "unknown Papyrus script class '{}'", a_request.script));
				result.function =
					FindGlobalFunction(*result.scriptType, a_request.function);
			}
			else
			{
				auto self = ResolveSelf(a_vm, a_request, a_bindNative);
				result.scriptType = std::move(self.actualType);
				result.self = std::move(self.object);
				result.function = FindMemberFunction(
					*self.requestedType, a_request.function, self.currentState);
			}
			if (!result.function)
				throw ToolError(404, std::format(
										 "no such {} Papyrus function '{}.{}'",
										 a_request.self ? "bound-object" : "global/static",
										 a_request.script, a_request.function));
			if (result.function->GetIsStatic() == a_request.self.has_value())
				throw ToolError(422, std::format(
										 "Papyrus function '{}.{}' is {}, but the request target is {}",
										 a_request.script, a_request.function,
										 result.function->GetIsStatic() ? "global/static" : "a member",
										 a_request.self ? "bound" : "global"));
			result.definition = MakeDefinition(a_request, *result.function);
			return result;
		}

		bool IsTypedNull(const json& a_value, const PapyrusType& a_type)
		{
			return a_value.is_object() && a_value.size() == 2 &&
			       a_value.contains("type") && a_value.at("type").is_string() &&
			       a_value.contains("value") && a_value.at("value").is_null() &&
			       EqualName(a_value.at("type").get<std::string>(),
					   tools::papyrus::FormatPapyrusType(a_type));
		}

		RE::BSTSmartPointer<bss::Object> BindFormArgument(
			Vm& a_vm, RE::TESForm& a_form, bss::ObjectTypeInfo& a_expectedType)
		{
			auto* policy = a_vm.GetObjectHandlePolicy();
			if (!policy)
				throw ToolError(503, "Papyrus object handle policy is unavailable");
			const auto nativeTypeId = static_cast<RE::VMTypeID>(a_form.GetFormType());
			const auto handle = policy->GetHandleForObject(nativeTypeId, &a_form);
			if (handle == policy->EmptyHandle())
				throw ToolError(422, "papyrus call: object argument has no VM handle");

			RE::BSTSmartPointer<bss::Object> object;
			if (a_vm.FindBoundObject(handle, a_expectedType.GetName(), object) && object)
				return object;

			RE::VMTypeID expectedTypeId{};
			if (a_vm.GetTypeIDForScriptObject(a_expectedType.GetName(), expectedTypeId) &&
				policy->HandleIsType(expectedTypeId, handle))
				return CreateAndBindObject(a_vm, a_form, a_expectedType, handle);

			RE::BSTSmartPointer<bss::ObjectTypeInfo> nativeType;
			if (!a_vm.GetScriptObjectType(nativeTypeId, nativeType) || !nativeType ||
				!FindClassInHierarchy(nativeType.get(), Text(a_expectedType.GetName())))
				throw ToolError(422, std::format(
										 "papyrus call: form is not compatible with object type '{}'",
										 Text(a_expectedType.GetName())));
			return CreateAndBindObject(a_vm, a_form, *nativeType, handle);
		}

		bss::TypeInfo ArrayElementType(const bss::TypeInfo& a_type)
		{
			switch (a_type.GetUnmangledRawType())
			{
				case bss::TypeInfo::RawType::kBoolArray:
					return bss::TypeInfo(bss::TypeInfo::RawType::kBool);
				case bss::TypeInfo::RawType::kIntArray:
					return bss::TypeInfo(bss::TypeInfo::RawType::kInt);
				case bss::TypeInfo::RawType::kFloatArray:
					return bss::TypeInfo(bss::TypeInfo::RawType::kFloat);
				case bss::TypeInfo::RawType::kStringArray:
					return bss::TypeInfo(bss::TypeInfo::RawType::kString);
				case bss::TypeInfo::RawType::kObjectArray:
					if (auto* type = a_type.GetTypeInfo())
						return bss::TypeInfo(type->GetRawType());
					break;
				default:
					break;
			}
			throw ToolError(422, std::format(
									 "papyrus call: unsupported VM array type '{}'",
									 tools::papyrus::FormatPapyrusType(DescribeType(a_type))));
		}

		bss::Variable ConvertValue(
			Vm& a_vm, const json& a_value, const bss::TypeInfo& a_type,
			std::size_t a_depth)
		{
			const auto described = DescribeType(a_type);
			if (IsTypedNull(a_value, described))
				return {};
			if (a_depth > tools::papyrus::kMaxValueDepth)
				throw ToolError(400, "papyrus call: value nesting is too deep");

			if (a_type.IsArray())
			{
				if (a_value.size() > kSkyrimMaxArrayElements)
					throw ToolError(422, std::format(
											 "papyrus call: Skyrim VM arrays are limited to {} elements",
											 kSkyrimMaxArrayElements));
				const auto                      elementType = ArrayElementType(a_type);
				RE::BSTSmartPointer<bss::Array> array;
				if (!a_vm.CreateArray(
						elementType, static_cast<std::uint32_t>(a_value.size()), array) ||
					!array)
					throw ToolError(503, std::format(
											 "papyrus call: could not allocate a {}-element VM array",
											 a_value.size()));
				for (std::uint32_t i = 0; i < array->size(); ++i)
					(*array)[i] =
						ConvertValue(a_vm, a_value[i], elementType, a_depth + 1);
				bss::Variable result;
				result.SetArray(std::move(array));
				return result;
			}

			bss::Variable result;
			switch (a_type.GetUnmangledRawType())
			{
				case bss::TypeInfo::RawType::kBool:
					result.SetBool(a_value.get<bool>());
					break;
				case bss::TypeInfo::RawType::kInt:
					result.SetSInt(a_value.get<std::int32_t>());
					break;
				case bss::TypeInfo::RawType::kFloat:
					result.SetFloat(static_cast<float>(a_value.get<double>()));
					break;
				case bss::TypeInfo::RawType::kString:
					result.SetString(a_value.get_ref<const std::string&>());
					break;
				case bss::TypeInfo::RawType::kObject:
				{
					auto* form =
						ResolveForm(a_value.at("form").get_ref<const std::string&>());
					if (!form)
						throw ToolError(404, std::format(
												 "papyrus call: object form '{}' was not found",
												 a_value.at("form").get<std::string>()));
					auto* expected = a_type.GetTypeInfo();
					if (!expected)
						throw ToolError(
							422, "papyrus call: object parameter type metadata is missing");
					result.SetObject(
						BindFormArgument(a_vm, *form, *expected), a_type.GetRawType());
					break;
				}
				default:
					throw ToolError(422, std::format(
											 "papyrus call: unsupported VM parameter type '{}'",
											 tools::papyrus::FormatPapyrusType(described)));
			}
			return result;
		}

		json TypedNull(const PapyrusType& a_type)
		{
			return json{
				{ "type", tools::papyrus::FormatPapyrusType(a_type) },
				{ "value", nullptr },
			};
		}

		struct OutputBudget
		{
			std::size_t nodes = 0;
		};

		json VariableToJson(
			Vm& a_vm, const bss::Variable& a_value,
			const PapyrusType& a_declaredType, std::size_t a_depth,
			OutputBudget& a_budget)
		{
			if (++a_budget.nodes > tools::papyrus::kMaxValueNodes)
				throw ToolError(422, "papyrus result exceeds the value node limit");
			if (a_depth > tools::papyrus::kMaxValueDepth)
				throw ToolError(422, "papyrus result exceeds the value depth limit");

			const auto rawType = a_value.GetType().GetUnmangledRawType();
			if (rawType == bss::TypeInfo::RawType::kNone ||
				rawType == bss::TypeInfo::RawType::kNoneArray)
			{
				if (a_declaredType.kind == PapyrusTypeKind::kObject ||
					a_declaredType.array)
					return TypedNull(a_declaredType);
				return nullptr;
			}
			if (a_value.IsBool())
				return a_value.GetBool();
			if (a_value.IsInt())
				return a_value.GetSInt();
			if (a_value.IsFloat())
				return a_value.GetFloat();
			if (a_value.IsString())
				return std::string(a_value.GetString());
			if (a_value.IsObject())
			{
				auto object = a_value.GetObject();
				if (!object)
					return TypedNull(a_declaredType);
				const auto* type = object->GetTypeInfo();
				const auto  scriptType = type ? Text(type->GetName()) : std::string{};
				json        out{ { "scriptType", scriptType } };
				if (type &&
					FindClassInHierarchy(
						const_cast<bss::ObjectTypeInfo*>(type), "Form"))
				{
					RE::VMTypeID typeId{};
					auto*        policy = a_vm.GetObjectHandlePolicy();
					if (policy &&
						a_vm.GetTypeIDForScriptObject(
							RE::BSFixedString(scriptType), typeId))
					{
						const auto handle = object->GetHandle();
						if (policy->IsHandleObjectAvailable(handle))
						{
							auto* form = static_cast<RE::TESForm*>(
								policy->GetObjectForHandle(typeId, handle));
							if (form)
							{
								out = SerializeForm(*form);
								out["scriptType"] = scriptType;
							}
						}
					}
				}
				return out;
			}
			if (a_value.IsArray())
			{
				auto array = a_value.GetArray();
				if (!array)
					return TypedNull(a_declaredType);
				if (array->size() > tools::papyrus::kMaxArrayElements)
					throw ToolError(
						422, "papyrus result exceeds the array element limit");
				const auto elementType = DescribeType(array->type_info());
				json       out = json::array();
				for (std::uint32_t i = 0; i < array->size(); ++i)
					out.push_back(VariableToJson(
						a_vm, (*array)[i], elementType, a_depth + 1, a_budget));
				return out;
			}
			throw ToolError(422, std::format(
									 "papyrus result uses unsupported VM type '{}'",
									 tools::papyrus::FormatPapyrusType(
										 DescribeType(a_value.GetType()))));
		}

		struct NativeCallLifetime
		{
			std::shared_ptr<tools::papyrus::PapyrusCallState> state;
			RE::BSTSmartPointer<bss::IVirtualMachine>         vm;
			RE::BSTSmartPointer<bss::Object>                  self;
			std::vector<bss::Variable>                        arguments;
			PapyrusType                                       returnType;
			std::string                                       call;
		};

		class RuntimeArguments final : public bss::IFunctionArguments
		{
		public:
			explicit RuntimeArguments(
				std::shared_ptr<NativeCallLifetime> a_lifetime) :
				lifetime_(std::move(a_lifetime))
			{}

			bool operator()(RE::BSScrapArray<bss::Variable>& a_out) const override
			{
				using Size = RE::BSScrapArray<bss::Variable>::size_type;
				a_out.resize(static_cast<Size>(lifetime_->arguments.size()));
				for (Size i = 0; i < a_out.size(); ++i)
					a_out[i] = lifetime_->arguments[i];
				return true;
			}

		private:
			std::shared_ptr<NativeCallLifetime> lifetime_;
		};

		void CompleteResultOnMainThread(
			const std::shared_ptr<NativeCallLifetime>& a_lifetime,
			bss::Variable                              a_result)
		{
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks)
			{
				a_lifetime->state->CompleteFailure(
					503, "result completed but the SKSE TaskInterface is unavailable");
				return;
			}
			try
			{
				tasks->AddTask(
					[lifetime = a_lifetime, result = std::move(a_result)]() mutable {
						try
						{
							OutputBudget budget;
							auto         value = VariableToJson(
								*static_cast<Vm*>(lifetime->vm.get()), result,
								lifetime->returnType, 0, budget);
							lifetime->state->CompleteSuccess(
								std::move(value),
								tools::papyrus::FormatPapyrusType(
									lifetime->returnType));
						}
						catch (const ToolError& a_error)
						{
							lifetime->state->CompleteFailure(
								a_error.code, a_error.what());
						}
						catch (const std::exception& a_exception)
						{
							lifetime->state->CompleteFailure(
								500, a_exception.what());
						}
						catch (...)
						{
							lifetime->state->CompleteFailure(
								500,
								"Papyrus result conversion failed with an unknown exception");
						}
					});
			}
			catch (const std::exception& a_exception)
			{
				a_lifetime->state->CompleteFailure(503, std::format(
															"could not queue Papyrus result conversion: {}",
															a_exception.what()));
			}
		}

		class StackCallback final : public bss::IStackCallbackFunctor
		{
		public:
			explicit StackCallback(
				std::shared_ptr<NativeCallLifetime> a_lifetime) :
				lifetime_(std::move(a_lifetime))
			{}

			void operator()(bss::Variable a_result) override
			{
				CompleteResultOnMainThread(lifetime_, std::move(a_result));
			}

			bool CanSave() const override { return false; }

			void SetObject(
				const RE::BSTSmartPointer<bss::Object>& a_object) override
			{
				if (a_object)
					lifetime_->self = a_object;
			}

		private:
			std::shared_ptr<NativeCallLifetime> lifetime_;
		};

		bool QueueCall(
			PapyrusCallRequest                                a_request,
			std::shared_ptr<tools::papyrus::PapyrusCallState> a_state)
		{
			if (const auto failure = CheckCallReadiness())
			{
				a_state->CompleteFailure(failure->code, failure->message);
				return true;
			}

			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks)
				return false;
			try
			{
				tasks->AddTask(
					[request = std::move(a_request), state = std::move(a_state)]() mutable {
						try
						{
							if (const auto failure = CheckCallReadiness())
							{
								state->CompleteFailure(
									failure->code, failure->message);
								return;
							}
							auto* vm = GetVm();
							if (vm->IsCompletelyFrozen())
							{
								state->CompleteFailure(
									409,
									"Papyrus VM execution is completely frozen; "
									"the call was not dispatched");
								return;
							}

							auto resolved = ResolveCall(*vm, request, false);
							tools::papyrus::ValidatePapyrusArguments(
								request.arguments, resolved.definition);
							if (request.self)
								resolved = ResolveCall(*vm, request, true);
							tools::papyrus::ValidatePapyrusArguments(
								request.arguments, resolved.definition);

							auto lifetime = std::make_shared<NativeCallLifetime>();
							lifetime->state = state;
							lifetime->vm.reset(vm);
							lifetime->self = resolved.self;
							lifetime->returnType = resolved.definition.returnType;
							lifetime->call =
								std::format("{}.{}", request.script, request.function);
							lifetime->arguments.reserve(
								resolved.definition.parameters.size());
							for (std::uint32_t i = 0;
								i < resolved.function->GetParamCount(); ++i)
							{
								RE::BSFixedString name;
								bss::TypeInfo     type;
								resolved.function->GetParam(i, name, type);
								lifetime->arguments.push_back(ConvertValue(
									*vm, request.arguments[i], type, 0));
							}

							// The Skyrim VM takes ownership of IFunctionArguments on both
							// accepted and refused dispatches. The argument object retains
							// the converted Variables until the VM has copied them.
							auto*                                           arguments = new RuntimeArguments(lifetime);
							RE::BSTSmartPointer<bss::IStackCallbackFunctor> callback(
								new StackCallback(lifetime));
							const bool accepted = request.self ?
							                          vm->DispatchMethodCall(
														  resolved.self,
														  RE::BSFixedString(request.function),
														  arguments, callback) :
							                          vm->DispatchStaticCall(
														  RE::BSFixedString(request.script),
														  RE::BSFixedString(request.function),
														  arguments, callback);
							if (!accepted)
								state->CompleteFailure(
									422,
									"the VM refused the validated Papyrus dispatch");
						}
						catch (const ToolError& a_error)
						{
							state->CompleteFailure(a_error.code, a_error.what());
						}
						catch (const std::exception& a_exception)
						{
							state->CompleteFailure(500, a_exception.what());
						}
						catch (...)
						{
							state->CompleteFailure(
								500,
								"Papyrus dispatch failed with an unknown exception");
						}
					});
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		tools::papyrus::PapyrusClassList ListClasses(
			std::string_view a_filter, std::size_t a_limit)
		{
			return RunOnMainThread([filter = LowerAscii(a_filter), a_limit] {
				auto*                    vm = GetVm();
				std::vector<std::string> matches;
				{
					RE::BSSpinLockGuard lock(vm->typeInfoLock);
					matches.reserve(std::min(
						a_limit,
						static_cast<std::size_t>(vm->objectTypeMap.size())));
					for (const auto& entry : vm->objectTypeMap)
					{
						auto name = Text(entry.first);
						if (!name.empty() &&
							(filter.empty() || LowerAscii(name).contains(filter)))
							matches.push_back(std::move(name));
					}
				}
				std::ranges::sort(matches);
				const auto total = matches.size();
				if (matches.size() > a_limit)
					matches.resize(a_limit);
				return tools::papyrus::PapyrusClassList{
					.total = total,
					.scripts = std::move(matches),
				};
			});
		}

		json DescribeScript(std::string_view a_script)
		{
			return RunOnMainThread([script = std::string(a_script)] {
				auto*                                    vm = GetVm();
				RE::BSTSmartPointer<bss::ObjectTypeInfo> type;
				if (!vm->GetScriptObjectType(RE::BSFixedString(script), type) || !type)
					throw ToolError(404, std::format(
											 "unknown Papyrus script class '{}'", script));

				json globalFunctions = json::array();
				for (std::uint32_t i = 0; i < type->GetNumGlobalFuncs(); ++i)
					if (const auto function = type->GetGlobalFuncIter()[i].func)
						globalFunctions.push_back(DescribeFunction(*function));

				json memberFunctions = json::array();
				for (std::uint32_t i = 0; i < type->GetNumMemberFuncs(); ++i)
					if (const auto function = type->GetMemberFuncIter()[i].func)
						memberFunctions.push_back(DescribeFunction(*function));

				json states = json::array();
				for (std::uint32_t stateIndex = 0;
					stateIndex < type->GetNumNamedStates(); ++stateIndex)
				{
					const auto& state = type->GetNamedStateIter()[stateIndex];
					json        functions = json::array();
					for (std::uint32_t i = 0; i < state.GetNumFuncs(); ++i)
						if (const auto function = state.GetFuncIter()[i].func)
							functions.push_back(DescribeFunction(*function));
					states.push_back(json{
						{ "name", Text(state.name) },
						{ "functions", std::move(functions) },
					});
				}

				json properties = json::array();
				for (std::uint32_t i = 0; i < type->GetNumProperties(); ++i)
				{
					const auto& property = type->GetPropertyIter()[i];
					properties.push_back(json{
						{ "name", Text(property.name) },
						{ "type",
							tools::papyrus::PapyrusTypeToJson(
								DescribeType(property.info.type)) },
						{ "readable", static_cast<bool>(property.info.getFunction) },
						{ "writable", static_cast<bool>(property.info.setFunction) },
						{ "docString", Text(property.info.docString) },
					});
				}

				return json{
					{ "name", Text(type->GetName()) },
					{ "parent",
						type->GetParent() ?
							json(Text(type->GetParent()->GetName())) :
							json(nullptr) },
					{ "docString", Text(type->docString) },
					{ "globalFunctions", std::move(globalFunctions) },
					{ "memberFunctions", std::move(memberFunctions) },
					{ "states", std::move(states) },
					{ "properties", std::move(properties) },
					{ "structs", json::array() },
				};
			});
		}

		PapyrusFunctionDefinition ResolveFunction(
			const PapyrusCallRequest& a_request)
		{
			return RunOnMainThread([request = a_request] {
				return ResolveCall(*GetVm(), request, false).definition;
			});
		}
	}

	tools::papyrus::PapyrusBackend MakePapyrusBackend()
	{
		return {
			.listClasses = &ListClasses,
			.describeScript = &DescribeScript,
			.resolveFunction = &ResolveFunction,
			.queueCall = &QueueCall,
		};
	}
}

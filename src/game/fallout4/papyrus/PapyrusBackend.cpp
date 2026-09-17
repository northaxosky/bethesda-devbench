#include "PapyrusBackend.h"

#include "game/fallout4/Lifecycle.h"
#include "game/fallout4/inspection/Form.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace dvb::fallout4::papyrus
{
	namespace
	{
		namespace bss = RE::BSScript;
		using Vm = bss::Internal::VirtualMachine;
		using tools::papyrus::PapyrusCallRequest;
		using tools::papyrus::PapyrusFunctionDefinition;
		using tools::papyrus::PapyrusParameter;
		using tools::papyrus::PapyrusStructDefinition;
		using tools::papyrus::PapyrusStructField;
		using tools::papyrus::PapyrusType;
		using tools::papyrus::PapyrusTypeKind;

		constexpr auto kMainThreadTimeout = std::chrono::milliseconds(5000);

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
				return static_cast<char>(a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
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
			auto* tasks = F4SE::GetTaskInterface();
			if (!tasks)
				throw ToolError(503, "F4SE TaskInterface is unavailable");

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
							{}
						}
					});
			}
			catch (const std::exception& a_exception)
			{
				throw ToolError(503, std::format(
										 "could not queue Papyrus main-thread work: {}", a_exception.what()));
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
					.message = "Papyrus calls require initialized game data; the call was not dispatched",
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

		PapyrusType DescribeType(bss::TypeInfo a_type)
		{
			PapyrusType result;
			result.array = a_type.IsArray();
			if (result.array)
				a_type.SetArray(false);

			switch (a_type.GetRawType())
			{
				case bss::TypeInfo::RawType::kNone:
					result.kind = PapyrusTypeKind::kNone;
					break;
				case bss::TypeInfo::RawType::kBool:
					result.kind = PapyrusTypeKind::kBool;
					break;
				case bss::TypeInfo::RawType::kInt:
					result.kind = PapyrusTypeKind::kInt;
					break;
				case bss::TypeInfo::RawType::kFloat:
					result.kind = PapyrusTypeKind::kFloat;
					break;
				case bss::TypeInfo::RawType::kString:
					result.kind = PapyrusTypeKind::kString;
					break;
				case bss::TypeInfo::RawType::kObject:
					result.kind = PapyrusTypeKind::kObject;
					if (const auto* type = a_type.GetObjectTypeInfo())
						result.name = Text(type->GetName());
					break;
				case bss::TypeInfo::RawType::kStruct:
					result.kind = PapyrusTypeKind::kStruct;
					if (const auto* type = a_type.GetStructTypeInfo())
						result.name = Text(type->GetName());
					break;
				case bss::TypeInfo::RawType::kVar:
					result.kind = PapyrusTypeKind::kVar;
					break;
				default:
					result.kind = PapyrusTypeKind::kUnsupported;
					result.name = std::format(
						"rawType:{}", static_cast<std::uint32_t>(a_type.GetRawType()));
					break;
			}
			return result;
		}

		struct OrderedStructField
		{
			std::uint32_t index = 0;
			std::string   name;
		};

		std::vector<OrderedStructField> StructFields(const bss::StructTypeInfo& a_type)
		{
			std::vector<OrderedStructField> fields;
			fields.reserve(a_type.varNameIndexMap.size());
			for (const auto& entry : a_type.varNameIndexMap)
			{
				if (entry.second < a_type.variables.size())
					fields.push_back({ entry.second, Text(entry.first) });
			}
			std::ranges::sort(fields, {}, &OrderedStructField::index);
			return fields;
		}

		void CollectStruct(
			bss::TypeInfo a_type, std::vector<PapyrusStructDefinition>& a_definitions,
			std::unordered_set<std::string>& a_seen, std::size_t a_depth)
		{
			if (a_type.IsArray())
				a_type.SetArray(false);
			if (a_type.GetRawType() != bss::TypeInfo::RawType::kStruct)
				return;
			auto* structType = a_type.GetStructTypeInfo();
			if (!structType)
				return;
			const auto name = Text(structType->GetName());
			const auto key = LowerAscii(name);
			if (!a_seen.insert(key).second)
				return;

			PapyrusStructDefinition definition{ .name = name, .fields = {} };
			if (a_depth < tools::papyrus::kMaxValueDepth)
			{
				for (const auto& field : StructFields(*structType))
				{
					const auto& nativeField = structType->variables[field.index];
					definition.fields.push_back(PapyrusStructField{
						.name = field.name,
						.type = DescribeType(nativeField.varType),
					});
				}
			}
			a_definitions.push_back(definition);
			if (a_depth >= tools::papyrus::kMaxValueDepth)
				return;
			for (const auto& field : StructFields(*structType))
				CollectStruct(structType->variables[field.index].varType, a_definitions, a_seen, a_depth + 1);
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
				{ "returnType", tools::papyrus::PapyrusTypeToJson(
									DescribeType(a_function.GetReturnType())) },
				{ "parameters", std::move(parameters) },
				{ "native", a_function.GetIsNative() },
				{ "static", a_function.GetIsStatic() },
				{ "empty", a_function.GetIsEmpty() },
				{ "callableFromTasklets", a_function.CanBeCalledFromTasklets() },
				{ "docString", Text(a_function.GetDocString()) },
				{ "source", Text(a_function.GetSourceFilename()) },
			};
		}

		json DescribeStruct(const bss::StructTypeInfo& a_type)
		{
			json fields = json::array();
			for (const auto& field : StructFields(a_type))
			{
				const auto& nativeField = a_type.variables[field.index];
				fields.push_back(json{
					{ "name", field.name },
					{ "type", tools::papyrus::PapyrusTypeToJson(
								  DescribeType(nativeField.varType)) },
					{ "const", nativeField.isConst },
					{ "docString", Text(nativeField.docString) },
				});
			}
			return json{
				{ "name", Text(a_type.GetName()) },
				{ "fields", std::move(fields) },
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
			std::unordered_set<std::string> seenStructs;
			CollectStruct(a_function.GetReturnType(), definition.structs, seenStructs, 0);
			for (std::uint32_t i = 0; i < a_function.GetParamCount(); ++i)
			{
				RE::BSFixedString name;
				bss::TypeInfo     type;
				a_function.GetParam(i, name, type);
				definition.parameters.push_back(PapyrusParameter{
					.name = Text(name),
					.type = DescribeType(type),
				});
				CollectStruct(type, definition.structs, seenStructs, 0);
			}
			return definition;
		}

		struct ResolvedSelf
		{
			std::size_t                              handle = 0;
			RE::BSTSmartPointer<bss::Object>         object;
			RE::BSTSmartPointer<bss::ObjectTypeInfo> actualType;
			bss::ObjectTypeInfo*                     requestedType = nullptr;
			std::string                              currentState;
		};

		bool CreateNativeObject(
			Vm& a_vm, const RE::BSFixedString& a_typeName,
			RE::BSTSmartPointer<bss::Object>& a_object)
		{
			constexpr REL::Version kSupportedRuntime{ 1, 11, 240, 0 };
			if (!REX::FModule::IsRuntimeAE() ||
				REX::FModule::GetExecutingModule().GetFileVersion() != kSupportedRuntime)
				throw ToolError(
					503, "native Papyrus object creation requires Fallout 4 AE 1.11.240");

			using CreateObject = bool (*)(
				Vm*, const RE::BSFixedString&, RE::BSTSmartPointer<bss::Object>&);
			// ID 2315144 is the no-properties creator and performs post-create initialization.
			static REL::Relocation<CreateObject> createObject{
				REL::ID(2315144)
			};

			constexpr std::size_t                  kCreateObjectSlot = 23;
			constexpr std::size_t                  kCreateObjectSize = 75;
			constexpr std::array<std::uint8_t, 51> kEntryBytes{
				0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
				0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x10, 0x48, 0x8B, 0xF1, 0x48, 0x83,
				0xC1, 0x10, 0x49, 0x8B, 0xF8, 0x4D, 0x8B, 0xC8, 0x45, 0x33, 0xC0, 0xFF,
				0x50, 0x48, 0x0F, 0xB6, 0xD8, 0x84, 0xC0, 0x74, 0x0B, 0x48, 0x8B, 0xD7,
				0x48, 0x8B, 0xCE
			};
			constexpr std::array<std::uint8_t, 19> kExitBytes{
				0x48, 0x8B, 0x74, 0x24, 0x38, 0x0F, 0xB6, 0xC3, 0x48, 0x8B,
				0x5C, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3
			};

			const auto        address = createObject.address();
			const auto* const vtable =
				*reinterpret_cast<const std::uintptr_t* const*>(std::addressof(a_vm));
			DWORD64     imageBase = 0;
			const auto* function = RtlLookupFunctionEntry(address, &imageBase, nullptr);
			const auto* code = reinterpret_cast<const std::uint8_t*>(address);
			if (!vtable || vtable[kCreateObjectSlot] != address || !function ||
				imageBase + function->BeginAddress != address ||
				function->EndAddress - function->BeginAddress != kCreateObjectSize ||
				!std::ranges::equal(
					kEntryBytes, std::span(code, kEntryBytes.size())) ||
				code[kEntryBytes.size()] != 0xE8 ||
				!std::ranges::equal(
					kExitBytes, std::span(code + 56, kExitBytes.size())))
				throw ToolError(
					503, "native Papyrus object creator failed executable validation");

			return createObject(std::addressof(a_vm), a_typeName, a_object);
		}

		RE::BSTSmartPointer<bss::Object> FindOrBindNativeObject(
			Vm& a_vm, std::size_t a_handle, bss::ObjectTypeInfo& a_nativeType)
		{
			RE::BSTSmartPointer<bss::Object> object;
			const auto                       nativeName = Text(a_nativeType.GetName());
			a_vm.FindBoundObject(
				a_handle, nativeName.c_str(), false, object, false);
			if (object)
				return object;

			if (!CreateNativeObject(a_vm, a_nativeType.name, object) || !object)
				throw ToolError(503, std::format(
										 "papyrus call: could not create native '{}' VM object",
										 nativeName));

			a_vm.GetObjectBindPolicy().BindObject(object, a_handle);

			object.reset();
			a_vm.FindBoundObject(
				a_handle, nativeName.c_str(), false, object, false);
			if (!object)
				throw ToolError(503, std::format(
										 "papyrus call: could not bind native '{}' VM object",
										 nativeName));
			REX::INFO(
				"devbench: Papyrus bound native VM object '{}'", nativeName);
			return object;
		}

		ResolvedSelf ResolveSelf(
			Vm& a_vm, const PapyrusCallRequest& a_request, bool a_bindNative)
		{
			if (!a_request.self)
				throw ToolError(500, "internal Papyrus target error: bound call has no self");

			RE::NiPointer<RE::TESObjectREFR> selected;
			RE::TESForm*                     form = nullptr;
			if (a_request.self->kind == tools::papyrus::PapyrusSelf::Kind::kSelected)
			{
				selected = RE::Console::GetCurrentPickREFR().get();
				form = selected.get();
				if (!form)
					throw ToolError(404,
						"papyrus call: self='selected' but no console reference is selected");
			}
			else
			{
				form = inspection::ResolveForm(a_request.self->form);
				if (!form)
					throw ToolError(404, std::format(
											 "papyrus call: self form '{}' was not found",
											 a_request.self->form));
			}

			auto&      policy = a_vm.GetObjectHandlePolicy();
			const auto nativeTypeId = static_cast<std::uint32_t>(form->GetFormType());
			const auto handle = policy.GetHandleForObject(nativeTypeId, form);
			if (handle == policy.EmptyHandle())
				throw ToolError(422, "papyrus call: target form has no VM object handle");

			ResolvedSelf result{
				.handle = handle,
				.object = {},
				.actualType = {},
				.requestedType = nullptr,
				.currentState = {},
			};
			a_vm.FindBoundObject(
				handle, a_request.script.c_str(), false, result.object, false);
			if (result.object)
			{
				result.actualType = result.object->type;
				result.requestedType =
					FindClassInHierarchy(result.actualType.get(), a_request.script);
				result.currentState = Text(result.object->currentState);
				if (!result.requestedType)
					throw ToolError(422, std::format(
											 "papyrus call: selected object is not a '{}' script target",
											 a_request.script));
				return result;
			}

			if (!a_vm.GetScriptObjectType(nativeTypeId, result.actualType) || !result.actualType)
				throw ToolError(422, "papyrus call: target form has no native Papyrus object type");
			result.requestedType =
				FindClassInHierarchy(result.actualType.get(), a_request.script);
			if (!result.requestedType)
				throw ToolError(422, std::format(
										 "papyrus call: form cannot target script '{}'; no existing "
										 "binding or compatible native type was found",
										 a_request.script));
			if (a_bindNative)
			{
				result.object =
					FindOrBindNativeObject(a_vm, handle, *result.actualType);
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
						RE::BSFixedString(a_request.script.c_str()), result.scriptType) ||
					!result.scriptType)
					throw ToolError(404, std::format(
											 "unknown Papyrus script class '{}'", a_request.script));
				result.function = FindGlobalFunction(*result.scriptType, a_request.function);
			}
			else
			{
				auto self = ResolveSelf(a_vm, a_request, a_bindNative);
				result.scriptType = self.actualType;
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
			Vm& a_vm, RE::TESForm& a_form, const bss::ObjectTypeInfo& a_expectedType)
		{
			auto&      policy = a_vm.GetObjectHandlePolicy();
			const auto nativeTypeId = static_cast<std::uint32_t>(a_form.GetFormType());
			const auto handle = policy.GetHandleForObject(nativeTypeId, std::addressof(a_form));
			if (handle == policy.EmptyHandle())
				throw ToolError(422, "papyrus call: object argument has no VM handle");

			RE::BSTSmartPointer<bss::Object> object;
			a_vm.FindBoundObject(
				handle, a_expectedType.GetName(), false, object, false);
			if (object)
				return object;

			RE::BSTSmartPointer<bss::ObjectTypeInfo> nativeType;
			if (!a_vm.GetScriptObjectType(nativeTypeId, nativeType) || !nativeType ||
				!FindClassInHierarchy(nativeType.get(), Text(a_expectedType.GetName())))
				throw ToolError(422, std::format(
										 "papyrus call: form is not compatible with object type '{}'",
										 Text(a_expectedType.GetName())));

			return FindOrBindNativeObject(a_vm, handle, *nativeType);
		}

		const bss::StructTypeInfo::StructVar* FindStructField(
			const bss::StructTypeInfo& a_type, std::string_view a_name,
			std::uint32_t& a_index, std::string& a_actualName)
		{
			for (const auto& entry : a_type.varNameIndexMap)
			{
				if (EqualName(Text(entry.first), a_name) &&
					entry.second < a_type.variables.size())
				{
					a_index = entry.second;
					a_actualName = Text(entry.first);
					return std::addressof(a_type.variables[entry.second]);
				}
			}
			return nullptr;
		}

		bss::Variable ConvertValue(
			Vm& a_vm, const json& a_value, bss::TypeInfo a_type, std::size_t a_depth)
		{
			const auto described = DescribeType(a_type);
			if (IsTypedNull(a_value, described))
				return {};
			if (a_depth > tools::papyrus::kMaxValueDepth)
				throw ToolError(400, "papyrus call: value nesting is too deep");

			if (a_type.IsArray())
			{
				auto elementType = a_type;
				elementType.SetArray(false);
				RE::BSTSmartPointer<bss::Array> array;
				if (!a_vm.CreateArray(
						elementType, static_cast<std::uint32_t>(a_value.size()), array) ||
					!array)
					throw ToolError(503, "papyrus call: could not allocate VM array");
				for (std::uint32_t i = 0; i < array->size(); ++i)
					(*array)[i] = ConvertValue(a_vm, a_value[i], elementType, a_depth + 1);
				bss::Variable result;
				result = std::move(array);
				return result;
			}

			bss::Variable result;
			switch (a_type.GetRawType())
			{
				case bss::TypeInfo::RawType::kBool:
					result = a_value.get<bool>();
					break;
				case bss::TypeInfo::RawType::kInt:
					result = a_value.get<std::int32_t>();
					break;
				case bss::TypeInfo::RawType::kFloat:
					result = static_cast<float>(a_value.get<double>());
					break;
				case bss::TypeInfo::RawType::kString:
					result = RE::BSFixedString(a_value.get<std::string>().c_str());
					break;
				case bss::TypeInfo::RawType::kObject:
				{
					auto* form = inspection::ResolveForm(a_value.at("form").get<std::string>());
					if (!form)
						throw ToolError(404, std::format(
												 "papyrus call: object form '{}' was not found",
												 a_value.at("form").get<std::string>()));
					auto* expected = a_type.GetObjectTypeInfo();
					if (!expected)
						throw ToolError(422, "papyrus call: object parameter type metadata is missing");
					result = BindFormArgument(a_vm, *form, *expected);
					break;
				}
				case bss::TypeInfo::RawType::kStruct:
				{
					auto* structType = a_type.GetStructTypeInfo();
					if (!structType)
						throw ToolError(422, "papyrus call: struct parameter type metadata is missing");
					RE::BSTSmartPointer<bss::Struct> value;
					if (!a_vm.CreateStruct(structType->name, value) || !value)
						throw ToolError(503, std::format(
												 "papyrus call: could not allocate struct '{}'",
												 Text(structType->GetName())));
					for (const auto& [name, fieldJson] : a_value.at("fields").items())
					{
						std::uint32_t index = 0;
						std::string   actualName;
						const auto*   field =
							FindStructField(*structType, name, index, actualName);
						if (!field)
							throw ToolError(400, std::format(
													 "papyrus call: unknown field '{}' for struct '{}'",
													 name, Text(structType->GetName())));
						value->variables[index] =
							ConvertValue(a_vm, fieldJson, field->varType, a_depth + 1);
					}
					result = std::move(value);
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
			Vm& a_vm, const bss::Variable& a_value, const PapyrusType& a_declaredType,
			std::size_t a_depth, OutputBudget& a_budget)
		{
			if (++a_budget.nodes > tools::papyrus::kMaxValueNodes)
				throw ToolError(422, "papyrus result exceeds the value node limit");
			if (a_depth > tools::papyrus::kMaxValueDepth)
				throw ToolError(422, "papyrus result exceeds the value depth limit");

			if (a_value.GetType().GetRawType() == bss::TypeInfo::RawType::kNone)
			{
				if (a_declaredType.kind == PapyrusTypeKind::kObject ||
					a_declaredType.kind == PapyrusTypeKind::kStruct ||
					a_declaredType.array)
					return TypedNull(a_declaredType);
				return nullptr;
			}
			if (a_value.is<bool>())
				return bss::get<bool>(a_value);
			if (a_value.is<std::int32_t>())
				return bss::get<std::int32_t>(a_value);
			if (a_value.is<float>())
				return bss::get<float>(a_value);
			if (a_value.is<RE::BSFixedString>())
				return Text(bss::get<RE::BSFixedString>(a_value));
			if (a_value.is<bss::Object>())
			{
				auto object = bss::get<bss::Object>(a_value);
				if (!object)
					return TypedNull(a_declaredType);
				const auto* type = object->GetTypeInfo();
				const auto  scriptType = type ? Text(type->GetName()) : std::string{};
				json        out{ { "scriptType", scriptType } };
				if (type && FindClassInHierarchy(const_cast<bss::ObjectTypeInfo*>(type), "Form"))
				{
					std::uint32_t handleType = 0;
					auto&         policy = a_vm.GetObjectHandlePolicy();
					const auto    handle = object->GetHandle();
					// A latent call can finish after a load. Resolve the retained VM
					// object back to a live form only on this main-thread task, and only
					// while its handle policy still reports the native object available.
					if (policy.IsHandleObjectAvailable(handle) &&
						policy.GetHandleType(handle, handleType))
					{
						auto* form = static_cast<RE::TESForm*>(
							policy.GetObjectForHandle(handleType, handle));
						if (form)
						{
							out = inspection::SerializeForm(form);
							out["scriptType"] = scriptType;
						}
					}
				}
				return out;
			}
			if (a_value.is<bss::Array>())
			{
				auto array = bss::get<bss::Array>(a_value);
				if (!array)
					return TypedNull(a_declaredType);
				if (array->size() > tools::papyrus::kMaxArrayElements)
					throw ToolError(422, "papyrus result exceeds the array element limit");
				std::vector<bss::Variable> elements;
				bss::TypeInfo              elementType;
				{
					RE::BSAutoLock lock(array->elementsLock);
					elementType = array->elementType;
					elements.assign(array->begin(), array->end());
				}
				auto expectedElement = DescribeType(elementType);
				json out = json::array();
				for (const auto& element : elements)
					out.push_back(VariableToJson(
						a_vm, element, expectedElement, a_depth + 1, a_budget));
				return out;
			}
			if (a_value.is<bss::Struct>())
			{
				auto value = bss::get<bss::Struct>(a_value);
				if (!value || !value->type)
					return TypedNull(a_declaredType);
				auto                       fields = StructFields(*value->type);
				std::vector<bss::Variable> values;
				{
					RE::BSAutoLock lock(value->structLock);
					values.reserve(fields.size());
					for (const auto& field : fields)
						values.push_back(value->variables[field.index]);
				}
				json outFields = json::object();
				for (std::size_t i = 0; i < fields.size(); ++i)
				{
					const auto& nativeField = value->type->variables[fields[i].index];
					outFields[fields[i].name] = VariableToJson(
						a_vm, values[i], DescribeType(nativeField.varType),
						a_depth + 1, a_budget);
				}
				return json{
					{ "type", Text(value->type->GetName()) },
					{ "fields", std::move(outFields) },
				};
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

		void CompleteResultOnMainThread(
			const std::shared_ptr<NativeCallLifetime>& a_lifetime, bss::Variable a_result)
		{
			auto* tasks = F4SE::GetTaskInterface();
			if (!tasks)
			{
				REX::ERROR(
					"devbench: Papyrus {} completed but result conversion could not be queued",
					a_lifetime->call);
				a_lifetime->state->CompleteFailure(
					503, "result completed but the F4SE TaskInterface is unavailable");
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
							REX::INFO(
								"devbench: Papyrus {} result conversion completed",
								lifetime->call);
							lifetime->state->CompleteSuccess(
								std::move(value),
								tools::papyrus::FormatPapyrusType(lifetime->returnType));
						}
						catch (const ToolError& a_error)
						{
							REX::ERROR(
								"devbench: Papyrus {} result conversion failed: {}",
								lifetime->call, a_error.what());
							lifetime->state->CompleteFailure(a_error.code, a_error.what());
						}
						catch (const std::exception& a_exception)
						{
							REX::ERROR(
								"devbench: Papyrus {} result conversion failed: {}",
								lifetime->call, a_exception.what());
							lifetime->state->CompleteFailure(500, a_exception.what());
						}
					});
			}
			catch (const std::exception& a_exception)
			{
				REX::ERROR(
					"devbench: Papyrus {} result conversion queue failed: {}",
					a_lifetime->call, a_exception.what());
				a_lifetime->state->CompleteFailure(503, std::format(
															"could not queue Papyrus result conversion: {}", a_exception.what()));
			}
		}

		class StackCallback final : public bss::IStackCallbackFunctor
		{
		public:
			explicit StackCallback(std::shared_ptr<NativeCallLifetime> a_lifetime) :
				lifetime_(std::move(a_lifetime))
			{}

			void CallQueued() override
			{
				REX::INFO("devbench: Papyrus {} callback queued", lifetime_->call);
			}

			void CallCanceled() override
			{
				REX::WARN("devbench: Papyrus {} callback canceled", lifetime_->call);
				lifetime_->state->CompleteFailure(409, "the VM canceled the Papyrus call");
			}

			void StartMultiDispatch() override {}
			void EndMultiDispatch() override {}

			void operator()(bss::Variable a_result) override
			{
				REX::INFO(
					"devbench: Papyrus {} callback invoked with VM type {}",
					lifetime_->call,
					static_cast<std::uint32_t>(a_result.GetType().GetRawType()));
				CompleteResultOnMainThread(lifetime_, std::move(a_result));
			}

			bool CanSave() override { return false; }

		private:
			std::shared_ptr<NativeCallLifetime> lifetime_;
		};

		bool QueueCall(
			PapyrusCallRequest                                a_request,
			std::shared_ptr<tools::papyrus::PapyrusCallState> a_state)
		{
			// Check before queueing so a request submitted in the main menu or during
			// loading cannot sit in F4SE's task queue and dispatch after the game loads.
			if (const auto failure = CheckCallReadiness())
			{
				a_state->CompleteFailure(failure->code, failure->message);
				return true;
			}

			auto* tasks = F4SE::GetTaskInterface();
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
								state->CompleteFailure(failure->code, failure->message);
								return;
							}
							auto* vm = GetVm();
							if (vm->IsCompletelyFrozen())
							{
								state->CompleteFailure(
									409,
									"Papyrus VM execution is completely frozen; the call was not dispatched");
								return;
							}

							// Resolve and validate against current VM metadata before any
							// binding or dispatch. This second validation closes the race
							// between the listener-thread metadata snapshot and this task.
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
							lifetime->arguments.reserve(resolved.definition.parameters.size());
							for (std::uint32_t i = 0;
								i < resolved.function->GetParamCount(); ++i)
							{
								RE::BSFixedString name;
								bss::TypeInfo     type;
								resolved.function->GetParam(i, name, type);
								lifetime->arguments.push_back(
									ConvertValue(*vm, request.arguments[i], type, 0));
							}

							RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<bss::Variable>&)>
								arguments = [lifetime](RE::BSScrapArray<bss::Variable>& a_out) {
									using Size = RE::BSScrapArray<bss::Variable>::size_type;
									a_out.resize(static_cast<Size>(lifetime->arguments.size()));
									for (Size i = 0; i < a_out.size(); ++i)
										a_out[i] = lifetime->arguments[i];
									return true;
								};
							RE::BSTSmartPointer<bss::IStackCallbackFunctor> callback(
								new StackCallback(lifetime));
							bool accepted = false;
							if (!request.self)
							{
								accepted = vm->DispatchStaticCall(
									RE::BSFixedString(request.script.c_str()),
									RE::BSFixedString(request.function.c_str()),
									arguments, callback);
							}
							else
							{
								accepted = vm->DispatchMethodCall(
									resolved.self,
									RE::BSFixedString(request.function.c_str()),
									arguments, callback);
							}
							if (!accepted)
							{
								REX::WARN(
									"devbench: Papyrus {} dispatch refused",
									lifetime->call);
								state->CompleteFailure(
									422, "the VM refused the validated Papyrus dispatch");
							}
							else
							{
								REX::INFO(
									"devbench: Papyrus {} dispatch accepted",
									lifetime->call);
							}
						}
						catch (const ToolError& a_error)
						{
							state->CompleteFailure(a_error.code, a_error.what());
						}
						catch (const std::exception& a_exception)
						{
							state->CompleteFailure(500, a_exception.what());
						}
					});
				return true;
			}
			catch (const std::exception& a_exception)
			{
				REX::ERROR("devbench: could not queue Papyrus call: {}", a_exception.what());
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
					RE::BSAutoLock lock(vm->typeInfoLock);
					matches.reserve(std::min(
						a_limit, static_cast<std::size_t>(vm->objectTypeMap.size())));
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
				if (!vm->GetScriptObjectType(RE::BSFixedString(script.c_str()), type) || !type)
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
						{ "type", tools::papyrus::PapyrusTypeToJson(
									  DescribeType(property.info.type)) },
						{ "readable", static_cast<bool>(property.info.getFunction) },
						{ "writable", static_cast<bool>(property.info.setFunction) },
						{ "docString", Text(property.info.docString) },
					});
				}

				std::vector<RE::BSTSmartPointer<bss::StructTypeInfo>> structTypes;
				{
					RE::BSAutoLock lock(vm->typeInfoLock);
					for (const auto& entry : vm->structTypeMap)
						if (entry.second &&
							entry.second->containingObjTypeInfo.get() == type.get())
							structTypes.push_back(entry.second);
				}
				std::ranges::sort(structTypes, {}, [](const auto& a_value) {
					return LowerAscii(Text(a_value->GetName()));
				});
				json structs = json::array();
				for (const auto& structType : structTypes)
					structs.push_back(DescribeStruct(*structType));

				return json{
					{ "name", Text(type->GetName()) },
					{ "parent", type->GetParent() ?
									json(Text(type->GetParent()->GetName())) :
									json(nullptr) },
					{ "docString", Text(type->docString) },
					{ "const", static_cast<bool>(type->isConst) },
					{ "globalFunctions", std::move(globalFunctions) },
					{ "memberFunctions", std::move(memberFunctions) },
					{ "states", std::move(states) },
					{ "properties", std::move(properties) },
					{ "structs", std::move(structs) },
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

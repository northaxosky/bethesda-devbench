#include "test_framework.h"

#include "tools/papyrus/PapyrusTool.h"

using dvb::json;
using dvb::ToolContext;
using dvb::ToolRegistry;
using namespace dvb::tools::papyrus;

namespace
{
	PapyrusType Scalar(PapyrusTypeKind a_kind, std::string a_name = {})
	{
		return PapyrusType{ .kind = a_kind, .name = std::move(a_name) };
	}

	PapyrusType Array(PapyrusTypeKind a_kind, std::string a_name = {})
	{
		return PapyrusType{ .kind = a_kind, .name = std::move(a_name), .array = true };
	}

	PapyrusFunctionDefinition StructFunction()
	{
		return PapyrusFunctionDefinition{
			.script = "WorkshopObject",
			.function = "ApplyData",
			.bound = true,
			.state = {},
			.returnType = Scalar(PapyrusTypeKind::kBool),
			.parameters = {
				{ "data", Scalar(PapyrusTypeKind::kStruct, "WorkshopObject#PlacementData") },
				{ "history", Array(PapyrusTypeKind::kStruct, "WorkshopObject#PlacementData") },
			},
			.structs = {
				{
					.name = "WorkshopObject#PlacementData",
					.fields = {
						{ "Enabled", Scalar(PapyrusTypeKind::kBool) },
						{ "Owner", Scalar(PapyrusTypeKind::kObject, "ObjectReference") },
						{ "Offsets", Array(PapyrusTypeKind::kFloat) },
						{ "Nested", Scalar(PapyrusTypeKind::kStruct, "WorkshopObject#NestedData") },
					},
				},
				{
					.name = "WorkshopObject#NestedData",
					.fields = {
						{ "Count", Scalar(PapyrusTypeKind::kInt) },
					},
				},
			},
		};
	}

	struct FakePapyrus
	{
		int                               listCalls = 0;
		int                               describeCalls = 0;
		int                               resolveCalls = 0;
		int                               queueCalls = 0;
		bool                              acceptQueue = true;
		bool                              completeSynchronously = true;
		PapyrusFunctionDefinition         definition = StructFunction();
		std::shared_ptr<PapyrusCallState> lateState;
		PapyrusCallRequest                queuedRequest;

		PapyrusBackend Backend()
		{
			return {
				.listClasses = [this](std::string_view a_filter, std::size_t a_limit) {
					++listCalls;
					CHECK(a_filter == "work");
					CHECK(a_limit == 1);
					return PapyrusClassList{
						.total = 2,
						.scripts = { "WorkshopObject" },
					}; },
				.describeScript = [this](std::string_view a_script) {
					++describeCalls;
					return json{
						{ "name", a_script },
						{ "parent", "ObjectReference" },
						{ "globalFunctions", json::array() },
						{ "memberFunctions", json::array() },
						{ "properties", json::array() },
						{ "structs", json::array() },
					}; },
				.resolveFunction = [this](const PapyrusCallRequest&) {
					++resolveCalls;
					return definition; },
				.queueCall = [this](
								 PapyrusCallRequest                a_request,
								 std::shared_ptr<PapyrusCallState> a_state) {
					++queueCalls;
					queuedRequest = std::move(a_request);
					if (!acceptQueue)
						return false;
					if (completeSynchronously)
						a_state->CompleteSuccess(true, "Bool");
					else
						lateState = std::move(a_state);
					return true; },
			};
		}
	};

	dvb::ToolResult Invoke(ToolRegistry& a_registry, json a_args)
	{
		return a_registry.Invoke("papyrus", a_args, ToolContext{});
	}

	json ValidStruct(std::int32_t a_count)
	{
		return json{
			{ "type", "WorkshopObject#PlacementData" },
			{ "fields", json{
							{ "Enabled", true },
							{ "Owner", json{ { "form", "0x14" } } },
							{ "Offsets", json::array({ 1.0, 2 }) },
							{ "Nested", json{
											{ "type", "WorkshopObject#NestedData" },
											{ "fields", json{ { "Count", a_count } } },
										} },
						} },
		};
	}
}

TEST_CASE("papyrus descriptor documents Fallout 4 typed calls and stable actions")
{
	const auto descriptor = BuildPapyrusDescriptor();
	CHECK(descriptor.name == "papyrus");
	CHECK(!descriptor.readOnly);
	CHECK(descriptor.description.find("typed native structs") != std::string::npos);
	CHECK(descriptor.description.find("timeout is not cancellation") != std::string::npos);
	CHECK(descriptor.inputSchema.at("properties").at("action").at("enum") ==
		  json::array({ "list", "describe", "call" }));
	CHECK(descriptor.inputSchema.at("properties").at("self").contains("anyOf"));
}

TEST_CASE("papyrus list and describe remain read-only while calls require permission")
{
	FakePapyrus  fake;
	ToolRegistry registry;
	RegisterPapyrusTool(registry, false, fake.Backend());

	const auto listed = Invoke(registry, json{
											 { "action", "list" },
											 { "filter", "work" },
											 { "limit", 1 },
										 });
	CHECK(listed.ok);
	CHECK(listed.value.at("total") == 2);
	CHECK(listed.value.at("truncated") == true);

	const auto described = Invoke(registry, json{
												{ "action", "describe" },
												{ "script", "WorkshopObject" },
											});
	CHECK(described.ok);
	CHECK(described.value.at("action") == "describe");

	const auto denied = Invoke(registry, json{
											 { "action", "call" },
											 { "script", "WorkshopObject" },
											 { "function", "ApplyData" },
											 { "self", "selected" },
											 { "args", json::array({ ValidStruct(1), json::array() }) },
										 });
	CHECK(denied.errorCode == 403);
	CHECK(fake.resolveCalls == 0);
	CHECK(fake.queueCalls == 0);
}

TEST_CASE("papyrus validates typed structs arrays and nulls before queueing")
{
	FakePapyrus  fake;
	ToolRegistry registry;
	RegisterPapyrusTool(registry, true, fake.Backend());

	const json typedNull{
		{ "type", "WorkshopObject#PlacementData" },
		{ "value", nullptr },
	};
	const auto valid = Invoke(registry, json{
											{ "action", "call" },
											{ "script", "WorkshopObject" },
											{ "function", "ApplyData" },
											{ "self", json{ { "form", "WorkshopWorkbench" } } },
											{ "args", json::array({
														  ValidStruct(7),
														  json::array({ ValidStruct(8), typedNull }),
													  }) },
										});
	CHECK(valid.ok);
	CHECK(valid.value.at("returned") == true);
	CHECK(valid.value.at("returnedType") == "Bool");
	CHECK(fake.queueCalls == 1);
	CHECK(fake.queuedRequest.self.has_value());
	CHECK(fake.queuedRequest.self->kind == PapyrusSelf::Kind::kForm);

	auto badField = ValidStruct(1);
	badField.at("fields")["Unknown"] = 4;
	const auto unknownField = Invoke(registry, json{
												   { "action", "call" },
												   { "script", "WorkshopObject" },
												   { "function", "ApplyData" },
												   { "self", "selected" },
												   { "args", json::array({ badField, json::array() }) },
											   });
	CHECK(unknownField.errorCode == 400);
	CHECK(fake.queueCalls == 1);

	auto wrongType = ValidStruct(1);
	wrongType["type"] = "Other#PlacementData";
	const auto mismatched = Invoke(registry, json{
												 { "action", "call" },
												 { "script", "WorkshopObject" },
												 { "function", "ApplyData" },
												 { "self", "selected" },
												 { "args", json::array({ wrongType, json::array() }) },
											 });
	CHECK(mismatched.errorCode == 400);
	CHECK(fake.queueCalls == 1);

	const auto wrongCount = Invoke(registry, json{
												 { "action", "call" },
												 { "script", "WorkshopObject" },
												 { "function", "ApplyData" },
												 { "self", "selected" },
												 { "args", json::array({ ValidStruct(1) }) },
											 });
	CHECK(wrongCount.errorCode == 400);
	CHECK(fake.queueCalls == 1);

	auto rawHandle = ValidStruct(1);
	rawHandle.at("fields")["Owner"] = json{ { "handle", 1234 } };
	const auto arbitraryHandle = Invoke(registry, json{
													  { "action", "call" },
													  { "script", "WorkshopObject" },
													  { "function", "ApplyData" },
													  { "self", "selected" },
													  { "args", json::array({ rawHandle, json::array() }) },
												  });
	CHECK(arbitraryHandle.errorCode == 400);
	CHECK(fake.queueCalls == 1);

	json tooMany = json::array();
	for (std::size_t i = 0; i <= kMaxArrayElements; ++i)
		tooMany.push_back(typedNull);
	const auto oversized = Invoke(registry, json{
												{ "action", "call" },
												{ "script", "WorkshopObject" },
												{ "function", "ApplyData" },
												{ "self", "selected" },
												{ "args", json::array({ ValidStruct(1), std::move(tooMany) }) },
											});
	CHECK(oversized.errorCode == 400);
	CHECK(fake.queueCalls == 1);
}

TEST_CASE("papyrus rejects unsupported Var instead of guessing a VM type")
{
	FakePapyrus fake;
	fake.definition = PapyrusFunctionDefinition{
		.script = "Utility",
		.function = "Consume",
		.bound = false,
		.state = {},
		.returnType = Scalar(PapyrusTypeKind::kNone),
		.parameters = { { "value", Scalar(PapyrusTypeKind::kVar) } },
		.structs = {},
	};
	ToolRegistry registry;
	RegisterPapyrusTool(registry, true, fake.Backend());

	const auto result = Invoke(registry, json{
											 { "action", "call" },
											 { "script", "Utility" },
											 { "function", "Consume" },
											 { "args", json::array({ json{ { "form", "0x14" } } }) },
										 });
	CHECK(result.errorCode == 422);
	CHECK(fake.queueCalls == 0);
}

TEST_CASE("papyrus reports failed native queueing without dispatch success")
{
	FakePapyrus fake;
	fake.acceptQueue = false;
	fake.definition = PapyrusFunctionDefinition{
		.script = "Utility",
		.function = "RandomInt",
		.bound = false,
		.state = {},
		.returnType = Scalar(PapyrusTypeKind::kInt),
		.parameters = {
			{ "minimum", Scalar(PapyrusTypeKind::kInt) },
			{ "maximum", Scalar(PapyrusTypeKind::kInt) },
		},
		.structs = {},
	};
	ToolRegistry registry;
	RegisterPapyrusTool(registry, true, fake.Backend());

	const auto result = Invoke(registry, json{
											 { "action", "call" },
											 { "script", "Utility" },
											 { "function", "RandomInt" },
											 { "args", json::array({ 1, 10 }) },
										 });
	CHECK(result.errorCode == 503);
	CHECK(fake.queueCalls == 1);
}

TEST_CASE("papyrus late completion state survives caller timeout")
{
	FakePapyrus fake;
	fake.completeSynchronously = false;
	fake.definition = PapyrusFunctionDefinition{
		.script = "Utility",
		.function = "Wait",
		.bound = false,
		.state = {},
		.returnType = Scalar(PapyrusTypeKind::kNone),
		.parameters = {},
		.structs = {},
	};
	ToolRegistry registry;
	RegisterPapyrusTool(registry, true, fake.Backend());

	const auto result = Invoke(registry, json{
											 { "action", "call" },
											 { "script", "Utility" },
											 { "function", "Wait" },
											 { "args", json::array() },
											 { "timeoutMs", 1 },
										 });
	CHECK(result.errorCode == 504);
	CHECK(result.errorMessage.find("timeout is not cancellation") != std::string::npos);
	CHECK(fake.lateState != nullptr);
	CHECK(!fake.lateState->IsComplete());

	fake.lateState->CompleteSuccess(nullptr, "None");
	CHECK(fake.lateState->IsComplete());
	const auto late = fake.lateState->WaitFor(std::chrono::milliseconds(0));
	CHECK(late.has_value());
	CHECK(late->ok);
	CHECK(late->returnedType == "None");
}

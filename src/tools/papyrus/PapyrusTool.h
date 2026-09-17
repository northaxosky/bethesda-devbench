#pragma once

#include "Json.h"
#include "ToolRegistry.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::tools::papyrus
{
	inline constexpr std::size_t kDefaultClassLimit = 200;
	inline constexpr std::size_t kMaxClassLimit = 500;
	inline constexpr std::size_t kMaxArguments = 32;
	inline constexpr std::size_t kMaxArrayElements = 256;
	inline constexpr std::size_t kMaxValueNodes = 2048;
	inline constexpr std::size_t kMaxValueDepth = 8;
	inline constexpr std::size_t kMaxStringBytes = 16384;
	inline constexpr std::size_t kMaxIdentifierBytes = 256;
	inline constexpr int         kDefaultTimeoutMs = 3000;
	inline constexpr int         kMaxTimeoutMs = 30000;

	enum class PapyrusTypeKind
	{
		kNone,
		kBool,
		kInt,
		kFloat,
		kString,
		kObject,
		kStruct,
		kVar,
		kUnsupported,
	};

	struct PapyrusType
	{
		PapyrusTypeKind kind = PapyrusTypeKind::kUnsupported;
		std::string     name;
		bool            array = false;
	};

	struct PapyrusParameter
	{
		std::string name;
		PapyrusType type;
	};

	struct PapyrusStructField
	{
		std::string name;
		PapyrusType type;
	};

	struct PapyrusStructDefinition
	{
		std::string                     name;
		std::vector<PapyrusStructField> fields;
	};

	struct PapyrusFunctionDefinition
	{
		std::string                          script;
		std::string                          function;
		bool                                 bound = false;
		std::string                          state;
		PapyrusType                          returnType;
		std::vector<PapyrusParameter>        parameters;
		std::vector<PapyrusStructDefinition> structs;
	};

	struct PapyrusSelf
	{
		enum class Kind
		{
			kSelected,
			kForm,
		};

		Kind        kind = Kind::kForm;
		std::string form;
	};

	struct PapyrusCallRequest
	{
		std::string                script;
		std::string                function;
		std::optional<PapyrusSelf> self;
		json                       arguments = json::array();
	};

	struct PapyrusClassList
	{
		std::size_t              total = 0;
		std::vector<std::string> scripts;
	};

	struct PapyrusCallOutcome
	{
		bool        ok = false;
		int         errorCode = 500;
		std::string error;
		json        returned;
		std::string returnedType;
	};

	class PapyrusCallState
	{
	public:
		void CompleteSuccess(json a_returned, std::string a_returnedType);
		void CompleteFailure(int a_errorCode, std::string a_error);

		[[nodiscard]] std::optional<PapyrusCallOutcome> WaitFor(std::chrono::milliseconds a_timeout);
		[[nodiscard]] bool                              IsComplete() const;

	private:
		void Complete(PapyrusCallOutcome a_outcome);

		mutable std::mutex                mutex_;
		std::condition_variable           condition_;
		std::optional<PapyrusCallOutcome> outcome_;
	};

	struct PapyrusBackend
	{
		std::function<PapyrusClassList(std::string_view, std::size_t)>      listClasses;
		std::function<json(std::string_view)>                               describeScript;
		std::function<PapyrusFunctionDefinition(const PapyrusCallRequest&)> resolveFunction;

		// Queue exactly one game-thread dispatch. Returning false means the request was
		// not accepted. Once accepted, the backend owns a shared reference to a_state and
		// may complete it after the caller's timeout; timeout is not cancellation.
		std::function<bool(PapyrusCallRequest, std::shared_ptr<PapyrusCallState>)> queueCall;
	};

	[[nodiscard]] std::string FormatPapyrusType(const PapyrusType& a_type);
	[[nodiscard]] json        PapyrusTypeToJson(const PapyrusType& a_type);

	// Validates the wire values against an exact VM metadata snapshot. This does not
	// infer a Papyrus type from arbitrary JSON: every value is checked against its
	// declared parameter/field type, and Var/unknown types are rejected.
	void ValidatePapyrusArguments(
		const json& a_arguments, const PapyrusFunctionDefinition& a_function);

	ToolDescriptor BuildPapyrusDescriptor();
	void           RegisterPapyrusTool(
		ToolRegistry& a_registry, bool a_allowCalls, PapyrusBackend a_backend);
}

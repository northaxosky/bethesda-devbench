#include "tools/scenario/ScenarioTool.h"

#include "ScenarioPolicy.h"
#include "tools/scenario/ValuePredicate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace dvb::tools::scenario
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Milliseconds = std::chrono::milliseconds;

		constexpr int kCancelledStatus = 499;

		enum class StepKind
		{
			kTool,
			kWait,
			kWaitFor,
			kWaitUntil,
			kAssert,
			kPose,
		};

		struct ActionCorrelation
		{
			std::uint64_t                eventCursor = 0;
			std::optional<std::uint64_t> gameOperationId;
		};

		struct ModalAcceptSpec
		{
			std::string matchBody;
			std::size_t buttonIndex = 0;
		};

		std::optional<ModalAcceptSpec> ParseAcceptModal(const json& a_step);

		struct WaitForSpec
		{
			std::string topic;
			json        match = json::object();
			std::optional<ModalAcceptSpec> acceptModal;
		};

		struct SceneSpec
		{
			bool          interior = false;
			std::uint32_t worldspaceFormId = 0;
			std::uint32_t cellFormId = 0;
			std::string   worldspace;
			std::string   cell;
			bool          soft = false;
		};

		struct Step
		{
			StepKind                      kind = StepKind::kWait;
			std::string                   label;
			std::string                   tool;
			json                          args = json::object();
			Milliseconds                  wait{ 0 };
			WaitForSpec                   event;
			std::optional<ValuePredicate> predicate;
			std::string                   condition;
			std::optional<SceneSpec>      scene;
			std::array<double, 5>         pose{};
			Milliseconds                  timeout{ 0 };
			Milliseconds                  poll{ 1 };
		};

		struct Plan
		{
			std::vector<Step> steps;
			std::size_t       repeat = 1;
			bool              continueOnError = false;
			bool              async = false;
		};

		enum class RunPhase
		{
			kPending,
			kRunning,
			kDone,
		};

		struct RunRecord
		{
			std::uint64_t           id = 0;
			Plan                    plan;
			ToolContext             context;
			ScenarioRunOptions      options;
			std::string             owner;
			std::atomic_bool        cancelRequested = false;
			bool                    ownsInput = false;
			bool                    cameraFreecamAcquired = false;
			bool                    cameraReleaseUncertain = false;
			std::optional<json>     cameraBefore;
			std::optional<json>     cameraSnapshotError;
			mutable std::mutex      mutex;
			std::condition_variable doneCv;
			RunPhase                phase = RunPhase::kPending;
			std::size_t             currentIndex = 0;
			std::size_t             currentRepetition = 0;
			json                    result;
		};

		struct StepOutcome
		{
			json transcript;
			bool failed = false;
			bool cancelled = false;
			bool runTimedOut = false;
		};

		enum class StopReason
		{
			kNone,
			kCancelled,
			kTimedOut,
		};

		std::string KindName(StepKind a_kind)
		{
			switch (a_kind)
			{
				case StepKind::kTool:
					return "tool";
				case StepKind::kWait:
					return "wait";
				case StepKind::kWaitFor:
					return "waitFor";
				case StepKind::kWaitUntil:
					return "waitUntil";
				case StepKind::kAssert:
					return "assert";
				case StepKind::kPose:
					return "pose";
			}
			return "unknown";
		}

		void RejectUnknownKeys(
			const json&                       a_value,
			const std::set<std::string_view>& a_allowed,
			std::string_view                  a_subject)
		{
			for (const auto& [key, unused] : a_value.items())
			{
				(void)unused;
				if (!a_allowed.contains(key))
					throw ToolError(
						400,
						std::format("unknown {} field '{}'", a_subject, key));
			}
		}

		std::uint64_t ReadPositiveId(const json& a_args)
		{
			const auto it = a_args.find("runId");
			if (it == a_args.end())
				throw ToolError(400, "action requires 'runId'");
			if ((!it->is_number_integer() && !it->is_number_unsigned()) ||
				(!it->is_number_unsigned() && it->get<std::int64_t>() <= 0))
				throw ToolError(400, "'runId' must be a positive integer");
			const auto value = it->is_number_unsigned() ?
			                       it->get<std::uint64_t>() :
			                       static_cast<std::uint64_t>(it->get<std::int64_t>());
			if (value == 0)
				throw ToolError(400, "'runId' must be a positive integer");
			return value;
		}

		std::size_t ReadRepeat(const json& a_args)
		{
			const auto it = a_args.find("repeat");
			if (it == a_args.end())
				return 1;
			if (!it->is_number_integer() && !it->is_number_unsigned())
				throw ToolError(400, "'repeat' must be an integer from 1 through 1000");
			if (!it->is_number_unsigned() && it->get<std::int64_t>() < 1)
				throw ToolError(400, "'repeat' must be an integer from 1 through 1000");
			const auto repeat = it->is_number_unsigned() ?
			                        it->get<std::uint64_t>() :
			                        static_cast<std::uint64_t>(it->get<std::int64_t>());
			if (repeat < 1 || repeat > 1000)
				throw ToolError(400, "'repeat' must be an integer from 1 through 1000");
			return static_cast<std::size_t>(repeat);
		}

		Milliseconds ReadMilliseconds(
			const json&  a_object,
			const char*  a_key,
			Milliseconds a_default,
			Milliseconds a_max,
			bool         a_allowZero)
		{
			const auto it = a_object.find(a_key);
			if (it == a_object.end())
				return a_default;
			if (!it->is_number_integer() && !it->is_number_unsigned())
				throw ToolError(400, std::format("'{}' must be an integer", a_key));
			if (!it->is_number_unsigned() && it->get<std::int64_t>() < 0)
				throw ToolError(400, std::format("'{}' must not be negative", a_key));
			const auto value = it->is_number_unsigned() ?
			                       it->get<std::uint64_t>() :
			                       static_cast<std::uint64_t>(it->get<std::int64_t>());
			if (!a_allowZero && value == 0)
				throw ToolError(400, std::format("'{}' must be greater than zero", a_key));
			if (value > static_cast<std::uint64_t>(a_max.count()))
				throw ToolError(
					400,
					std::format("'{}' is capped at {} ms", a_key, a_max.count()));
			return Milliseconds(value);
		}

		bool IsLifecycleName(std::string_view a_name)
		{
			constexpr std::array<std::string_view, 13> names{
				"newGame",
				"preLoadGame",
				"postLoadGame",
				"deleteGame",
				"postLoad",
				"postPostLoad",
				"preSaveGame",
				"postSaveGame",
				"inputLoaded",
				"gameLoaded",
				"gameDataReady",
				"mainMenuEntered",
				"mainMenuExited",
			};
			return std::ranges::find(names, a_name) != names.end() ||
			       a_name == "loadingMenuOpened" || a_name == "loadingMenuClosed";
		}

		WaitForSpec ParseWaitFor(const json& a_step)
		{
			const json& value = a_step.at("waitFor");
			if (value.is_string())
			{
				const std::string shorthand = value.get<std::string>();
				if (IsLifecycleName(shorthand))
					return { "lifecycle", json{ { "event", shorthand } } };
				if (shorthand == "menuOpened" || shorthand == "menuClosed")
				{
					const auto name = a_step.find("name");
					if (name == a_step.end() || !name->is_string() ||
						name->get_ref<const std::string&>().empty())
						throw ToolError(
							400,
							std::format(
								"waitFor '{}' requires a non-empty string 'name'",
								shorthand));
					return {
						"menu",
						json{
							{ "name", name->get<std::string>() },
							{ "opening", shorthand == "menuOpened" },
						},
						ParseAcceptModal(a_step),
					};
				}
				throw ToolError(
					400,
					std::format("unknown waitFor shorthand '{}'", shorthand));
			}
			if (!value.is_object())
				throw ToolError(
					400,
					"'waitFor' must be a string shorthand or {topic, match}");
			RejectUnknownKeys(value, { "topic", "match" }, "waitFor");
			const auto topic = value.find("topic");
			if (topic == value.end() || !topic->is_string() ||
				topic->get_ref<const std::string&>().empty())
				throw ToolError(400, "waitFor object requires a non-empty string 'topic'");
			json match = json::object();
			if (const auto it = value.find("match"); it != value.end())
			{
				if (!it->is_object())
					throw ToolError(400, "waitFor 'match' must be an object");
				match = *it;
			}
			return {
				topic->get<std::string>(),
				std::move(match),
				ParseAcceptModal(a_step),
			};
		}

		bool PayloadMatches(const json& a_payload, const json& a_match)
		{
			if (!a_payload.is_object())
				return a_match.empty();
			for (const auto& [key, value] : a_match.items())
			{
				const auto it = a_payload.find(key);
				if (it == a_payload.end() || *it != value)
					return false;
			}
			return true;
		}

		bool ContainsCaseInsensitive(std::string_view a_value, std::string_view a_needle)
		{
			if (a_needle.empty())
				return true;
			const auto Lower = [](unsigned char a_ch) {
				return static_cast<char>(
					a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			};
			for (std::size_t start = 0; start + a_needle.size() <= a_value.size(); ++start)
			{
				std::size_t index = 0;
				for (; index < a_needle.size() &&
					   Lower(static_cast<unsigned char>(a_value[start + index])) ==
						   Lower(static_cast<unsigned char>(a_needle[index]));
					 ++index)
				{}
				if (index == a_needle.size())
					return true;
			}
			return false;
		}

		std::uint32_t ReadFormId(const json& a_value, std::string_view a_name)
		{
			if (!a_value.is_number_integer() && !a_value.is_number_unsigned())
				throw ToolError(400, std::format("'{}' must be a 32-bit integer", a_name));
			if (!a_value.is_number_unsigned() && a_value.get<std::int64_t>() < 0)
				throw ToolError(400, std::format("'{}' must not be negative", a_name));
			const auto value = a_value.is_number_unsigned() ?
			                       a_value.get<std::uint64_t>() :
			                       static_cast<std::uint64_t>(a_value.get<std::int64_t>());
			if (value > std::numeric_limits<std::uint32_t>::max())
				throw ToolError(400, std::format("'{}' exceeds a 32-bit FormID", a_name));
			return static_cast<std::uint32_t>(value);
		}

		std::optional<ModalAcceptSpec> ParseAcceptModal(const json& a_step)
		{
			const auto it = a_step.find("acceptModal");
			if (it == a_step.end())
				return std::nullopt;
			if (!it->is_object())
				throw ToolError(400, "'acceptModal' must be an object");
			RejectUnknownKeys(*it, { "matchBody", "buttonIndex" }, "acceptModal");
			const auto body = it->find("matchBody");
			if (body == it->end() || !body->is_string() ||
				body->get_ref<const std::string&>().empty())
				throw ToolError(400, "'acceptModal.matchBody' must be a non-empty string");
			const auto matchBody = body->get<std::string>();
			if (!ContainsCaseInsensitive(matchBody, "no longer present") ||
				!ContainsCaseInsensitive("no longer present", matchBody))
				throw ToolError(
					400,
					"automatic modal acceptance is restricted to matchBody='no longer present'");

			std::size_t buttonIndex = 0;
			if (const auto index = it->find("buttonIndex"); index != it->end())
			{
				if ((!index->is_number_integer() && !index->is_number_unsigned()) ||
					(!index->is_number_unsigned() && index->get<std::int64_t>() < 0))
					throw ToolError(400, "'acceptModal.buttonIndex' must be a non-negative integer");
				const auto value = index->is_number_unsigned() ?
				                       index->get<std::uint64_t>() :
				                       static_cast<std::uint64_t>(index->get<std::int64_t>());
				if (value > 31)
					throw ToolError(400, "'acceptModal.buttonIndex' must not exceed 31");
				buttonIndex = static_cast<std::size_t>(value);
			}
			return ModalAcceptSpec{ .matchBody = matchBody, .buttonIndex = buttonIndex };
		}

		ValuePredicate ParseCondition(
			const json&         a_value,
			const ToolRegistry& a_registry,
			bool                a_allowPlayerLoaded)
		{
			if (a_value.is_string())
			{
				const std::string shorthand = a_value.get<std::string>();
				if (a_allowPlayerLoaded && shorthand == "playerLoaded")
					return ParseValuePredicate(
						json{
							{ "tool", "inspect" },
							{ "args", json{ { "kind", "state" } } },
							{ "path", "/playerLoaded" },
							{ "eq", true },
						},
						a_registry);
				throw ToolError(
					400,
					std::format(
						"unsupported condition shorthand '{}' (supported: playerLoaded)",
						shorthand));
			}
			return ParseValuePredicate(a_value, a_registry);
		}

		bool IsStateCondition(std::string_view a_condition)
		{
			return a_condition == "playerLoaded" || a_condition == "noModal" ||
			       a_condition == "noMenu" || a_condition == "noBlockingMenu";
		}

		SceneSpec ParseSceneSpec(const json& a_value)
		{
			RejectUnknownKeys(
				a_value,
				{ "assert", "interior", "worldspaceFormID", "cellFormID", "worldspace",
					"cell", "soft", "timeoutMs", "pollMs", "label", "atMs" },
				"scene assertion");
			SceneSpec scene;
			const auto interior = a_value.find("interior");
			if (interior == a_value.end() || !interior->is_boolean())
				throw ToolError(400, "scene assertion requires boolean 'interior'");
			scene.interior = interior->get<bool>();
			if (const auto it = a_value.find("worldspaceFormID"); it != a_value.end())
				scene.worldspaceFormId = ReadFormId(*it, "worldspaceFormID");
			if (const auto it = a_value.find("cellFormID"); it != a_value.end())
				scene.cellFormId = ReadFormId(*it, "cellFormID");
			if (const auto it = a_value.find("worldspace"); it != a_value.end())
			{
				if (!it->is_string())
					throw ToolError(400, "'worldspace' must be a string");
				scene.worldspace = it->get<std::string>();
			}
			if (const auto it = a_value.find("cell"); it != a_value.end())
			{
				if (!it->is_string())
					throw ToolError(400, "'cell' must be a string");
				scene.cell = it->get<std::string>();
			}
			if (const auto it = a_value.find("soft"); it != a_value.end())
			{
				if (!it->is_boolean())
					throw ToolError(400, "'soft' must be a boolean");
				scene.soft = it->get<bool>();
			}
			if ((scene.interior && scene.cellFormId == 0) ||
				(!scene.interior && scene.worldspaceFormId == 0))
				throw ToolError(
					400,
					scene.interior ?
						"interior scene assertion requires non-zero 'cellFormID'" :
						"exterior scene assertion requires non-zero 'worldspaceFormID'");
			return scene;
		}

		Step ParseStep(
			const json&           a_value,
			const ToolRegistry&   a_registry,
			const ScenarioConfig& a_config)
		{
			if (!a_value.is_object())
				throw ToolError(400, "each scenario step must be an object");

			std::size_t discriminators = 0;
			for (const auto key : { "tool", "waitFor", "waitUntil", "assert", "pose" })
				discriminators += a_value.contains(key) ? 1 : 0;
			if (a_value.contains("wait") && !a_value.contains("pose"))
				++discriminators;
			if (discriminators != 1)
				throw ToolError(
					400,
					"each scenario step requires exactly one of tool|wait|waitFor|waitUntil|assert|pose");

			Step step;
			if (const auto label = a_value.find("label"); label != a_value.end())
			{
				if (!label->is_string())
					throw ToolError(400, "step 'label' must be a string");
				step.label = label->get<std::string>();
			}

			if (a_value.contains("tool"))
			{
				RejectUnknownKeys(a_value, { "tool", "args", "label", "atMs" }, "tool step");
				const auto& tool = a_value.at("tool");
				if (!tool.is_string() || tool.get_ref<const std::string&>().empty())
					throw ToolError(400, "tool step requires a non-empty string 'tool'");
				step.kind = StepKind::kTool;
				step.tool = tool.get<std::string>();
				if (step.tool == "scenario")
					throw ToolError(
						400,
						"nested scenario calls are not allowed");
				if (!a_registry.Has(step.tool))
					throw ToolError(
						404,
						std::format("unknown scenario step tool '{}'", step.tool));
				if (const auto args = a_value.find("args"); args != a_value.end())
				{
					if (!args->is_object())
						throw ToolError(400, "tool step 'args' must be an object");
					step.args = *args;
				}
				return step;
			}

			if (a_value.contains("pose"))
			{
				RejectUnknownKeys(a_value, { "pose", "wait", "label", "atMs" }, "pose step");
				const auto& pose = a_value.at("pose");
				if (!pose.is_array() || pose.size() != step.pose.size())
					throw ToolError(400, "pose step needs exactly [x, y, z, yawDeg, pitchDeg]");
				for (std::size_t index = 0; index < step.pose.size(); ++index)
				{
					if (!pose[index].is_number())
						throw ToolError(400, "pose values must be numeric");
					const auto value = pose[index].get<double>();
					if (!std::isfinite(value))
						throw ToolError(400, "pose values must be finite");
					step.pose[index] = value;
				}
				step.kind = StepKind::kPose;
				step.wait = ReadMilliseconds(
					a_value, "wait", Milliseconds(0), a_config.maxWait, true);
				return step;
			}

			if (a_value.contains("wait"))
			{
				RejectUnknownKeys(a_value, { "wait", "label", "atMs" }, "wait step");
				step.kind = StepKind::kWait;
				step.wait = ReadMilliseconds(
					a_value, "wait", Milliseconds(0), a_config.maxWait, true);
				return step;
			}

			if (a_value.contains("waitFor"))
			{
				RejectUnknownKeys(
					a_value,
					{ "waitFor", "name", "timeoutMs", "pollMs", "acceptModal", "label",
						"atMs" },
					"waitFor step");
				step.kind = StepKind::kWaitFor;
				step.event = ParseWaitFor(a_value);
				step.timeout = ReadMilliseconds(
					a_value,
					"timeoutMs",
					a_config.defaultWaitForTimeout,
					a_config.maxStepTimeout,
					false);
				step.poll = ReadMilliseconds(
					a_value,
					"pollMs",
					a_config.defaultWaitForPoll,
					a_config.maxPoll,
					false);
				if (step.poll > step.timeout)
					throw ToolError(400, "'pollMs' must not exceed 'timeoutMs'");
				return step;
			}

			if (a_value.contains("waitUntil"))
			{
				RejectUnknownKeys(
					a_value,
					{ "waitUntil", "timeoutMs", "pollMs", "label", "atMs" },
					"waitUntil step");
				step.kind = StepKind::kWaitUntil;
				if (a_value.at("waitUntil").is_string())
				{
					step.condition = a_value.at("waitUntil").get<std::string>();
					if (!IsStateCondition(step.condition))
						throw ToolError(
							400,
							std::format(
								"unsupported waitUntil shorthand '{}' "
								"(playerLoaded|noModal|noMenu|noBlockingMenu)",
								step.condition));
				}
				else
					step.predicate =
						ParseCondition(a_value.at("waitUntil"), a_registry, true);
				step.timeout = ReadMilliseconds(
					a_value,
					"timeoutMs",
					a_config.defaultWaitUntilTimeout,
					a_config.maxStepTimeout,
					false);
				step.poll = ReadMilliseconds(
					a_value,
					"pollMs",
					a_config.defaultWaitUntilPoll,
					a_config.maxPoll,
					false);
				if (step.poll > step.timeout)
					throw ToolError(400, "'pollMs' must not exceed 'timeoutMs'");
				return step;
			}

			step.kind = StepKind::kAssert;
			if (a_value.at("assert").is_string())
			{
				step.condition = a_value.at("assert").get<std::string>();
				if (step.condition == "scene")
				{
					step.scene = ParseSceneSpec(a_value);
					step.timeout = ReadMilliseconds(
						a_value,
						"timeoutMs",
						Milliseconds(10000),
						a_config.maxStepTimeout,
						false);
					step.poll = ReadMilliseconds(
						a_value,
						"pollMs",
						a_config.defaultWaitUntilPoll,
						a_config.maxPoll,
						false);
					if (step.poll > step.timeout)
						throw ToolError(400, "'pollMs' must not exceed 'timeoutMs'");
				}
				else if (step.condition == "noBlockingMenu")
					RejectUnknownKeys(
						a_value, { "assert", "label", "atMs" }, "assert step");
				else
					throw ToolError(
						400,
						std::format(
							"unsupported assert shorthand '{}' (scene|noBlockingMenu)",
							step.condition));
			}
			else
			{
				RejectUnknownKeys(a_value, { "assert", "label", "atMs" }, "assert step");
				step.predicate = ParseCondition(a_value.at("assert"), a_registry, false);
			}
			return step;
		}

		Plan ParsePlan(
			const json&           a_args,
			const ToolRegistry&   a_registry,
			const ScenarioConfig& a_config)
		{
			RejectUnknownKeys(
				a_args,
				{ "action", "steps", "repeat", "continueOnError", "async" },
				"scenario run");
			const auto steps = a_args.find("steps");
			if (steps == a_args.end() || !steps->is_array())
				throw ToolError(400, "action='run' requires a 'steps' array");

			Plan plan;
			plan.repeat = ReadRepeat(a_args);
			if (const auto it = a_args.find("continueOnError"); it != a_args.end())
			{
				if (!it->is_boolean())
					throw ToolError(400, "'continueOnError' must be a boolean");
				plan.continueOnError = it->get<bool>();
			}
			if (const auto it = a_args.find("async"); it != a_args.end())
			{
				if (!it->is_boolean())
					throw ToolError(400, "'async' must be a boolean");
				plan.async = it->get<bool>();
			}

			if (steps->size() > a_config.maxExpandedSteps ||
				(plan.repeat != 0 &&
					steps->size() > a_config.maxExpandedSteps / plan.repeat))
				throw ToolError(
					400,
					std::format(
						"expanded scenario steps are capped at {}",
						a_config.maxExpandedSteps));

			std::uint64_t declaredMs = 0;
			plan.steps.reserve(steps->size());
			for (const auto& value : *steps)
			{
				Step step = ParseStep(value, a_registry, a_config);
				if (step.kind == StepKind::kWait || step.kind == StepKind::kPose)
					declaredMs += static_cast<std::uint64_t>(step.wait.count());
				else if (step.kind == StepKind::kWaitFor ||
						 step.kind == StepKind::kWaitUntil)
					declaredMs += static_cast<std::uint64_t>(step.timeout.count());
				plan.steps.push_back(std::move(step));
			}
			const auto maxRunMs =
				static_cast<std::uint64_t>(a_config.maxRunTime.count());
			if (declaredMs > maxRunMs ||
				(plan.repeat != 0 && declaredMs > maxRunMs / plan.repeat))
				throw ToolError(
					400,
					std::format(
						"declared scenario waits exceed the {} ms run limit",
						a_config.maxRunTime.count()));
			return plan;
		}

		json StepEvent(
			const RunRecord& a_record,
			const Step&      a_step,
			std::size_t      a_index,
			std::size_t      a_repetition,
			std::string_view a_phase)
		{
			json event{
				{ "runId", a_record.id },
				{ "index", a_index },
				{ "total", a_record.plan.steps.size() },
				{ "repetition", a_repetition },
				{ "repeat", a_record.plan.repeat },
				{ "kind", KindName(a_step.kind) },
				{ "phase", a_phase },
			};
			if (!a_step.label.empty())
				event["label"] = a_step.label;
			if (a_step.kind == StepKind::kTool)
				event["tool"] = a_step.tool;
			return event;
		}

		json StepBase(
			const Step& a_step,
			std::size_t a_index,
			std::size_t a_repetition)
		{
			json out{
				{ "index", a_index },
				{ "repetition", a_repetition },
				{ "kind", KindName(a_step.kind) },
			};
			if (!a_step.label.empty())
				out["label"] = a_step.label;
			return out;
		}

		void ApplyObservation(
			json&                       a_transcript,
			const ValuePredicate&       a_predicate,
			const PredicateObservation& a_observation)
		{
			a_transcript["condition"] = DescribeValuePredicate(a_predicate);
			a_transcript["path"] = a_predicate.path;
			a_transcript["expected"] = a_predicate.expected;
			a_transcript["actual"] = a_observation.actual;
			a_transcript["available"] = a_observation.available;
			a_transcript["missing"] = !a_observation.available;
		}
	}

	struct ScenarioService::Impl
	{
		ToolRegistry&  registry;
		EventBus&      events;
		ScenarioConfig config;

		std::mutex                                          mutex;
		std::condition_variable                             workerCv;
		bool                                                stopping = false;
		bool                                                registered = false;
		std::uint64_t                                       nextId = 0;
		std::shared_ptr<RunRecord>                          active;
		std::shared_ptr<RunRecord>                          pending;
		std::map<std::uint64_t, std::shared_ptr<RunRecord>> runs;
		std::deque<std::uint64_t>                           completed;
		std::thread                                         worker;

		Impl(
			ToolRegistry&  a_registry,
			EventBus&      a_events,
			ScenarioConfig a_config) :
			registry(a_registry),
			events(a_events),
			config(std::move(a_config))
		{
			if (config.historyLimit == 0 || config.maxExpandedSteps == 0 ||
				config.maxTranscriptBytes == 0 || config.maxRunTime.count() <= 0 ||
				config.maxStepTimeout.count() <= 0 || config.maxWait.count() < 0 ||
				config.maxPoll.count() <= 0)
				throw std::invalid_argument("invalid ScenarioConfig limits");
			worker = std::thread([this] { WorkerLoop(); });
		}

		~Impl()
		{
			Shutdown();
		}

		bool Register()
		{
			{
				std::lock_guard lock(mutex);
				if (registered)
					return false;
				if (stopping)
					throw std::logic_error("cannot register a stopped ScenarioService");
				registered = true;
			}
			return registry.Register(
				BuildScenarioDescriptor(config),
				[this](const json& a_args, const ToolContext& a_context) {
					return Handle(a_args, a_context, {});
				});
		}

		void Shutdown()
		{
			{
				std::lock_guard lock(mutex);
				if (stopping)
					return;
				stopping = true;
				if (active)
					active->cancelRequested.store(true);
				if (pending)
					pending->cancelRequested.store(true);
			}
			workerCv.notify_all();
			if (worker.joinable())
				worker.join();
		}

		json Handle(
			const json& a_args, const ToolContext& a_context,
			ScenarioRunOptions a_options)
		{
			if (!a_args.is_object())
				throw ToolError(400, "scenario arguments must be an object");
			std::string action = "run";
			if (const auto it = a_args.find("action"); it != a_args.end())
			{
				if (!it->is_string())
					throw ToolError(400, "'action' must be a string");
				action = it->get<std::string>();
			}

			if (action == "status")
			{
				RejectUnknownKeys(a_args, { "action", "runId" }, "scenario status");
				return Status(ReadPositiveId(a_args));
			}
			if (action == "cancel")
			{
				RejectUnknownKeys(a_args, { "action", "runId" }, "scenario cancel");
				return Cancel(ReadPositiveId(a_args));
			}
			if (action != "run")
				throw ToolError(
					400,
					std::format("unknown scenario action '{}' (run|status|cancel)", action));

			Plan       plan = ParsePlan(a_args, registry, config);
			const bool isAsync = plan.async;
			auto       record = Submit(
				std::move(plan), a_context, std::move(a_options));
			if (isAsync)
				return json{
					{ "queued", true },
					{ "runId", record->id },
					{ "steps", record->plan.steps.size() },
					{ "expandedSteps", record->plan.steps.size() * record->plan.repeat },
				};

			std::unique_lock lock(record->mutex);
			record->doneCv.wait(lock, [&] { return record->phase == RunPhase::kDone; });
			return record->result;
		}

		std::shared_ptr<RunRecord> Submit(
			Plan a_plan, const ToolContext& a_context, ScenarioRunOptions a_options)
		{
			std::shared_ptr<RunRecord> record;
			{
				std::lock_guard lock(mutex);
				if (stopping)
					throw ToolError(503, "scenario service is shutting down");
				if (active)
					throw ToolError(409, "another scenario is already pending or running");

				record = std::make_shared<RunRecord>();
				record->id = ++nextId;
				record->plan = std::move(a_plan);
				record->context = a_context;
				record->options = std::move(a_options);
				record->owner = record->options.owner.empty() ?
				                    std::format("scenario:{}", record->id) :
				                    record->options.owner;
				active = record;
				pending = record;
				runs.emplace(record->id, record);
			}
			if (record->options.onSubmitted)
			{
				try
				{
					record->options.onSubmitted(record->id);
				}
				catch (...)
				{}
			}
			workerCv.notify_one();
			return record;
		}

		std::shared_ptr<RunRecord> Find(std::uint64_t a_runId)
		{
			std::lock_guard lock(mutex);
			const auto      it = runs.find(a_runId);
			return it == runs.end() ? nullptr : it->second;
		}

		json Status(std::uint64_t a_runId)
		{
			const auto record = Find(a_runId);
			if (!record)
				throw ToolError(
					404,
					std::format("unknown scenario runId {}", a_runId));

			std::lock_guard lock(record->mutex);
			if (record->phase == RunPhase::kDone)
				return json{
					{ "runId", record->id },
					{ "done", true },
					{ "ok", record->result.value("ok", false) },
					{ "result", record->result },
				};
			return json{
				{ "runId", record->id },
				{ "done", false },
				{ "status", record->phase == RunPhase::kPending ? "pending" : "running" },
				{ "cancelRequested", record->cancelRequested.load() },
				{ "index", record->currentIndex },
				{ "repetition", record->currentRepetition },
			};
		}

		json Cancel(std::uint64_t a_runId)
		{
			const auto record = Find(a_runId);
			if (!record)
				throw ToolError(
					404,
					std::format("unknown scenario runId {}", a_runId));

			{
				std::lock_guard lock(record->mutex);
				if (record->phase == RunPhase::kDone)
					return json{
						{ "runId", record->id },
						{ "done", true },
						{ "cancelRequested", false },
						{ "result", record->result },
					};
				record->cancelRequested.store(true);
			}
			workerCv.notify_all();
			return json{
				{ "runId", record->id },
				{ "done", false },
				{ "cancelRequested", true },
				{ "note", "cancellation is cooperative and cannot undo or preempt an accepted tool mutation" },
			};
		}

		bool IsStopping()
		{
			std::lock_guard lock(mutex);
			return stopping;
		}

		StopReason CheckStop(
			const std::shared_ptr<RunRecord>& a_record,
			Clock::time_point                 a_runDeadline)
		{
			if (a_record->cancelRequested.load() || IsStopping())
				return StopReason::kCancelled;
			if (Clock::now() >= a_runDeadline)
				return StopReason::kTimedOut;
			return StopReason::kNone;
		}

		StopReason WaitInterruptibly(
			const std::shared_ptr<RunRecord>& a_record,
			Clock::time_point                 a_until,
			Clock::time_point                 a_runDeadline)
		{
			const auto       wakeAt = std::min(a_until, a_runDeadline);
			std::unique_lock lock(mutex);
			workerCv.wait_until(lock, wakeAt, [&] {
				return stopping || a_record->cancelRequested.load();
			});
			if (stopping || a_record->cancelRequested.load())
				return StopReason::kCancelled;
			if (Clock::now() >= a_runDeadline)
				return StopReason::kTimedOut;
			return StopReason::kNone;
		}

		void WorkerLoop()
		{
			for (;;)
			{
				std::shared_ptr<RunRecord> record;
				{
					std::unique_lock lock(mutex);
					workerCv.wait(lock, [&] { return stopping || pending != nullptr; });
					if (!pending)
					{
						if (stopping)
							return;
						continue;
					}
					record = std::move(pending);
				}

				const auto started = Clock::now();
				const auto deadline = started + config.maxRunTime;
				{
					std::lock_guard lock(record->mutex);
					record->phase = RunPhase::kRunning;
				}
				events.Publish(
					"scenario.started",
					json{
						{ "runId", record->id },
						{ "steps", record->plan.steps.size() },
						{ "repeat", record->plan.repeat },
						{ "expandedSteps", record->plan.steps.size() * record->plan.repeat },
					});

				json result;
				try
				{
					result = Execute(record, started, deadline);
				}
				catch (const std::exception& a_error)
				{
					result = json{
						{ "runId", record->id },
						{ "ok", false },
						{ "status", "failed" },
						{ "aborted", true },
						{ "cancelled", false },
						{ "timedOut", false },
						{ "stepsRun", 0 },
						{ "elapsedMs", std::chrono::duration_cast<Milliseconds>(
										   Clock::now() - started)
										   .count() },
						{ "error", a_error.what() },
						{ "results", json::array() },
					};
				}
				Finalize(record, std::move(result), started, deadline);
			}
		}

		void Finalize(
			const std::shared_ptr<RunRecord>& a_record,
			json                              a_result,
			Clock::time_point                 a_started,
			Clock::time_point                 a_deadline)
		{
			const auto cleanup = CleanupOwned(*a_record);
			if (!cleanup.empty())
				a_result["cleanup"] = cleanup;
			const bool cleanupIncomplete =
				cleanup.is_object() && cleanup.contains("camera") &&
				cleanup.at("camera").is_object() &&
				cleanup.at("camera").value("restorationIncomplete", false);
			const bool cleanupUncertain =
				cleanup.is_object() && cleanup.contains("camera") &&
				cleanup.at("camera").is_object() &&
				(cleanup.at("camera").value("releaseUncertain", false) ||
					cleanup.at("camera").value("fallbackUncertain", false));
			std::function<void(std::uint64_t, const json&)> completedCallback;
			json                                             completedResult;
			{
				std::lock_guard recordLock(a_record->mutex);
				if (a_record->cancelRequested.load() ||
					(a_result.value("status", std::string{}) != "cancelled" && IsStopping()))
				{
					a_result["ok"] = false;
					a_result["status"] = "cancelled";
					a_result["aborted"] = true;
					a_result["cancelled"] = true;
				}
				else if (Clock::now() >= a_deadline &&
						 a_result.value("status", std::string{}) != "timedOut")
				{
					a_result["ok"] = false;
					a_result["status"] = "timedOut";
					a_result["aborted"] = true;
					a_result["timedOut"] = true;
				}
				else if (cleanupIncomplete || cleanupUncertain)
				{
					a_result["ok"] = false;
					a_result["status"] = "failed";
					a_result["aborted"] = true;
					a_result["cleanupUncertain"] = cleanupUncertain;
				}
				a_result["elapsedMs"] =
					std::chrono::duration_cast<Milliseconds>(Clock::now() - a_started)
						.count();
				if (a_record->options.finalizeResult)
				{
					try
					{
						a_record->options.finalizeResult(a_result);
					}
					catch (const std::exception& a_error)
					{
						a_result["ok"] = false;
						a_result["status"] = "failed";
						a_result["aborted"] = true;
						a_result["resultFinalizationError"] = a_error.what();
					}
				}
				try
				{
					events.Publish(
						"scenario.finished",
						json{
							{ "runId", a_record->id },
							{ "ok", a_result.value("ok", false) },
							{ "status", a_result.value("status", "failed") },
							{ "stepsRun", a_result.value("stepsRun", 0) },
							{ "cancelled", a_result.value("cancelled", false) },
							{ "timedOut", a_result.value("timedOut", false) },
						});
				}
				catch (...)
				{}
				a_record->result = std::move(a_result);
				a_record->phase = RunPhase::kDone;
				completedCallback = a_record->options.onCompleted;
				completedResult = a_record->result;
			}
			{
				std::lock_guard lock(mutex);
				if (active == a_record)
					active.reset();
				completed.push_back(a_record->id);
				while (completed.size() > config.historyLimit)
				{
					const auto oldest = completed.front();
					completed.pop_front();
					if (!active || active->id != oldest)
						runs.erase(oldest);
				}
			}
			a_record->doneCv.notify_all();
			workerCv.notify_all();
			if (completedCallback)
			{
				try
				{
					completedCallback(a_record->id, completedResult);
				}
				catch (...)
				{}
			}
		}

		json CleanupOwned(const RunRecord& a_record) noexcept
		{
			json cleanup = json::object();
			try
			{
				ToolContext context = a_record.context;
				context.internal = true;
				if (a_record.ownsInput && registry.Has("input"))
					(void)registry.Invoke(
						"input",
						json{
							{ "action", "releaseAll" },
							{ "owner", a_record.owner },
						},
						context);
				if (a_record.cameraSnapshotError)
					cleanup["camera"] = json{
						{ "snapshotError", *a_record.cameraSnapshotError },
					};
				if (a_record.cameraFreecamAcquired && registry.Has("camera"))
				{
					auto& camera = cleanup["camera"];
					if (!camera.is_object())
						camera = json::object();
					camera["acquired"] = true;
					if (a_record.cameraReleaseUncertain)
					{
						camera["releaseUncertain"] = true;
						camera["restorationIncomplete"] = true;
						camera["restoration"] =
							"not retried after an uncertain freecam mutation";
					}
					else
					{
						const auto released = registry.Invoke(
							"camera",
							json{
								{ "action", "freecam" },
								{ "on", false },
							},
							context);
						camera["release"] = released.ok ?
						                        released.value :
						                        json{
													{ "ok", false },
													{ "status", released.errorCode },
													{ "error", released.errorMessage },
												};
						if (!released.ok && released.errorCode == 504)
						{
							camera["releaseUncertain"] = true;
							camera["restorationIncomplete"] = true;
							camera["restoration"] =
								"freecam release started or is uncertain; not retried";
						}
						else if (!released.ok)
						{
							camera["restorationIncomplete"] = true;
							camera["restoration"] =
								"freecam release failed";
						}
						else if (released.ok && a_record.cameraBefore)
						{
							json current = released.value;
							if (!current.is_object() || !current.contains("pov"))
							{
								const auto observed = registry.Invoke(
									"camera", json{ { "action", "get" } }, context);
								if (observed.ok && observed.value.is_object())
									current = observed.value;
							}
							const auto wantedPov =
								a_record.cameraBefore->value("pov", std::string{});
							const auto currentPov =
								current.value("pov", std::string{});
							camera["initialPov"] = wantedPov;
							camera["observedPov"] = currentPov;
							if (!wantedPov.empty() && wantedPov != currentPov)
							{
								if (wantedPov == "first" ||
									wantedPov == "third" ||
									wantedPov == "vanity")
								{
									const auto corrected = registry.Invoke(
										"camera",
										json{
											{ "action", "setPov" },
											{ "pov", wantedPov },
										},
										context);
									camera["fallback"] = corrected.ok ?
									                         corrected.value :
									                         json{
																 { "ok", false },
																 { "status", corrected.errorCode },
																 { "error", corrected.errorMessage },
															 };
									if (!corrected.ok &&
										corrected.errorCode == 504)
										camera["fallbackUncertain"] = true;
									if (!corrected.ok)
										camera["restorationIncomplete"] = true;
								}
								else
								{
									camera["restorationIncomplete"] = true;
									camera["restoration"] =
										"initial POV 'other' cannot be forced through the public camera contract";
								}
							}
						}
						else if (released.ok)
						{
							camera["restorationIncomplete"] = true;
							camera["restoration"] =
								"initial camera snapshot unavailable; restored POV cannot be verified";
						}
					}
				}
			}
			catch (...)
			{}
			return cleanup;
		}

		json Execute(
			const std::shared_ptr<RunRecord>& a_record,
			Clock::time_point                 a_started,
			Clock::time_point                 a_runDeadline)
		{
			json        results = json::array();
			std::size_t transcriptBytes = 0;
			bool        anyFailure = false;
			bool        aborted = false;
			bool        cancelled = false;
			bool        timedOut = false;

			const bool usesCameraMutation =
				std::ranges::any_of(
					a_record->plan.steps,
					[](const Step& a_step) {
						return a_step.kind == StepKind::kTool &&
						       a_step.tool == "camera" &&
						       a_step.args.value(
								   "action", std::string("get")) != "get";
					});
			if (usesCameraMutation && registry.Has("camera"))
			{
				const auto before = InvokeInternal(
					"camera", json{ { "action", "get" } }, a_record->context);
				if (before.ok && before.value.is_object())
					a_record->cameraBefore = before.value;
				else
					a_record->cameraSnapshotError = json{
						{ "status", before.errorCode },
						{ "error", before.errorMessage },
					};
			}

			for (std::size_t repetition = 0;
				repetition < a_record->plan.repeat && !aborted;
				++repetition)
			{
				std::optional<ActionCorrelation> correlation;
				bool                             sceneMismatch = false;
				for (std::size_t index = 0;
					index < a_record->plan.steps.size() && !aborted;
					++index)
				{
					const StopReason before = CheckStop(a_record, a_runDeadline);
					if (before != StopReason::kNone)
					{
						aborted = true;
						cancelled = before == StopReason::kCancelled;
						timedOut = before == StopReason::kTimedOut;
						break;
					}

					{
						std::lock_guard lock(a_record->mutex);
						a_record->currentIndex = index;
						a_record->currentRepetition = repetition;
					}
					const Step& step = a_record->plan.steps[index];
					events.Publish(
						"scenario.step",
						StepEvent(*a_record, step, index, repetition, "started"));

					const auto  stepStarted = Clock::now();
					StepOutcome outcome = ExecuteStep(
						a_record,
						step,
						index,
						repetition,
						a_runDeadline,
						correlation,
						sceneMismatch);
					outcome.transcript["elapsedMs"] =
						std::chrono::duration_cast<Milliseconds>(
							Clock::now() - stepStarted)
							.count();

					const std::size_t stepBytes = outcome.transcript.dump().size();
					if (stepBytes > config.maxTranscriptBytes - std::min(
																	transcriptBytes, config.maxTranscriptBytes))
					{
						outcome.transcript = StepBase(step, index, repetition);
						outcome.transcript["ok"] = false;
						outcome.transcript["status"] = 413;
						outcome.transcript["error"] =
							"scenario transcript size limit exceeded";
						outcome.transcript["elapsedMs"] =
							std::chrono::duration_cast<Milliseconds>(
								Clock::now() - stepStarted)
								.count();
						outcome.failed = true;
						aborted = true;
					}
					transcriptBytes += outcome.transcript.dump().size();
					results.push_back(outcome.transcript);

					json completedEvent =
						StepEvent(*a_record, step, index, repetition, "completed");
					completedEvent["ok"] = outcome.transcript.value("ok", false);
					completedEvent["status"] = outcome.transcript.value("status", 500);
					completedEvent["elapsedMs"] =
						outcome.transcript.value("elapsedMs", 0);
					events.Publish("scenario.step", std::move(completedEvent));

					if (outcome.failed)
						anyFailure = true;
					if (outcome.cancelled)
					{
						cancelled = true;
						aborted = true;
					}
					else if (outcome.runTimedOut)
					{
						timedOut = true;
						aborted = true;
					}
					else if (outcome.failed && !a_record->plan.continueOnError)
						aborted = true;
				}
			}

			if (!cancelled && !timedOut)
			{
				const StopReason after = CheckStop(a_record, a_runDeadline);
				cancelled = after == StopReason::kCancelled;
				timedOut = after == StopReason::kTimedOut;
				aborted = aborted || cancelled || timedOut;
			}

			std::string status = "completed";
			if (cancelled)
				status = "cancelled";
			else if (timedOut)
				status = "timedOut";
			else if (anyFailure)
				status = "failed";
			json result{
				{ "runId", a_record->id },
				{ "ok", !anyFailure && !cancelled && !timedOut },
				{ "status", status },
				{ "aborted", aborted },
				{ "cancelled", cancelled },
				{ "timedOut", timedOut },
				{ "stepsRun", results.size() },
				{ "elapsedMs", std::chrono::duration_cast<Milliseconds>(
								   Clock::now() - a_started)
								   .count() },
				{ "results", std::move(results) },
			};
			if (a_record->options.resultMetadata.is_object())
				result.update(a_record->options.resultMetadata);
			return result;
		}

		StepOutcome ExecuteStep(
			const std::shared_ptr<RunRecord>& a_record,
			const Step&                       a_step,
			std::size_t                       a_index,
			std::size_t                       a_repetition,
			Clock::time_point                 a_runDeadline,
			std::optional<ActionCorrelation>& a_correlation,
			bool&                             a_sceneMismatch)
		{
			StepOutcome outcome;
			outcome.transcript = StepBase(a_step, a_index, a_repetition);

			if (a_step.kind == StepKind::kTool)
			{
				outcome.transcript["tool"] = a_step.tool;
				const std::uint64_t dispatchCursor = events.HeadSeq();
				a_correlation = ActionCorrelation{ .eventCursor = dispatchCursor };
				ToolContext context = a_record->context;
				context.internal = true;
				json args = a_step.args;
				if (a_step.tool == "capture")
				{
					args["runId"] = a_record->id;
					args["repeat"] = a_repetition;
					if (a_sceneMismatch)
						args["sceneMismatch"] = true;
				}
				else if (a_step.tool == "input" && !args.contains("owner"))
				{
					args["owner"] = a_record->owner;
					a_record->ownsInput = true;
				}
				const ToolResult result =
					registry.Invoke(a_step.tool, args, context);
				if (!result.ok)
				{
					if (a_step.tool == "camera" &&
						args.value("action", std::string{}) == "freecam" &&
						result.errorCode == 504)
						a_record->cameraReleaseUncertain = true;
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = result.errorCode;
					outcome.transcript["error"] = result.errorMessage;
					outcome.failed = true;
					return outcome;
				}

				const bool embeddedFailure =
					ScenarioPolicy::IsEmbeddedToolFailure(result.value);
				outcome.transcript["result"] = result.value;
				if (result.value.is_object())
				{
					const auto cursor = result.value.find("eventCursor");
					if (cursor != result.value.end() &&
						(cursor->is_number_integer() || cursor->is_number_unsigned()))
					{
						std::optional<std::uint64_t> receiptCursor;
						if (cursor->is_number_unsigned())
							receiptCursor = cursor->get<std::uint64_t>();
						else
						{
							const auto value = cursor->get<std::int64_t>();
							if (value >= 0)
								receiptCursor = static_cast<std::uint64_t>(value);
						}
						const std::uint64_t head = events.HeadSeq();
						if (receiptCursor && *receiptCursor >= dispatchCursor &&
							*receiptCursor <= head)
						{
							a_correlation->eventCursor = *receiptCursor;
							outcome.transcript["eventCursorUsed"] = *receiptCursor;
						}
						else
						{
							outcome.transcript["eventCursorIgnored"] = true;
						}
					}
					if (a_step.tool == "game")
					{
						const auto operationId = result.value.find("operationId");
						if (operationId != result.value.end() &&
							(operationId->is_number_integer() ||
								operationId->is_number_unsigned()))
						{
							const auto value = operationId->is_number_unsigned() ?
							                       operationId->get<std::uint64_t>() :
							                       static_cast<std::uint64_t>(
													   std::max<std::int64_t>(
														   0, operationId->get<std::int64_t>()));
							if (value != 0)
								a_correlation->gameOperationId = value;
						}
					}
				}
				if (embeddedFailure)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] =
						ScenarioPolicy::kEmbeddedToolFallbackErrorCode;
					outcome.transcript["errorCode"] =
						ScenarioPolicy::EmbeddedToolErrorCode(result.value);
					outcome.transcript["error"] =
						ScenarioPolicy::EmbeddedToolErrorMessage(result.value);
					outcome.failed = true;
					return outcome;
				}
				if (a_step.tool == "capture" && result.value.is_object() &&
					result.value.value("inconclusive", false))
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 422;
					outcome.transcript["errorCode"] = "capture_inconclusive";
					outcome.transcript["error"] = result.value.value(
						"inconclusiveReason",
						std::string("capture is inconclusive and cannot satisfy a scenario checkpoint"));
					outcome.failed = true;
					return outcome;
				}
				if (a_step.tool == "camera" &&
					args.value("action", std::string{}) == "freecam")
				{
					const bool requestedOn = args.value("on", false);
					if (requestedOn &&
						result.value.value("changed", false) &&
						result.value.value("freeCamOwned", false))
						a_record->cameraFreecamAcquired = true;
					else if (
						!requestedOn &&
						result.value.value("completed", false) &&
						!result.value.value("freeCam", true))
						a_record->cameraFreecamAcquired = false;
				}

				outcome.transcript["ok"] = true;
				outcome.transcript["status"] = 200;
				const bool queued = result.value.is_object() &&
				                    result.value.value("queued", false);
				outcome.transcript["accepted"] = true;
				outcome.transcript["completed"] = !queued;
				return outcome;
			}

			if (a_step.kind == StepKind::kPose)
			{
				outcome.transcript["pose"] = a_step.pose;
				ToolContext context = a_record->context;
				context.internal = true;
				constexpr std::array<std::string_view, 5> commands{
					"player.setpos x ",
					"player.setpos y ",
					"player.setpos z ",
					"player.setangle z ",
					"player.setangle x ",
				};
				json receipts = json::array();
				for (std::size_t index = 0; index < commands.size(); ++index)
				{
					const auto command =
						std::format("{}{:.2f}", commands[index], a_step.pose[index]);
					const auto result = registry.Invoke(
						"console",
						json{ { "action", "exec" }, { "command", command } },
						context);
					if (!result.ok ||
						ScenarioPolicy::IsEmbeddedToolFailure(result.value))
					{
						outcome.transcript["ok"] = false;
						outcome.transcript["status"] =
							result.ok ?
								ScenarioPolicy::kEmbeddedToolFallbackErrorCode :
								result.errorCode;
						outcome.transcript["error"] = result.ok ?
							ScenarioPolicy::EmbeddedToolErrorMessage(result.value) :
							std::format("pose command '{}' failed: {}", command, result.errorMessage);
						outcome.transcript["receipts"] = std::move(receipts);
						outcome.failed = true;
						return outcome;
					}
					receipts.push_back(result.value);
				}
				outcome.transcript["receipts"] = std::move(receipts);
				if (a_step.wait.count() > 0)
				{
					const auto started = Clock::now();
					const auto stop = WaitInterruptibly(
						a_record, started + a_step.wait, a_runDeadline);
					outcome.transcript["expectedWaitMs"] = a_step.wait.count();
					outcome.transcript["actualWaitMs"] =
						std::chrono::duration_cast<Milliseconds>(
							Clock::now() - started)
							.count();
					if (stop != StopReason::kNone)
					{
						outcome.transcript["ok"] = false;
						outcome.transcript["status"] =
							stop == StopReason::kCancelled ? kCancelledStatus : 504;
						outcome.transcript["error"] =
							stop == StopReason::kCancelled ?
								"scenario cancelled during pose pacing wait" :
								"scenario run deadline reached during pose pacing wait";
						outcome.failed = true;
						outcome.cancelled = stop == StopReason::kCancelled;
						outcome.runTimedOut = stop == StopReason::kTimedOut;
						return outcome;
					}
				}
				outcome.transcript["ok"] = true;
				outcome.transcript["status"] = 200;
				return outcome;
			}

			if (a_step.kind == StepKind::kWait)
			{
				const auto started = Clock::now();
				outcome.transcript["expected"] = a_step.wait.count();
				const StopReason stop = WaitInterruptibly(
					a_record, started + a_step.wait, a_runDeadline);
				outcome.transcript["actual"] =
					std::chrono::duration_cast<Milliseconds>(Clock::now() - started)
						.count();
				if (stop == StopReason::kCancelled)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = kCancelledStatus;
					outcome.transcript["error"] = "scenario cancelled during wait";
					outcome.failed = true;
					outcome.cancelled = true;
				}
				else if (stop == StopReason::kTimedOut)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 504;
					outcome.transcript["timedOut"] = true;
					outcome.transcript["error"] = "scenario run deadline reached during wait";
					outcome.failed = true;
					outcome.runTimedOut = true;
				}
				else
				{
					outcome.transcript["ok"] = true;
					outcome.transcript["status"] = 200;
					outcome.transcript["result"] =
						json{ { "waitedMs", outcome.transcript["actual"] } };
				}
				return outcome;
			}

			if (a_step.kind == StepKind::kWaitFor)
				return ExecuteWaitFor(
					a_record,
					a_step,
					a_index,
					a_repetition,
					a_runDeadline,
					a_correlation);

			if (!a_step.condition.empty())
				return ExecuteStateCondition(
					a_record, a_step, a_index, a_repetition, a_runDeadline,
					a_sceneMismatch);

			return ExecutePredicate(
				a_record,
				a_step,
				a_index,
				a_repetition,
				a_runDeadline);
		}

		ToolResult InvokeInternal(
			std::string_view a_tool, const json& a_args, const ToolContext& a_context)
		{
			ToolContext context = a_context;
			context.internal = true;
			return registry.Invoke(a_tool, a_args, context);
		}

		json ReadOpenMenus(const ToolContext& a_context)
		{
			if (registry.Has("menu"))
			{
				const auto result =
					InvokeInternal("menu", json{ { "action", "list" } }, a_context);
				if (result.ok && result.value.is_object())
				{
					if (const auto menus = result.value.find("openMenus");
						menus != result.value.end() && menus->is_array())
						return *menus;
					if (const auto menus = result.value.find("menus");
						menus != result.value.end() && menus->is_array())
						return *menus;
					if (const auto menus = result.value.find("open");
						menus != result.value.end() && menus->is_array())
						return *menus;
				}
			}

			const auto state = InvokeInternal(
				"inspect", json{ { "kind", "state" } }, a_context);
			if (!state.ok)
				throw ToolError(state.errorCode, state.errorMessage);
			const auto menus = state.value.find("menus");
			if (menus == state.value.end() || !menus->is_object())
				throw ToolError(500, "inspect state omitted menu state");
			const auto open = menus->find("open");
			if (open == menus->end() || !open->is_array())
				throw ToolError(500, "inspect state omitted open menu names");
			return *open;
		}

		std::vector<std::string> BlockingMenus(const ToolContext& a_context)
		{
			const auto menus = ReadOpenMenus(a_context);
			std::vector<std::string> blocking;
			for (const auto& item : menus)
			{
				if (item.is_object())
				{
					if (!item.value("open", true) || !item.value("blocking", true))
						continue;
					const auto name = item.value("name", std::string{});
					if (!name.empty())
						blocking.push_back(name);
					continue;
				}
				if (!item.is_string())
					continue;
				const auto name = item.get<std::string>();
				if (name == "HUD Menu" || name == "HUDMenu" ||
					name == "Cursor Menu" || name == "CursorMenu" ||
					name == "Console")
					continue;
				blocking.push_back(name);
			}
			return blocking;
		}

		bool CheckState(std::string_view a_condition, const ToolContext& a_context)
		{
			if (a_condition == "playerLoaded")
			{
				const auto state = InvokeInternal(
					"inspect", json{ { "kind", "state" } }, a_context);
				return state.ok && state.value.value("playerLoaded", false);
			}

			const auto menus = ReadOpenMenus(a_context);
			if (a_condition == "noMenu")
				return menus.empty();
			if (a_condition == "noModal")
			{
				for (const auto& item : menus)
				{
					const auto name = item.is_string() ?
					                      item.get<std::string>() :
					                      item.value("name", std::string{});
					if (name == "MessageBoxMenu")
						return false;
				}
				return true;
			}
			if (a_condition == "noBlockingMenu")
				return BlockingMenus(a_context).empty();
			throw ToolError(400, std::format(
									 "unknown scenario state condition '{}'", a_condition));
		}

		std::optional<json> AcceptReplayModal(
			const ModalAcceptSpec& a_spec, const ToolContext& a_context,
			const std::shared_ptr<RunRecord>& a_record,
			Clock::time_point a_runDeadline)
		{
			if (!registry.Has("menu"))
				return std::nullopt;
			const auto described = InvokeInternal(
				"menu",
				json{ { "action", "describe" } },
				a_context);
			if (!described.ok || !described.value.is_object() ||
				!described.value.value("messageBoxOpen", false))
				return std::nullopt;

			const auto body = described.value.value(
				"bodyText", std::string{});
			if (!ContainsCaseInsensitive(body, a_spec.matchBody))
				return std::nullopt;
			const auto buttons = described.value.value("buttons", json::array());
			const auto fingerprint = described.value.value(
				"fingerprint", json(nullptr));
			if (!fingerprint.is_object())
				throw ToolError(
					409,
					"matching MessageBoxMenu has no fingerprint; refusing automatic acceptance");
			if (!buttons.is_array() || a_spec.buttonIndex >= buttons.size())
				throw ToolError(
					409,
					std::format(
						"matching MessageBoxMenu has no button at index {}",
						a_spec.buttonIndex));
			const auto stop = CheckStop(a_record, a_runDeadline);
			if (stop != StopReason::kNone)
				throw ToolError(
					stop == StopReason::kCancelled ? kCancelledStatus : 504,
					stop == StopReason::kCancelled ?
						"scenario cancelled before modal acceptance" :
						"scenario run deadline reached before modal acceptance");

			const auto accepted = InvokeInternal(
				"menu",
				json{
					{ "action", "accept" },
					{ "index", a_spec.buttonIndex },
					{ "matchBody", a_spec.matchBody },
				},
				a_context);
			if (!accepted.ok)
				throw ToolError(accepted.errorCode, accepted.errorMessage);
			return json{
				{ "body", body },
				{ "buttons", buttons },
				{ "fingerprint", fingerprint },
				{ "index", a_spec.buttonIndex },
				{ "response", accepted.value },
			};
		}

		StepOutcome ExecuteStateCondition(
			const std::shared_ptr<RunRecord>& a_record,
			const Step&                       a_step,
			std::size_t                       a_index,
			std::size_t                       a_repetition,
			Clock::time_point                 a_runDeadline,
			bool&                             a_sceneMismatch)
		{
			StepOutcome outcome;
			outcome.transcript = StepBase(a_step, a_index, a_repetition);
			outcome.transcript["condition"] = a_step.condition;

			if (a_step.condition == "scene")
			{
				const auto deadline = Clock::now() + a_step.timeout;
				json       actual;
				bool       available = false;
				for (;;)
				{
					const auto observed = InvokeInternal(
						"inspect", json{ { "kind", "scene" } }, a_record->context);
					if (observed.ok && observed.value.value("playerLoaded", false))
					{
						actual = observed.value;
						available = true;
						break;
					}
					const auto now = Clock::now();
					if (now >= deadline || now >= a_runDeadline)
						break;
					const auto stop = WaitInterruptibly(
						a_record,
						std::min(deadline, now + a_step.poll),
						a_runDeadline);
					if (stop != StopReason::kNone)
					{
						outcome.transcript["ok"] = false;
						outcome.transcript["status"] =
							stop == StopReason::kCancelled ? kCancelledStatus : 504;
						outcome.transcript["error"] =
							stop == StopReason::kCancelled ?
								"scenario cancelled while checking scene" :
								"scenario run deadline reached while checking scene";
						outcome.failed = true;
						outcome.cancelled = stop == StopReason::kCancelled;
						outcome.runTimedOut = stop == StopReason::kTimedOut;
						return outcome;
					}
				}

				const auto& scene = *a_step.scene;
				outcome.transcript["expected"] = json{
					{ "interior", scene.interior },
					{ "worldspaceFormID", scene.worldspaceFormId },
					{ "cellFormID", scene.cellFormId },
					{ "worldspace", scene.worldspace },
					{ "cell", scene.cell },
				};
				outcome.transcript["actual"] = available ? actual : json(nullptr);
				outcome.transcript["available"] = available;
				bool matches = false;
				std::uint32_t currentWorldspace = 0;
				std::uint32_t currentCell = 0;
				if (available)
				{
					if (const auto worldspace = actual.find("worldspace");
						worldspace != actual.end() && worldspace->is_object())
						currentWorldspace =
							worldspace->value("formId", std::uint32_t{ 0 });
					if (const auto cell = actual.find("cell");
						cell != actual.end() && cell->is_object())
						currentCell = cell->value("formId", std::uint32_t{ 0 });
					matches = scene.interior ?
					              currentCell == scene.cellFormId :
					              currentWorldspace == scene.worldspaceFormId;
				}

				if (available && matches)
				{
					outcome.transcript["ok"] = true;
					outcome.transcript["status"] = 200;
					outcome.transcript["sceneConfirmed"] = true;
					return outcome;
				}

				a_sceneMismatch = true;
				outcome.transcript["sceneMismatch"] = true;
				outcome.transcript["sceneConfirmed"] = false;
				outcome.transcript["worldspaceFormID"] = currentWorldspace;
				outcome.transcript["cellFormID"] = currentCell;
				const auto message = available ?
				                         std::format(
											 "scene mismatch: recorded {} '{}' (0x{:08X}), currently in 0x{:08X}",
											 scene.interior ? "cell" : "worldspace",
											 scene.interior ? scene.cell : scene.worldspace,
											 scene.interior ? scene.cellFormId : scene.worldspaceFormId,
											 scene.interior ? currentCell : currentWorldspace) :
				                         std::string("scene assert: player never finished loading");
				outcome.transcript["ok"] = scene.soft;
				outcome.transcript["status"] = scene.soft ? 200 : (available ? 409 : 504);
				outcome.transcript[scene.soft ? "warning" : "error"] = message;
				outcome.failed = !scene.soft;
				return outcome;
			}

			if (a_step.kind == StepKind::kWaitUntil)
			{
				const auto deadline = Clock::now() + a_step.timeout;
				for (;;)
				{
					try
					{
						if (CheckState(a_step.condition, a_record->context))
						{
							outcome.transcript["ok"] = true;
							outcome.transcript["status"] = 200;
							outcome.transcript["expected"] = true;
							outcome.transcript["actual"] = true;
							outcome.transcript["available"] = true;
							outcome.transcript["result"] =
								json{ { "satisfied", true } };
							return outcome;
						}
					}
					catch (const ToolError& error)
					{
						if (error.code != 503 && error.code != 504)
						{
							outcome.transcript["ok"] = false;
							outcome.transcript["status"] = error.code;
							outcome.transcript["error"] = error.what();
							outcome.failed = true;
							return outcome;
						}
					}

					const auto now = Clock::now();
					if (now >= deadline || now >= a_runDeadline)
					{
						outcome.transcript["ok"] = false;
						outcome.transcript["status"] = 504;
						outcome.transcript["timedOut"] = true;
						outcome.transcript["expected"] = true;
						outcome.transcript["actual"] = false;
						outcome.transcript["available"] = true;
						outcome.transcript["error"] = std::format(
							"waitUntil '{}' timed out after {} ms",
							a_step.condition,
							a_step.timeout.count());
						outcome.failed = true;
						outcome.runTimedOut = now >= a_runDeadline;
						return outcome;
					}
					const auto stop = WaitInterruptibly(
						a_record,
						std::min(deadline, now + a_step.poll),
						a_runDeadline);
					if (stop != StopReason::kNone)
					{
						outcome.transcript["ok"] = false;
						outcome.transcript["status"] =
							stop == StopReason::kCancelled ? kCancelledStatus : 504;
						outcome.transcript["error"] =
							stop == StopReason::kCancelled ?
								"scenario cancelled while waiting for condition" :
								"scenario run deadline reached while waiting for condition";
						outcome.failed = true;
						outcome.cancelled = stop == StopReason::kCancelled;
						outcome.runTimedOut = stop == StopReason::kTimedOut;
						return outcome;
					}
				}
			}

			const auto blocking = BlockingMenus(a_record->context);
			outcome.transcript["openMenus"] = blocking;
			outcome.transcript["ok"] = blocking.empty();
			outcome.transcript["status"] = blocking.empty() ? 200 : 409;
			if (!blocking.empty())
			{
				std::string names;
				for (const auto& name : blocking)
					names += (names.empty() ? "" : ", ") + name;
				outcome.transcript["error"] = std::format(
					"scenario blocked by open menu(s): [{}]", names);
				outcome.failed = true;
			}
			return outcome;
		}

		StepOutcome ExecuteWaitFor(
			const std::shared_ptr<RunRecord>& a_record,
			const Step&                       a_step,
			std::size_t                       a_index,
			std::size_t                       a_repetition,
			Clock::time_point                 a_runDeadline,
			std::optional<ActionCorrelation>& a_correlation)
		{
			StepOutcome outcome;
			outcome.transcript = StepBase(a_step, a_index, a_repetition);
			outcome.transcript["topic"] = a_step.event.topic;
			outcome.transcript["expected"] =
				json{ { "topic", a_step.event.topic }, { "match", a_step.event.match } };
			outcome.transcript["actual"] = nullptr;
			outcome.transcript["available"] = false;
			json modalResponses = json::array();

			const auto expectedOperation =
				a_correlation ? a_correlation->gameOperationId : std::nullopt;
			std::uint64_t cursor =
				a_correlation ? a_correlation->eventCursor : events.HeadSeq();
			a_correlation.reset();
			const auto stepDeadline = Clock::now() + a_step.timeout;

			for (;;)
			{
				const auto found = events.Since(cursor);
				if (!found.empty() && found.front().seq > cursor + 1)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 409;
					outcome.transcript["eventHistoryLost"] = true;
					outcome.transcript["error"] = std::format(
						"event history was truncated after sequence {} before waitFor could inspect it",
						cursor);
					outcome.failed = true;
					return outcome;
				}
				for (const auto& event : found)
				{
					cursor = event.seq;
					if (event.topic == a_step.event.topic &&
						PayloadMatches(event.payload, a_step.event.match))
					{
						if (expectedOperation)
						{
							const auto status = InvokeInternal(
								"game",
								json{ { "action", "status" } },
								a_record->context);
							if (!status.ok)
							{
								outcome.transcript["ok"] = false;
								outcome.transcript["status"] = status.errorCode;
								outcome.transcript["error"] =
									"could not verify the game operation after its lifecycle event: " +
									status.errorMessage;
								outcome.failed = true;
								return outcome;
							}
							const auto operation =
								status.value.value("operation", json(nullptr));
							if (!operation.is_object() ||
								operation.value("operationId", std::uint64_t{ 0 }) !=
									*expectedOperation)
							{
								outcome.transcript["ok"] = false;
								outcome.transcript["status"] = 409;
								outcome.transcript["operationIdExpected"] =
									*expectedOperation;
								outcome.transcript["operation"] = operation;
								outcome.transcript["error"] =
									"lifecycle event did not correlate to the submitted game operation";
								outcome.failed = true;
								return outcome;
							}
							outcome.transcript["operationId"] =
								*expectedOperation;
							outcome.transcript["operation"] =
								std::move(operation);
						}
						outcome.transcript["ok"] = true;
						outcome.transcript["status"] = 200;
						outcome.transcript["available"] = true;
						outcome.transcript["actual"] = json{
							{ "seq", event.seq },
							{ "frame", event.frame },
							{ "topic", event.topic },
							{ "payload", event.payload },
						};
						outcome.transcript["result"] =
							json{ { "satisfied", true }, { "seq", event.seq } };
						if (!modalResponses.empty())
							outcome.transcript["modalResponses"] =
								std::move(modalResponses);
						return outcome;
					}
				}

				if (a_step.event.acceptModal)
				{
					const auto stop = CheckStop(a_record, a_runDeadline);
					if (stop != StopReason::kNone)
					{
						outcome.transcript["ok"] = false;
						outcome.transcript["status"] =
							stop == StopReason::kCancelled ? kCancelledStatus : 504;
						outcome.transcript["error"] =
							stop == StopReason::kCancelled ?
								"scenario cancelled before modal acceptance" :
								"scenario run deadline reached before modal acceptance";
						outcome.failed = true;
						outcome.cancelled = stop == StopReason::kCancelled;
						outcome.runTimedOut = stop == StopReason::kTimedOut;
						return outcome;
					}
					try
					{
						if (auto response = AcceptReplayModal(
								*a_step.event.acceptModal, a_record->context,
								a_record, a_runDeadline))
							modalResponses.push_back(std::move(*response));
					}
					catch (const ToolError& error)
					{
						outcome.transcript["ok"] = false;
						outcome.transcript["status"] = error.code;
						outcome.transcript["error"] = error.what();
						outcome.transcript["modalResponses"] =
							std::move(modalResponses);
						outcome.failed = true;
						outcome.cancelled = error.code == kCancelledStatus;
						outcome.runTimedOut = error.code == 504 &&
						                      Clock::now() >= a_runDeadline;
						return outcome;
					}
				}

				const auto now = Clock::now();
				if (now >= a_runDeadline)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 504;
					outcome.transcript["timedOut"] = true;
					outcome.transcript["error"] =
						"scenario run deadline reached while waiting for event";
					outcome.failed = true;
					if (!modalResponses.empty())
						outcome.transcript["modalResponses"] =
							std::move(modalResponses);
					outcome.runTimedOut = true;
					return outcome;
				}
				if (now >= stepDeadline)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 504;
					outcome.transcript["timedOut"] = true;
					outcome.transcript["error"] = std::format(
						"waitFor '{}' timed out after {} ms",
						a_step.event.topic,
						a_step.timeout.count());
					outcome.failed = true;
					if (!modalResponses.empty())
						outcome.transcript["modalResponses"] =
							std::move(modalResponses);
					return outcome;
				}

				const StopReason stop = WaitInterruptibly(
					a_record,
					std::min(stepDeadline, now + a_step.poll),
					a_runDeadline);
				if (stop == StopReason::kCancelled)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = kCancelledStatus;
					outcome.transcript["error"] =
						"scenario cancelled while waiting for event";
					outcome.failed = true;
					if (!modalResponses.empty())
						outcome.transcript["modalResponses"] =
							std::move(modalResponses);
					outcome.cancelled = true;
					return outcome;
				}
				if (stop == StopReason::kTimedOut)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 504;
					outcome.transcript["timedOut"] = true;
					outcome.transcript["error"] =
						"scenario run deadline reached while waiting for event";
					outcome.failed = true;
					if (!modalResponses.empty())
						outcome.transcript["modalResponses"] =
							std::move(modalResponses);
					outcome.runTimedOut = true;
					return outcome;
				}
			}
		}

		StepOutcome ExecutePredicate(
			const std::shared_ptr<RunRecord>& a_record,
			const Step&                       a_step,
			std::size_t                       a_index,
			std::size_t                       a_repetition,
			Clock::time_point                 a_runDeadline)
		{
			StepOutcome outcome;
			outcome.transcript = StepBase(a_step, a_index, a_repetition);
			const ValuePredicate& predicate = *a_step.predicate;
			const bool            waiting = a_step.kind == StepKind::kWaitUntil;
			const auto            stepDeadline =
				waiting ? Clock::now() + a_step.timeout : Clock::now();
			PredicateObservation last;

			for (;;)
			{
				last = ObserveValue(predicate, registry, a_record->context);
				ApplyObservation(outcome.transcript, predicate, last);
				if (last.observed && last.satisfied)
				{
					outcome.transcript["ok"] = true;
					outcome.transcript["status"] = 200;
					outcome.transcript["result"] =
						json{ { "satisfied", true }, { "source", last.source } };
					return outcome;
				}

				if (!waiting)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] =
						last.status == 200 ? 422 : last.status;
					outcome.transcript["error"] = last.error.empty() ?
					                                  std::string("VALUE assertion was not satisfied") :
					                                  last.error;
					outcome.failed = true;
					return outcome;
				}
				// Native operation fields are null until their outcomes are known.
				const bool awaitingValue = last.observed && last.available &&
				                           last.actual.is_null() && !predicate.expected.is_null();
				if (last.observed && last.status != 200 && !awaitingValue)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = last.status;
					outcome.transcript["error"] = last.error;
					outcome.failed = true;
					return outcome;
				}
				if (!last.observed && !last.retryable)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = last.status;
					outcome.transcript["error"] = last.error;
					outcome.failed = true;
					return outcome;
				}

				const auto now = Clock::now();
				if (now >= a_runDeadline)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 504;
					outcome.transcript["timedOut"] = true;
					outcome.transcript["error"] =
						"scenario run deadline reached while waiting for condition";
					outcome.failed = true;
					outcome.runTimedOut = true;
					return outcome;
				}
				if (now >= stepDeadline)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 504;
					outcome.transcript["timedOut"] = true;
					outcome.transcript["error"] =
						std::format(
							"waitUntil timed out after {} ms; last observation: {}",
							a_step.timeout.count(),
							last.error.empty() ?
								(last.available ? last.actual.dump() : "<missing>") :
								last.error);
					outcome.failed = true;
					return outcome;
				}

				const StopReason stop = WaitInterruptibly(
					a_record,
					std::min(stepDeadline, now + a_step.poll),
					a_runDeadline);
				if (stop == StopReason::kCancelled)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = kCancelledStatus;
					outcome.transcript["error"] =
						"scenario cancelled while waiting for condition";
					outcome.failed = true;
					outcome.cancelled = true;
					return outcome;
				}
				if (stop == StopReason::kTimedOut)
				{
					outcome.transcript["ok"] = false;
					outcome.transcript["status"] = 504;
					outcome.transcript["timedOut"] = true;
					outcome.transcript["error"] =
						"scenario run deadline reached while waiting for condition";
					outcome.failed = true;
					outcome.runTimedOut = true;
					return outcome;
				}
			}
		}
	};

	ToolDescriptor BuildScenarioDescriptor(const ScenarioConfig& a_config)
	{
		ToolDescriptor descriptor;
		descriptor.name = "scenario";
		descriptor.description = std::format(
			"Run a validated sequence server-side and return a per-step transcript. "
			"action='run' is synchronous by default; async:true returns a runId for status/cancel. "
			"Steps use the upstream tool, wait, waitFor, waitUntil, and assert vocabulary. "
			"Tool steps dispatch through the common registry, so the target handler's permissions "
			"and validation remain authoritative. A valid result.eventCursor may refine the "
			"pre-dispatch event cursor used by the following waitFor; queued game operations "
			"are additionally correlated by operationId. pose validates all five finite values "
			"before lowering through the console tool. waitFor accepts lifecycle/menu shorthands, "
			"{{topic,match}}, and the narrowly gated replay missing-content modal response. "
			"waitUntil supports playerLoaded/noModal/noMenu/noBlockingMenu or a VALUE predicate; "
			"assert supports scene/noBlockingMenu or a VALUE predicate over a "
			"registered readOnly tool: {{tool,args,path,eq|ne|lt|le|gt|ge|exists}}. "
			"Only one run executes at a time. Limits: repeat<=1000, expandedSteps<={}, "
			"wait/timeout<={}ms, run<={}ms, transcript<={} bytes, retained histories={}. "
			"Cancellation is cooperative and cannot undo or preempt a tool callback already running.",
			a_config.maxExpandedSteps,
			std::max(a_config.maxWait, a_config.maxStepTimeout).count(),
			a_config.maxRunTime.count(),
			a_config.maxTranscriptBytes,
			a_config.historyLimit);
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "additionalProperties", false },
			{ "properties", json{
								{ "action", json{
												{ "type", "string" },
												{ "enum", json::array({ "run", "status", "cancel" }) },
												{ "default", "run" },
											} },
								{ "steps", json{
											   { "type", "array" },
											   { "maxItems", a_config.maxExpandedSteps },
										   } },
								{ "repeat", json{
												{ "type", "integer" },
												{ "minimum", 1 },
												{ "maximum", 1000 },
												{ "default", 1 },
											} },
								{ "continueOnError", json{ { "type", "boolean" }, { "default", false } } },
								{ "async", json{ { "type", "boolean" }, { "default", false } } },
								{ "runId", json{ { "type", "integer" }, { "minimum", 1 } } },
							} },
		};
		return descriptor;
	}

	ScenarioService::ScenarioService(
		ToolRegistry&  a_registry,
		EventBus&      a_events,
		ScenarioConfig a_config) :
		impl_(std::make_unique<Impl>(
			a_registry, a_events, std::move(a_config)))
	{}

	ScenarioService::~ScenarioService() = default;

	bool ScenarioService::Register()
	{
		return impl_->Register();
	}

	json ScenarioService::Run(
		const json& a_args, const ToolContext& a_context,
		ScenarioRunOptions a_options)
	{
		json args = a_args;
		if (!args.is_object())
			throw ToolError(400, "scenario arguments must be an object");
		args["action"] = "run";
		return impl_->Handle(args, a_context, std::move(a_options));
	}

	json ScenarioService::Status(std::uint64_t a_runId)
	{
		return impl_->Status(a_runId);
	}

	json ScenarioService::Cancel(std::uint64_t a_runId)
	{
		return impl_->Cancel(a_runId);
	}

	void ScenarioService::Shutdown()
	{
		impl_->Shutdown();
	}
}

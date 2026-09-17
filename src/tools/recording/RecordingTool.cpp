#include "tools/recording/RecordingTool.h"

#include "Config.h"
#include "io/WindowsPaths.h"
#include "tools/GameTool.h"
#include "tools/ToolPermissions.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <utility>

namespace dvb::tools::recording
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Milliseconds = std::chrono::milliseconds;
		constexpr std::size_t kReplayPlanningBudget = 9900;

		enum class RecorderPhase
		{
			kIdle,
			kStarting,
			kRecording,
			kStopping,
		};

		std::string_view PhaseName(RecorderPhase a_phase)
		{
			switch (a_phase)
			{
				case RecorderPhase::kIdle:
					return "idle";
				case RecorderPhase::kStarting:
					return "starting";
				case RecorderPhase::kRecording:
					return "recording";
				case RecorderPhase::kStopping:
					return "stopping";
			}
			return "unknown";
		}

		std::string LowerAscii(std::string a_value)
		{
			std::ranges::transform(a_value, a_value.begin(), [](unsigned char a_ch) {
				return static_cast<char>(
					a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return a_value;
		}

		void RequireObject(const json& a_args, std::string_view a_subject)
		{
			if (!a_args.is_object())
				throw ToolError(
					400, std::format("{} arguments must be an object", a_subject));
		}

		void ValidateOnly(
			const json& a_args,
			std::initializer_list<std::string_view> a_allowed,
			std::string_view a_subject)
		{
			for (const auto& [key, value] : a_args.items())
			{
				(void)value;
				if (std::ranges::find(a_allowed, key) == a_allowed.end())
					throw ToolError(
						400,
						std::format(
							"unexpected parameter '{}' for {}", key, a_subject));
			}
		}

		bool ReadBool(
			const json& a_args, std::string_view a_name, bool a_default)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return a_default;
			if (!it->is_boolean())
				throw ToolError(
					400, std::format("'{}' must be a boolean", a_name));
			return it->get<bool>();
		}

		std::string ReadString(
			const json& a_args, std::string_view a_name,
			std::string a_default = {})
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return a_default;
			if (!it->is_string())
				throw ToolError(
					400, std::format("'{}' must be a string", a_name));
			return it->get<std::string>();
		}

		std::int64_t ReadInteger(
			const json& a_args, std::string_view a_name,
			std::int64_t a_default, std::int64_t a_min,
			std::int64_t a_max)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return a_default;
			if (!it->is_number_integer() && !it->is_number_unsigned())
				throw ToolError(
					400, std::format("'{}' must be an integer", a_name));
			if (it->is_number_unsigned() &&
				it->get<std::uint64_t>() >
					static_cast<std::uint64_t>(a_max))
				throw ToolError(
					400,
					std::format(
						"'{}' must be between {} and {}", a_name, a_min, a_max));
			const auto value = it->is_number_unsigned() ?
			                       static_cast<std::int64_t>(it->get<std::uint64_t>()) :
			                       it->get<std::int64_t>();
			if (value < a_min || value > a_max)
				throw ToolError(
					400,
					std::format(
						"'{}' must be between {} and {}", a_name, a_min, a_max));
			return value;
		}

		std::string Trim(std::string a_value)
		{
			const auto first = a_value.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return {};
			const auto last = a_value.find_last_not_of(" \t\r\n");
			return a_value.substr(first, last - first + 1);
		}

		struct ConsoleAction
		{
			std::string verb;
			std::string value;
		};

		ConsoleAction ParseConsoleAction(std::string_view a_command)
		{
			const auto split = a_command.find_first_of(" \t");
			auto verb = LowerAscii(std::string(a_command.substr(0, split)));
			auto value = split == std::string_view::npos ?
			                 std::string{} :
			                 Trim(std::string(a_command.substr(split + 1)));
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
				value = value.substr(1, value.size() - 2);
			return { std::move(verb), std::move(value) };
		}

		bool IsRollingSave(std::string_view a_name)
		{
			const auto lower = LowerAscii(std::string(a_name));
			return lower.starts_with("autosave") ||
			       lower.starts_with("quicksave");
		}

		std::string SceneKey(const json& a_snapshot)
		{
			const auto scene = a_snapshot.value("scene", json::object());
			const auto cell = scene.value("cell", json(nullptr));
			const auto worldspace = scene.value("worldspace", json(nullptr));
			return std::format(
				"{}:{}:{}",
				scene.value("interior", false),
				cell.is_object() ? cell.value("formIdHex", std::string{}) : "",
				worldspace.is_object() ?
					worldspace.value("formIdHex", std::string{}) :
					"");
		}

		std::string SceneIdentityName(const json& a_meta, std::string_view a_key)
		{
			const auto value = a_meta.find(a_key);
			if (value == a_meta.end())
				return {};
			if (value->is_string())
				return value->get<std::string>();
			if (!value->is_object())
				return {};
			if (const auto editorId = value->find("editorId");
				editorId != value->end() && editorId->is_string() &&
				!editorId->get_ref<const std::string&>().empty())
				return editorId->get<std::string>();
			if (const auto formId = value->find("formIdHex");
				formId != value->end() && formId->is_string())
				return formId->get<std::string>();
			return {};
		}

		std::string BuildCowCommand(const json& a_target)
		{
			const auto worldspace =
				a_target.value("worldspace", json(nullptr));
			std::uint64_t formId = 0;
			if (worldspace.is_object())
			{
				const auto value = worldspace.find("formId");
				if (value != worldspace.end())
				{
					if (!value->is_number_integer() &&
						!value->is_number_unsigned())
						throw ToolError(
							422,
							"recorded exterior WRLD formId must be an integer");
					if (value->is_number_unsigned())
						formId = value->get<std::uint64_t>();
					else
					{
						const auto signedValue = value->get<std::int64_t>();
						if (signedValue > 0)
							formId = static_cast<std::uint64_t>(signedValue);
					}
				}
			}
			if (formId == 0)
			{
				const auto value = a_target.find("worldspaceFormID");
				if (value != a_target.end())
				{
					if (!value->is_number_integer() &&
						!value->is_number_unsigned())
						throw ToolError(
							422,
							"recorded exterior worldspaceFormID must be an integer");
					if (value->is_number_unsigned())
						formId = value->get<std::uint64_t>();
					else
					{
						const auto signedValue = value->get<std::int64_t>();
						if (signedValue > 0)
							formId = static_cast<std::uint64_t>(signedValue);
					}
				}
			}
			if (formId == 0 ||
				formId > std::numeric_limits<std::uint32_t>::max())
				throw ToolError(
					422,
					"recorded exterior transition requires a valid numeric WRLD FormID");

			const auto anchor =
				a_target.value("anchor", json::object());
			const auto ReadCoordinate =
				[&anchor](std::string_view a_name) -> double {
				const auto value = anchor.find(a_name);
				if (value == anchor.end() || !value->is_number())
					throw ToolError(
						422,
						std::format(
							"recorded exterior transition requires numeric anchor.{}",
							a_name));
				const auto coordinate = value->get<double>();
				if (!std::isfinite(coordinate))
					throw ToolError(
						422,
						std::format(
							"recorded exterior anchor.{} must be finite",
							a_name));
				return coordinate;
			};
			const auto gridXValue = std::floor(ReadCoordinate("x") / 4096.0);
			const auto gridYValue = std::floor(ReadCoordinate("y") / 4096.0);
			constexpr auto minGrid =
				static_cast<double>(std::numeric_limits<std::int32_t>::min());
			constexpr auto maxGrid =
				static_cast<double>(std::numeric_limits<std::int32_t>::max());
			if (gridXValue < minGrid || gridXValue > maxGrid ||
				gridYValue < minGrid || gridYValue > maxGrid)
				throw ToolError(
					422, "recorded exterior anchor exceeds COW grid range");

			return std::format(
				"COW {:08X} {} {}",
				static_cast<std::uint32_t>(formId),
				static_cast<std::int32_t>(gridXValue),
				static_cast<std::int32_t>(gridYValue));
		}

		std::optional<std::int64_t> EventTime(const json& a_event)
		{
			const auto it = a_event.find("tMs");
			if (it == a_event.end() ||
				(!it->is_number_integer() && !it->is_number_unsigned()))
				return std::nullopt;
			if (!it->is_number_unsigned() && it->get<std::int64_t>() < 0)
				return std::nullopt;
			if (it->is_number_unsigned() &&
				it->get<std::uint64_t>() >
					static_cast<std::uint64_t>(
						std::numeric_limits<std::int64_t>::max()))
				return std::nullopt;
			return it->is_number_unsigned() ?
			           static_cast<std::int64_t>(it->get<std::uint64_t>()) :
			           it->get<std::int64_t>();
		}

		json BuildRecordedSteps(const std::vector<json>& a_samples)
		{
			json steps = json::array();
			std::int64_t previous = 0;
			for (const auto& sample : a_samples)
			{
				const auto at = sample.value("tMs", previous);
				json       step{
					{ "pose", sample.at("pose") },
					{ "atMs", at },
					{ "wait", std::max<std::int64_t>(0, at - previous) },
				};
				steps.push_back(std::move(step));
				previous = at;
			}
			return steps;
		}

		json ActivityCounts(const std::vector<json>& a_events)
		{
			json counts = json::object();
			for (const auto& event : a_events)
			{
				const auto kind = event.value("kind", std::string("unknown"));
				counts[kind] = counts.value(kind, 0) + 1;
			}
			return counts;
		}

		std::size_t ProjectedReplayUnits(
			std::size_t a_samples, std::size_t a_activity,
			std::size_t a_checkpoints)
		{
			return a_samples + (4 * a_activity) + (4 * a_checkpoints);
		}

		json CheckpointSummary(const json& a_result)
		{
			json checkpoints = json::array();
			for (const auto& step : a_result.value("results", json::array()))
			{
				if (step.value("tool", std::string{}) != "capture" ||
					!step.contains("result"))
					continue;
				const auto& capture = step.at("result");
				if (!capture.contains("checkpointId"))
					continue;
				json item{
					{ "id", capture.at("checkpointId") },
					{ "ok", step.value("ok", false) },
					{ "path", capture.value("path", std::string{}) },
					{ "inconclusive", capture.value("inconclusive", false) },
				};
				for (const auto key :
					{ "inconclusiveReason", "ssim", "threshold", "passed" })
					if (capture.contains(key))
						item[key] = capture[key];
				checkpoints.push_back(std::move(item));
			}
			return checkpoints;
		}

		json ReadGameSaves(ToolRegistry& a_registry, const ToolContext& a_context)
		{
			ToolContext context = a_context;
			context.internal = true;
			const auto result = a_registry.Invoke(
				"game",
				json{
					{ "action", "list" },
					{ "limit", kMaxSaveListLimit },
				},
				context);
			if (!result.ok)
				throw ToolError(result.errorCode, result.errorMessage);
			return result.value;
		}

		std::string ResolveSaveForReplay(
			ToolRegistry& a_registry, const ToolContext& a_context,
			std::string_view a_recordedName, bool a_rolling)
		{
			const auto listing = ReadGameSaves(a_registry, a_context);
			const auto saves = listing.value("saves", json::array());
			if (a_rolling)
			{
				if (saves.empty() || !saves.front().is_object())
					throw ToolError(404, "no Fallout 4 save is available for rolling-slot restoration");
				return saves.front().value("name", std::string{});
			}
			for (const auto& save : saves)
				if (save.is_object() &&
					save.value("name", std::string{}) == a_recordedName)
					return std::string(a_recordedName);
			throw ToolError(
				404,
				std::format(
					"named recording fixture '{}' is missing; refusing loadLast fallback",
					a_recordedName));
		}

		struct TimedStep
		{
			std::int64_t atMs = 0;
			std::size_t order = 0;
			json        step;
		};

		json BuildCaptureStep(
			const json& a_checkpoint, const json& a_args,
			std::string_view a_recordingStem)
		{
			json captureArgs{
				{ "kind", "auto" },
				{ "allowNative", false },
				{ "checkpointId", a_checkpoint.at("id") },
				{ "recording", a_recordingStem },
				{ "variant", a_args.value("variant", std::string("default")) },
				{ "excludeUi", a_checkpoint.value("excludeUi", true) },
				{ "atMs", a_checkpoint.at("atMs") },
			};
			const auto metaCapability = a_args.value("_captureCapability", json::object());
			if (metaCapability.is_object())
			{
				const auto provider =
					metaCapability.value("provider", std::string{});
				if (!provider.empty())
					captureArgs["kind"] = provider;
				captureArgs["allowNative"] =
					metaCapability.value("allowNative", false);
			}
			for (const auto key : { "settleMs", "subrect" })
				if (a_checkpoint.contains(key))
					captureArgs[key] = a_checkpoint[key];
			const auto goldens = a_args.value("goldens", json::object());
			const auto id = a_checkpoint.at("id").get<std::string>();
			if (goldens.is_object() && goldens.contains(id) &&
				goldens.at(id).is_object())
			{
				for (const auto key : { "golden", "threshold", "regions" })
					if (goldens.at(id).contains(key))
						captureArgs[key] = goldens.at(id).at(key);
			}
			return json{
				{ "tool", "capture" },
				{ "args", std::move(captureArgs) },
				{ "label", std::format("checkpoint:{}", id) },
			};
		}

		json TransitionStep(json a_target)
		{
			return json{
				{ "tool", "record" },
				{ "args",
					json{
						{ "action", "transition" },
						{ "target", std::move(a_target) },
					} },
			};
		}

		json TransitionWaitStep()
		{
			return json{
				{ "waitFor", "loadingMenuClosed" },
				{ "timeoutMs", 60000 },
				{ "pollMs", 100 },
			};
		}

		json PlayerReadyStep()
		{
			return json{
				{ "waitUntil", "playerLoaded" },
				{ "timeoutMs", 60000 },
				{ "pollMs", 100 },
			};
		}
	}

	struct RecordingService::State :
		public std::enable_shared_from_this<RecordingService::State>
	{
		ToolRegistry&              registry;
		EventBus&                  events;
		scenario::ScenarioService& scenarios;
		bool                       allowGameActions;
		RecordingConfiguration     configuration;
		RecordingBackend           backend;
		RecordingStore             store;

		std::mutex              mutex;
		std::condition_variable cv;
		RecorderPhase           phase = RecorderPhase::kIdle;
		bool                    stopping = false;
		bool                    stopRequested = false;
		bool                    replaying = false;
		std::optional<std::uint64_t> replayStartCursor;
		std::optional<std::uint64_t> replaySuppressionEnd;
		bool                    registered = false;
		bool                    limitReached = false;
		std::string             limitReason;
		std::thread             worker;
		EventBus::SubId         subscription = 0;
		std::uint64_t           generation = 0;
		Clock::time_point       started;
		Milliseconds            interval{ 10 };
		std::string             correlationId;
		json                    manifest;
		std::vector<json>       samples;
		std::vector<json>       activity;
		std::vector<json>       checkpoints;
		std::string             lastSceneKey;

		struct EntryPoint
		{
			std::string       kind = "unknown";
			std::string       value;
			json              target = json::object();
			Clock::time_point observed{};
		};
		EntryPoint                 entry;
		std::optional<std::string> pendingLoadName;

		State(
			ToolRegistry& a_registry, EventBus& a_events,
			scenario::ScenarioService& a_scenarios,
			bool a_allowGameActions,
			RecordingConfiguration a_configuration,
			RecordingBackend a_backend) :
			registry(a_registry),
			events(a_events),
			scenarios(a_scenarios),
			allowGameActions(a_allowGameActions),
			configuration(std::move(a_configuration)),
			backend(std::move(a_backend)),
			store(configuration.root)
		{
			if (!backend.snapshot)
				throw std::invalid_argument("recording backend requires snapshot");
			if (configuration.sampleInterval.count() < 10)
				configuration.sampleInterval = Milliseconds(10);
		}

		~State()
		{
			Shutdown();
		}

		void Subscribe()
		{
			std::weak_ptr<State> weak = shared_from_this();
			subscription = events.Subscribe([weak](const EventBus::Event& a_event) {
				if (const auto state = weak.lock())
					state->ObserveEvent(a_event);
			});
		}

		bool Register()
		{
			{
				const std::lock_guard lock{ mutex };
				if (registered)
					return false;
				if (stopping)
					throw std::logic_error("cannot register a stopped RecordingService");
				registered = true;
			}
			const bool recordAdded = registry.Register(
				BuildRecordDescriptor(),
				[weak = weak_from_this()](
					const json& a_args, const ToolContext& a_context) {
					const auto state = weak.lock();
					if (!state)
						throw ToolError(503, "recording service is unavailable");
					return state->HandleRecord(a_args, a_context);
				});
			const bool recordingsAdded = registry.Register(
				BuildRecordingsDescriptor(),
				[weak = weak_from_this()](
					const json& a_args, const ToolContext& a_context) {
					const auto state = weak.lock();
					if (!state)
						throw ToolError(503, "recording service is unavailable");
					return state->HandleRecordings(a_args, a_context);
				});
			return recordAdded && recordingsAdded;
		}

		void Shutdown() noexcept
		{
			EventBus::SubId unsubscribe = 0;
			{
				const std::lock_guard lock{ mutex };
				if (stopping)
					return;
				stopping = true;
				stopRequested = true;
				unsubscribe = std::exchange(subscription, 0);
			}
			cv.notify_all();
			if (worker.joinable())
				worker.join();
			if (unsubscribe)
				events.Unsubscribe(unsubscribe);
		}

		void ObserveEvent(const EventBus::Event& a_event)
		{
			const auto now = Clock::now();
			const std::lock_guard lock{ mutex };
			const bool brokeredConsole =
				a_event.topic == "console.command" &&
				a_event.payload.value("source", std::string{}) == "broker";
			const bool inReplayRange =
				replayStartCursor && a_event.seq > *replayStartCursor &&
				(replaying ||
					(replaySuppressionEnd &&
						a_event.seq <= *replaySuppressionEnd));
			if (inReplayRange &&
				(a_event.topic != "console.command" || brokeredConsole))
				return;
			if (replaySuppressionEnd &&
				a_event.seq > *replaySuppressionEnd)
			{
				replayStartCursor.reset();
				replaySuppressionEnd.reset();
			}
			if (a_event.topic == "lifecycle")
			{
				const auto event =
					a_event.payload.value("event", std::string{});
				if (event == "preLoadGame")
				{
					const auto name =
						a_event.payload.value("name", std::string{});
					pendingLoadName =
						name.empty() ? std::nullopt :
						               std::optional<std::string>(name);
				}
				else if (
					event == "postLoadGame" &&
					a_event.payload.value("success", false))
				{
					entry.kind = pendingLoadName ? "save" : "unknown";
					entry.value = pendingLoadName.value_or(std::string{});
					entry.target = json::object();
					entry.observed = now;
					pendingLoadName.reset();
				}
			}
			else if (a_event.topic == "console.command")
			{
				const auto command =
					a_event.payload.value("command", std::string{});
				const auto parsed = ParseConsoleAction(command);
				if (parsed.verb == "coc" || parsed.verb == "cow")
				{
					entry.kind = "console";
					entry.value = command;
					entry.target = json::object();
					entry.observed = now;
				}
			}

			if (phase != RecorderPhase::kRecording || replaying)
				return;
			if (activity.size() >= kMaximumActivityEvents)
			{
				limitReached = true;
				limitReason = "maximum retained activity event budget reached";
				stopRequested = true;
				cv.notify_all();
				return;
			}

			std::string kind;
			if (a_event.topic == "console.command")
				kind = "console";
			else if (a_event.topic == "lifecycle")
				kind = "lifecycle";
			else if (a_event.topic == "menu")
				kind = "menu";
			else
				return;
			if (ProjectedReplayUnits(
					samples.size(), activity.size() + 1, checkpoints.size()) >
				kReplayPlanningBudget)
			{
				limitReached = true;
				limitReason =
					"maximum replayable combined sample/activity budget reached";
				stopRequested = true;
				cv.notify_all();
				return;
			}
			json item = a_event.payload;
			item["kind"] = kind;
			item["eventSeq"] = a_event.seq;
			item["frame"] = a_event.frame;
			item["tMs"] = std::chrono::duration_cast<Milliseconds>(
				now - started)
			                  .count();
			activity.push_back(std::move(item));
		}

		json Start(const json& a_args)
		{
			ValidateOnly(
				a_args,
				{ "action", "intervalMs", "allowNoPlayer", "correlationId" },
				"record start");
			const auto requestedInterval = Milliseconds(ReadInteger(
				a_args,
				"intervalMs",
				configuration.sampleInterval.count(),
				10,
				kMaximumRecordingDuration.count()));
			const auto allowNoPlayer =
				ReadBool(a_args, "allowNoPlayer", false);
			const auto requestedCorrelation =
				ReadString(a_args, "correlationId");
			if (requestedCorrelation.size() > 128)
				throw ToolError(
					400, "'correlationId' must contain at most 128 bytes");

			{
				const std::lock_guard lock{ mutex };
				if (phase != RecorderPhase::kIdle)
					throw ToolError(
						409,
						std::format(
							"recorder is not idle (state={})", PhaseName(phase)));
				phase = RecorderPhase::kStarting;
			}

			json initial;
			try
			{
				initial = backend.snapshot();
				if (!initial.is_object())
					throw ToolError(
						500, "recording snapshot returned a non-object");
				if (!initial.value("playerLoaded", false) && !allowNoPlayer)
					throw ToolError(
						409,
						"player is not loaded; pass allowNoPlayer=true only for a main-menu trace");
			}
			catch (...)
			{
				const std::lock_guard lock{ mutex };
				phase = RecorderPhase::kIdle;
				throw;
			}

			{
				const std::lock_guard lock{ mutex };
				samples.clear();
				activity.clear();
				checkpoints.clear();
				limitReached = false;
				limitReason.clear();
				stopRequested = false;
				interval = requestedInterval;
				correlationId = requestedCorrelation;
				started = Clock::now();
				lastSceneKey = SceneKey(initial);
				manifest = initial.value("scene", json::object());
				manifest["game"] = "fo4";
				manifest["format"] = kRecordingFormat;
				manifest["runtime"] = json{
					{ "recordedOnVR", false },
					{ "compat", json::array({ "ae" }) },
				};
				manifest["startState"] =
					initial.value("playerLoaded", false) ?
						"playerLoaded" :
						"noPlayer";
				manifest["entryPoint"] = json{
					{ "kind", entry.kind },
					{ "value", entry.value },
					{ "target", entry.target },
				};
				if (entry.observed.time_since_epoch().count() != 0)
					manifest["entryPoint"]["ageMs"] =
						std::chrono::duration_cast<Milliseconds>(
							started - entry.observed)
							.count();
				if (!correlationId.empty())
					manifest["correlationId"] = correlationId;
				const auto currentGeneration = ++generation;
				phase = RecorderPhase::kRecording;
				try
				{
					worker = std::thread(
						[self = shared_from_this(), currentGeneration] {
							self->SampleLoop(currentGeneration);
						});
				}
				catch (...)
				{
					phase = RecorderPhase::kIdle;
					throw;
				}
			}
			events.Publish(
				"record.started",
				json{
					{ "intervalMs", interval.count() },
					{ "anchored", initial.value("playerLoaded", false) },
					{ "format", kRecordingFormat },
				});
			return json{
				{ "action", "start" },
				{ "recording", true },
				{ "intervalMs", interval.count() },
				{ "anchored", initial.value("playerLoaded", false) },
				{ "correlationId", correlationId },
				{ "format", kRecordingFormat },
				{ "capabilities",
					json{
						{ "pose", true },
						{ "console", true },
						{ "lifecycle", true },
						{ "vrTracking", false },
					} },
			};
		}

		void SampleLoop(std::uint64_t a_generation)
		{
			for (;;)
			{
				{
					std::unique_lock lock{ mutex };
					const auto deadline =
						std::min(started + kMaximumRecordingDuration,
							Clock::now() + interval);
					cv.wait_until(lock, deadline, [&] {
						return stopping || stopRequested ||
						       generation != a_generation ||
						       phase != RecorderPhase::kRecording;
					});
					if (stopping || stopRequested ||
						generation != a_generation ||
						phase != RecorderPhase::kRecording)
						return;
					if (Clock::now() >= started + kMaximumRecordingDuration)
					{
						limitReached = true;
						limitReason = "maximum recording duration reached";
						stopRequested = true;
						return;
					}
				}

				json snapshot;
				try
				{
					snapshot = backend.snapshot();
				}
				catch (...)
				{
					continue;
				}
				if (!snapshot.is_object())
					continue;

				const auto now = Clock::now();
				const auto elapsed =
					std::chrono::duration_cast<Milliseconds>(now - started).count();
				const std::lock_guard lock{ mutex };
				if (phase != RecorderPhase::kRecording ||
					generation != a_generation || replaying)
					continue;
				const auto sceneKey = SceneKey(snapshot);
				if (!sceneKey.empty() && !lastSceneKey.empty() &&
					sceneKey != lastSceneKey &&
					activity.size() < kMaximumActivityEvents)
				{
					activity.push_back(json{
						{ "kind", "cell" },
						{ "target", snapshot.value("scene", json::object()) },
						{ "frame", snapshot.value("frame", -1) },
						{ "tMs", elapsed },
					});
				}
				if (!sceneKey.empty())
					lastSceneKey = sceneKey;
				const auto pose = snapshot.find("pose");
				if (pose == snapshot.end() || !pose->is_array())
					continue;
				if (samples.size() >= kMaximumTrajectorySamples)
				{
					limitReached = true;
					limitReason = "maximum retained trajectory sample budget reached";
					stopRequested = true;
					return;
				}
				if (ProjectedReplayUnits(
						samples.size() + 1, activity.size(),
						checkpoints.size()) > kReplayPlanningBudget)
				{
					limitReached = true;
					limitReason =
						"maximum replayable combined sample/activity budget reached";
					stopRequested = true;
					return;
				}
				samples.push_back(json{
					{ "pose", *pose },
					{ "frame", snapshot.value("frame", -1) },
					{ "tMs", elapsed },
				});
			}
		}

		json Checkpoint(const json& a_args)
		{
			ValidateOnly(
				a_args,
				{ "action", "id", "excludeUi", "settleMs", "subrect" },
				"record checkpoint");
			const auto id = ReadString(a_args, "id");
			if (id.empty() || id.size() > 120)
				throw ToolError(
					400, "checkpoint 'id' must contain 1..120 bytes");
			const auto excludeUi = ReadBool(a_args, "excludeUi", true);
			json       checkpoint;
			std::size_t checkpointCount = 0;
			{
				const std::lock_guard lock{ mutex };
				if (phase != RecorderPhase::kRecording)
					throw ToolError(
						409, "not recording; call action='start' first");
				if (checkpoints.size() >= kMaximumCheckpoints)
					throw ToolError(
						413,
						std::format(
							"checkpoint limit {} reached", kMaximumCheckpoints));
				if (ProjectedReplayUnits(
						samples.size(), activity.size(),
						checkpoints.size() + 1) > kReplayPlanningBudget)
					throw ToolError(
						413,
						"checkpoint would exceed the replayable scenario-step budget");
				if (std::ranges::any_of(checkpoints, [&](const json& a_item) {
						return a_item.value("id", std::string{}) == id;
					}))
					throw ToolError(
						409,
						std::format(
							"checkpoint id '{}' is already present", id));
				checkpoint = json{
					{ "id", id },
					{ "atMs",
						std::chrono::duration_cast<Milliseconds>(
							Clock::now() - started)
							.count() },
					{ "excludeUi", excludeUi },
				};
				if (a_args.contains("settleMs"))
					checkpoint["settleMs"] = ReadInteger(
						a_args, "settleMs",
						configuration.captureSettle.count(), 0, 60000);
				if (a_args.contains("subrect"))
					checkpoint["subrect"] = a_args.at("subrect");
				checkpoints.push_back(checkpoint);
				checkpointCount = checkpoints.size();
			}
			events.Publish("record.checkpoint", checkpoint);
			return json{
				{ "action", "checkpoint" },
				{ "checkpoint", checkpoint },
				{ "checkpointCount", checkpointCount },
			};
		}

		json Stop(const json& a_args)
		{
			ValidateOnly(a_args, { "action" }, "record stop");
			{
				const std::lock_guard lock{ mutex };
				if (phase != RecorderPhase::kRecording)
					throw ToolError(
						409,
						std::format("not recording (state={})", PhaseName(phase)));
				phase = RecorderPhase::kStopping;
				stopRequested = true;
			}
			cv.notify_all();
			if (worker.joinable())
				worker.join();

			json              document;
			std::vector<json> localSamples;
			std::vector<json> localActivity;
			std::vector<json> localCheckpoints;
			std::int64_t      recordedMs = 0;
			bool              reached = false;
			std::string       reason;
			{
				const std::lock_guard lock{ mutex };
				recordedMs = std::chrono::duration_cast<Milliseconds>(
					Clock::now() - started)
				                 .count();
				localSamples = samples;
				localActivity = activity;
				localCheckpoints = checkpoints;
				reached = limitReached;
				reason = limitReason;
				manifest["format"] = kRecordingFormat;
				manifest["sampleCount"] = localSamples.size();
				manifest["activityCounts"] = ActivityCounts(localActivity);
				manifest["recordedMs"] = recordedMs;
				manifest["recordedAt"] =
					static_cast<std::int64_t>(std::time(nullptr));
				manifest["intervalMs"] = interval.count();
				manifest["poseCapture"] = json{
					{ "name", "devbench.recording.pose" },
					{ "version", json{ { "major", 3 }, { "minor", 0 } } },
					{ "player",
						json::array({ "position", "yaw", "pitch" }) },
					{ "vrTracking", false },
				};
				manifest["activityCapture"] = json{
					{ "console", true },
					{ "lifecycle", true },
					{ "menu", true },
					{ "cell", true },
					{ "input", false },
				};
				if (!localCheckpoints.empty())
				{
					manifest["checkpoints"] = localCheckpoints;
					manifest["capabilities"] = json::array({
						json{
							{ "capability", "capture" },
							{ "required", true },
						},
					});
				}
				document = json{
					{ "meta", manifest },
					{ "samples", localSamples },
					{ "activityEvents", localActivity },
					{ "steps", BuildRecordedSteps(localSamples) },
				};
			}

			std::string file;
			try
			{
				(void)ParseRecording(document);
				file = store.WriteUnique(document);
			}
			catch (...)
			{
				const std::lock_guard lock{ mutex };
				phase = RecorderPhase::kIdle;
				throw;
			}
			{
				const std::lock_guard lock{ mutex };
				phase = RecorderPhase::kIdle;
			}
			events.Publish(
				"record.stopped",
				json{
					{ "file", file },
					{ "sampleCount", localSamples.size() },
					{ "activityCounts", ActivityCounts(localActivity) },
				});
			return json{
				{ "action", "stop" },
				{ "file", file },
				{ "path",
					io::PhysicalFilePath(store.Root() / file).generic_string() },
				{ "sampleCount", localSamples.size() },
				{ "checkpointCount", localCheckpoints.size() },
				{ "activityCounts", ActivityCounts(localActivity) },
				{ "recordedMs", recordedMs },
				{ "limitReached", reached },
				{ "limitReason", reason },
				{ "meta", document.at("meta") },
			};
		}

		json Status()
		{
			const std::lock_guard lock{ mutex };
			return json{
				{ "recording", phase == RecorderPhase::kRecording },
				{ "state", PhaseName(phase) },
				{ "replaying", replaying },
				{ "correlationId", correlationId },
				{ "sampleCount", samples.size() },
				{ "checkpointCount", checkpoints.size() },
				{ "activityCounts", ActivityCounts(activity) },
				{ "intervalMs", interval.count() },
				{ "limitReached", limitReached },
				{ "limitReason", limitReason },
				{ "maximumSamples", kMaximumTrajectorySamples },
				{ "maximumActivityEvents", kMaximumActivityEvents },
				{ "maximumDurationMs", kMaximumRecordingDuration.count() },
			};
		}

		json CaptureCapability(const json& a_meta) const
		{
			const auto capabilities =
				a_meta.value("capabilities", json::array());
			if (!capabilities.is_array())
				throw ToolError(400, "'meta.capabilities' must be an array");
			for (const auto& capability : capabilities)
				if (capability.is_object() &&
					capability.value("capability", std::string{}) == "capture")
					return capability;
			return json::object();
		}

		json BuildReplayPlan(
			const ParsedRecording& a_recording, const json& a_args,
			const ToolContext& a_context, std::string_view a_file)
		{
			const auto& document = a_recording.document;
			const auto  meta = document.value("meta", json::object());
			json        warnings = a_recording.warnings;
			const bool  force = ReadBool(a_args, "force", false);
			const bool  restoreScene = ReadBool(a_args, "restoreScene", false);
			const bool  captureCheckpoints =
				ReadBool(a_args, "captureCheckpoints", true);
			const bool hasCheckpoints =
				meta.contains("checkpoints") &&
				meta.at("checkpoints").is_array() &&
				!meta.at("checkpoints").empty();
			const auto settleMs = ReadInteger(
				a_args, "settleMs", configuration.loadSettle.count(), 0, 60000);
			const auto captureSettleMs = configuration.captureSettle.count();

			auto recordedEntry = meta.value("entryPoint", json::object());
			const auto entryKind =
				recordedEntry.value("kind", std::string("unknown"));
			const auto entryValue =
				recordedEntry.value("value", std::string{});
			const bool interior = meta.value("interior", false);
			const auto worldspaceFormId =
				meta.value("worldspaceFormID", std::uint32_t{ 0 });
			const auto cellFormId =
				meta.value("cellFormID", std::uint32_t{ 0 });
			const bool haveScene =
				interior ? cellFormId != 0 : worldspaceFormId != 0;

			const auto coupling = meta.value("coupling", json::object());
			const auto anchorMs = coupling.value(
				"anchorMs", configuration.couplingAnchor.count());
			const auto cellMs = coupling.value(
				"cellMs", configuration.couplingCell.count());
			auto tier = coupling.value("tier", std::string{});
			if (tier.empty())
			{
				if (recordedEntry.contains("ageMs"))
				{
					const auto age =
						recordedEntry.value("ageMs", std::int64_t{ 0 });
					tier = age <= anchorMs ?
					           "anchored" :
					           (age <= cellMs ? "cell" : "worldspace");
				}
				else
					tier = "cell";
			}
			const auto producerTier = tier;
			if (const auto overrideTier = ReadString(a_args, "coupling");
				!overrideTier.empty())
			{
				if (overrideTier != "anchored" &&
					overrideTier != "cell" &&
					overrideTier != "worldspace")
					throw ToolError(
						400,
						"'coupling' must be anchored, cell, or worldspace");
				tier = overrideTier;
			}

			json steps = json::array();
			json restoration{
				{ "requested", restoreScene },
				{ "restored", false },
				{ "entryKind", entryKind },
				{ "recordedValue", entryValue },
			};
			if (restoreScene && tier != "worldspace")
			{
				if (entryKind == "save" && !entryValue.empty())
				{
					const bool rolling = IsRollingSave(entryValue);
					const auto resolved = ResolveSaveForReplay(
						registry, a_context, entryValue, rolling);
					steps.push_back(json{
						{ "tool", "game" },
						{ "args",
							json{
								{ "action", "load" },
								{ "name", resolved },
							} },
					});
					steps.push_back(json{
						{ "waitFor",
							json{
								{ "topic", "lifecycle" },
								{ "match",
									json{
										{ "event", "postLoadGame" },
										{ "success", true },
									} },
							} },
						{ "timeoutMs", 60000 },
						{ "pollMs", 100 },
						{ "acceptModal",
							json{
								{ "matchBody", "no longer present" },
								{ "buttonIndex", 0 },
							} },
					});
					steps.push_back(json{
						{ "waitUntil", "playerLoaded" },
						{ "timeoutMs", 60000 },
						{ "pollMs", 100 },
					});
					restoration["scheduled"] = true;
					restoration["resolvedSave"] = resolved;
					restoration["rollingResolved"] = rolling;
				}
				else if (entryKind == "scene" || entryKind == "console")
				{
					json target = recordedEntry.value(
						"target", meta.value("scene", json::object()));
					if (target.empty())
						target = meta;
					const bool targetInterior =
						target.value("interior", interior);
					if (!backend.transition &&
						(targetInterior ||
							(configuration.cleanTransition &&
								!configuration.cleanTransitionCell.empty())))
						throw ToolError(
							501,
							"recorded CELL transition is unsupported by this runtime");
					if (configuration.cleanTransition &&
						!configuration.cleanTransitionCell.empty())
					{
						steps.push_back(TransitionStep(json{
							{ "interior", true },
							{ "editorCell",
								configuration.cleanTransitionCell },
						}));
						steps.push_back(TransitionWaitStep());
						steps.push_back(PlayerReadyStep());
						restoration["cleanTransition"] =
							configuration.cleanTransitionCell;
					}
					steps.push_back(TransitionStep(std::move(target)));
					steps.push_back(TransitionWaitStep());
					steps.push_back(PlayerReadyStep());
					restoration["scheduled"] = true;
					restoration["transition"] =
						targetInterior ? "typedCell" : "consoleCow";
				}
				else
				{
					restoration["warning"] =
						"recording entry point is unknown; scene was not restored";
				}
				if (restoration.value("restored", false) && settleMs > 0)
					steps.push_back(json{ { "wait", settleMs } });
			}
			else if (restoreScene && tier == "worldspace")
				restoration["warning"] =
					"worldspace coupling intentionally skipped entry restoration";

			if (haveScene)
				steps.push_back(json{
					{ "assert", "scene" },
					{ "interior", interior },
					{ "worldspaceFormID", worldspaceFormId },
					{ "cellFormID", cellFormId },
					{ "worldspace",
						SceneIdentityName(meta, "worldspace") },
					{ "cell", SceneIdentityName(meta, "cell") },
					{ "soft", force },
					{ "timeoutMs", 10000 },
				});
			if (meta.value("startState", std::string{}) != "noPlayer")
				steps.push_back(json{ { "assert", "noBlockingMenu" } });

			std::vector<TimedStep> timeline;
			std::size_t            order = 0;
			for (const auto& step : document.at("steps"))
			{
				if (const auto at = EventTime(step))
				{
					auto clean = step;
					clean.erase("atMs");
					clean.erase("wait");
					timeline.push_back({ *at, order++, std::move(clean) });
				}
				else
					steps.push_back(step);
			}

			json unsupported = json::array();
			std::map<std::string, std::int64_t> recentGameActions;
			for (const auto& event :
				document.value("activityEvents", json::array()))
			{
				const auto at = EventTime(event);
				if (!at)
					continue;
				const auto kind = event.value("kind", std::string{});
				if (kind == "console")
				{
					const auto command =
						event.value("command", std::string{});
					const auto parsed = ParseConsoleAction(command);
					if (parsed.verb == "save" || parsed.verb == "savegame" ||
						parsed.verb == "load" || parsed.verb == "loadgame")
					{
						if (parsed.value.empty())
						{
							unsupported.push_back(event);
							continue;
						}
						const auto action =
							parsed.verb.starts_with("save") ? "save" : "load";
						const auto key =
							std::format("{}:{}", action, parsed.value);
						const auto previous = recentGameActions.find(key);
						if (previous == recentGameActions.end() ||
							std::abs(*at - previous->second) > 1000)
						{
							recentGameActions[key] = *at;
							timeline.push_back({
								*at,
								order++,
								json{
									{ "tool", "game" },
									{ "args",
										json{
											{ "action", action },
											{ "name", parsed.value },
										} },
								},
							});
						}
					}
					else if (parsed.verb == "coc" || parsed.verb == "cow")
					{
						auto observed = event;
						observed["reason"] =
							"console cell transition is replayed only from an observed typed scene identity";
						unsupported.push_back(std::move(observed));
					}
					else if (!command.empty())
						timeline.push_back({
							*at,
							order++,
							json{
								{ "tool", "console" },
								{ "args",
									json{
										{ "action", "exec" },
										{ "command", command },
									} },
							},
						});
				}
				else if (kind == "lifecycle")
				{
					const auto name =
						event.value("name", std::string{});
					const auto lifecycle =
						event.value("event", std::string{});
					const auto action =
						lifecycle == "preSaveGame" ?
							std::string("save") :
							(lifecycle == "preLoadGame" ?
								 std::string("load") :
								 std::string{});
					if (!action.empty() && !name.empty())
					{
						const auto key = std::format("{}:{}", action, name);
						const auto previous = recentGameActions.find(key);
						if (previous == recentGameActions.end() ||
							std::abs(*at - previous->second) > 1000)
						{
							recentGameActions[key] = *at;
							timeline.push_back({
								*at,
								order++,
								json{
									{ "tool", "game" },
									{ "args",
										json{
											{ "action", action },
											{ "name", name },
										} },
								},
							});
						}
					}
				}
				else if (kind == "cell" && event.contains("target"))
				{
					timeline.push_back({
						*at,
						order++,
						TransitionStep(event.at("target")),
					});
					timeline.push_back({
						*at,
						order++,
						TransitionWaitStep(),
					});
					timeline.push_back({
						*at,
						order++,
						PlayerReadyStep(),
					});
				}
				else if (kind != "menu")
					unsupported.push_back(event);
			}

			json captureCapability = CaptureCapability(meta);
			if (captureCheckpoints && !captureCapability.empty() &&
				captureCapability.value("required", true))
			{
				if (!registry.Has("capture") && !force)
					throw ToolError(
						409,
						"recording requires capture checkpoints but the capture tool is unavailable");
			}
			json replayArgs = a_args;
			replayArgs["_captureCapability"] = captureCapability;
			if (captureCheckpoints && hasCheckpoints &&
				!registry.Has("capture"))
			{
				if (!force)
					throw ToolError(
						409,
						"recording has capture checkpoints but the capture tool is unavailable");
				warnings.push_back(
					"capture checkpoints were skipped because the capture tool is unavailable");
			}
			else if (captureCheckpoints)
			{
				for (const auto& checkpoint :
					meta.value("checkpoints", json::array()))
				{
					auto step = BuildCaptureStep(
						checkpoint, replayArgs,
						std::filesystem::path(a_file).stem().string());
					const auto checkpointSettle = checkpoint.value(
						"settleMs", captureSettleMs);
					if (checkpointSettle > 0)
						step["beforeWaitMs"] = checkpointSettle;
					timeline.push_back({
						checkpoint.at("atMs").get<std::int64_t>(),
						order++,
						std::move(step),
					});
				}
			}

			std::ranges::stable_sort(
				timeline, [](const TimedStep& a_left, const TimedStep& a_right) {
					return a_left.atMs < a_right.atMs;
				});
			for (std::size_t index = 0; index < timeline.size(); ++index)
			{
				auto& item = timeline[index];
				if (item.step.contains("beforeWaitMs"))
				{
					const auto wait = item.step.at("beforeWaitMs");
					item.step.erase("beforeWaitMs");
					steps.push_back(json{ { "waitUntil", "noBlockingMenu" },
						{ "timeoutMs", 5000 }, { "pollMs", 100 } });
					steps.push_back(json{ { "assert", "noBlockingMenu" } });
					steps.push_back(json{ { "wait", wait } });
				}
				steps.push_back(std::move(item.step));
				if (index + 1 < timeline.size())
				{
					const auto wait =
						std::max<std::int64_t>(
							0, timeline[index + 1].atMs - item.atMs);
					if (wait > 0)
					{
						auto& previous = steps.back();
						if (previous.contains("pose") &&
							!previous.contains("wait"))
							previous["wait"] = wait;
						else
							steps.push_back(json{ { "wait", wait } });
					}
				}
			}

			if (steps.size() > kMaximumTrajectorySamples)
				throw ToolError(
					413,
					std::format(
						"expanded replay contains {} steps; the scenario limit is {}",
						steps.size(), kMaximumTrajectorySamples));
			return json{
				{ "steps", std::move(steps) },
				{ "restoration", std::move(restoration) },
				{ "coupling",
					json{
						{ "tier", tier },
						{ "producer", producerTier },
						{ "overridden", tier != producerTier },
						{ "forced", force },
					} },
				{ "activity",
					json{
						{ "unsupported", std::move(unsupported) },
						{ "feedbackSuppressed", true },
					} },
				{ "warnings", std::move(warnings) },
			};
		}

		json Replay(const json& a_args, const ToolContext& a_context)
		{
			ValidateOnly(
				a_args,
				{ "action", "file", "path", "restoreScene", "settleMs",
					"captureCheckpoints", "variant", "goldens", "coupling",
					"force", "async", "repeat", "continueOnError" },
				"record replay");
			auto file = ReadString(a_args, "file");
			if (file.empty())
			{
				const auto legacyPath = ReadString(a_args, "path");
				if (!legacyPath.empty())
					file = legacyPath;
			}
			if (file.empty())
			{
				const auto listing = store.List();
				const auto recordings =
					listing.value("recordings", json::array());
				for (const auto& item : recordings)
					if (item.is_object() && !item.contains("error"))
					{
						file = item.value("file", std::string{});
						break;
					}
				if (file.empty())
					throw ToolError(404, "no recording is available to replay");
			}
			const bool force = ReadBool(a_args, "force", false);
			const auto parsed = ParseRecording(store.Load(file), force);
			const auto plan =
				BuildReplayPlan(parsed, a_args, a_context, file);

			json scenarioArgs{
				{ "steps", plan.at("steps") },
				{ "repeat", a_args.value("repeat", 1) },
				{ "continueOnError",
					a_args.value("continueOnError", false) },
				{ "async", a_args.value("async", true) },
			};
			{
				const std::lock_guard lock{ mutex };
				if (replaying)
					throw ToolError(409, "another recording replay is active");
				replaying = true;
				replayStartCursor = events.HeadSeq();
				replaySuppressionEnd.reset();
			}

			scenario::ScenarioRunOptions options;
			options.owner =
				"recording:" + std::filesystem::path(file).stem().string();
			options.resultMetadata = json{
				{ "recording", file },
				{ "restoration", plan.at("restoration") },
				{ "coupling", plan.at("coupling") },
				{ "activity", plan.at("activity") },
				{ "warnings", plan.at("warnings") },
			};
			options.finalizeResult = [](json& a_result) {
				a_result["checkpoints"] = CheckpointSummary(a_result);
				auto restoration =
					a_result.value("restoration", json::object());
				if (restoration.value("scheduled", false))
				{
					bool confirmed = false;
					for (const auto& step :
						a_result.value("results", json::array()))
						if (step.value("sceneConfirmed", false))
						{
							confirmed = true;
							break;
						}
					restoration["restored"] = confirmed;
					a_result["restoration"] = std::move(restoration);
				}
			};
			options.onSubmitted =
				[weak = weak_from_this(), file,
				 stepCount = plan.at("steps").size(),
				 coupling = plan.at("coupling")](std::uint64_t a_runId) {
					if (const auto state = weak.lock())
						state->events.Publish(
							"replay.started",
							json{
								{ "runId", a_runId },
								{ "recording", file },
								{ "steps", stepCount },
								{ "coupling", coupling },
							});
				};
			options.onCompleted =
				[weak = weak_from_this(), file](
					std::uint64_t a_runId, const json& a_result) {
					if (const auto state = weak.lock())
					{
						{
							const std::lock_guard lock{ state->mutex };
							state->replaySuppressionEnd =
								state->events.HeadSeq();
							state->replaying = false;
						}
						state->events.Publish(
							"replay.finished",
							json{
								{ "runId", a_runId },
								{ "recording", file },
								{ "ok", a_result.value("ok", false) },
								{ "status",
									a_result.value(
										"status", std::string("failed")) },
								{ "checkpoints",
									a_result.value("checkpoints", json::array()) },
							});
					}
				};

			json result;
			try
			{
				result = scenarios.Run(
					scenarioArgs, a_context, std::move(options));
			}
			catch (...)
			{
				const std::lock_guard lock{ mutex };
				replaySuppressionEnd = events.HeadSeq();
				replaying = false;
				throw;
			}
			result["recording"] = file;
			if (!result.contains("restoration"))
				result["restoration"] = plan.at("restoration");
			if (!result.contains("coupling"))
				result["coupling"] = plan.at("coupling");
			if (!result.contains("activity"))
				result["activity"] = plan.at("activity");
			return result;
		}

		json Transition(const json& a_args, const ToolContext& a_context)
		{
			ValidateOnly(a_args, { "action", "target" }, "record transition");
			if (!a_context.internal)
				throw ToolError(
					403,
					"record transition is reserved for scenario/replay");
			RequireToolPermission(
				allowGameActions, ToolPermission::kGameActions);
			if (!backend.transition)
				throw ToolError(
					501,
					"recorded scene transition is unsupported by this runtime");
			const auto target = a_args.find("target");
			if (target == a_args.end() || !target->is_object())
				throw ToolError(
					400, "record transition requires object 'target'");
			if (!target->value("interior", false))
			{
				ToolContext context = a_context;
				context.internal = true;
				const auto actionCursor = events.HeadSeq();
				const auto command = BuildCowCommand(*target);
				const auto result = registry.Invoke(
					"console",
					json{
						{ "action", "exec" },
						{ "command", command },
					},
					context);
				if (!result.ok)
					throw ToolError(result.errorCode, result.errorMessage);
				return json{
					{ "queued", result.value.value("queued", false) },
					{ "eventCursor", actionCursor },
					{ "actionCursor", actionCursor },
					{ "mode", "worldspaceConsole" },
					{ "command", command },
				};
			}
			const auto receipt = backend.transition(*target);
			return json{
				{ "queued", receipt.queued },
				{ "eventCursor", receipt.actionCursor },
				{ "actionCursor", receipt.actionCursor },
				{ "mode", receipt.mode },
			};
		}

		json HandleRecord(
			const json& a_args, const ToolContext& a_context)
		{
			{
				const std::lock_guard lock{ mutex };
				if (stopping)
					throw ToolError(503, "recording service is shutting down");
			}
			RequireObject(a_args, "record");
			const auto action =
				ReadString(a_args, "action", "status");
			if (action == "start")
				return Start(a_args);
			if (action == "stop")
				return Stop(a_args);
			if (action == "checkpoint")
				return Checkpoint(a_args);
			if (action == "replay")
				return Replay(a_args, a_context);
			if (action == "transition")
				return Transition(a_args, a_context);
			if (action == "status")
			{
				ValidateOnly(a_args, { "action", "runId" }, "record status");
				if (a_args.contains("runId"))
				{
					const auto runId = ReadInteger(
						a_args, "runId", 0, 1,
						std::numeric_limits<std::int64_t>::max());
					return scenarios.Status(
						static_cast<std::uint64_t>(runId));
				}
				return Status();
			}
			throw ToolError(
				400,
				std::format(
					"unknown record action '{}' "
					"(start|stop|status|checkpoint|replay)",
					action));
		}

		json HandleRecordings(
			const json& a_args, const ToolContext&)
		{
			{
				const std::lock_guard lock{ mutex };
				if (stopping)
					throw ToolError(503, "recording service is shutting down");
			}
			RequireObject(a_args, "recordings");
			const auto action =
				ReadString(a_args, "action", "list");
			if (action == "list")
			{
				ValidateOnly(a_args, { "action" }, "recordings list");
				return store.List();
			}
			const auto file = ReadString(a_args, "file");
			if (file.empty())
				throw ToolError(400, "'file' is required");
			if (action == "describe")
			{
				ValidateOnly(
					a_args, { "action", "file", "force" }, "recordings describe");
				const auto parsed =
					ParseRecording(
						store.Load(file), ReadBool(a_args, "force", false));
				return json{
					{ "file", file },
					{ "meta",
						parsed.document.value("meta", json::object()) },
					{ "format", parsed.format },
					{ "legacy", parsed.legacy },
					{ "foreignCapability", parsed.foreignCapability },
					{ "warnings", parsed.warnings },
				};
			}
			if (action == "validate")
			{
				ValidateOnly(
					a_args,
					{ "action", "file", "value", "force" },
					"recordings validate");
				const auto value = ReadBool(a_args, "value", true);
				auto parsed = ParseRecording(
					store.Load(file), ReadBool(a_args, "force", false));
				parsed.document["meta"]["validated"] = value;
				store.Replace(file, parsed.document);
				return json{
					{ "file", file },
					{ "validated", value },
					{ "note",
						"validation marks metadata only; it does not execute or prove the recording" },
				};
			}
			if (action == "delete")
			{
				ValidateOnly(
					a_args, { "action", "file" }, "recordings delete");
				store.Delete(file);
				return json{
					{ "file", file },
					{ "deleted", true },
				};
			}
			throw ToolError(
				400,
				std::format(
					"unknown recordings action '{}' "
					"(list|describe|validate|delete)",
					action));
		}
	};

	RecordingConfiguration RecordingConfigurationFromConfig(
		const Config& a_config)
	{
		return {
			.sampleInterval =
				Milliseconds(std::max(10, a_config.recordIntervalMs)),
			.loadSettle = Milliseconds(std::max(0, a_config.loadSettleMs)),
			.captureSettle =
				Milliseconds(std::max(0, a_config.captureSettleMs)),
			.couplingAnchor =
				Milliseconds(std::max(0, a_config.couplingAnchorMs)),
			.couplingCell =
				Milliseconds(std::max(
					a_config.couplingAnchorMs, a_config.couplingCellMs)),
			.cleanTransition = a_config.cleanTransition,
			.cleanTransitionCell = a_config.cleanTransitionCell,
		};
	}

	ToolDescriptor BuildRecordDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "record";
		descriptor.description =
			"Capture bounded Fallout 4 pose, console, lifecycle, menu, and cell activity into "
			"devbench-recording-3, mark checkpoints by id, and replay through the shared scenario "
			"worker/history. Replays preserve named-save identity, resolve rolling saves exactly "
			"once, use typed CELL/WRLD transitions where available, suppress feedback recording, "
			"and never claim VR tracking. validate is provided by the recordings tool and marks "
			"metadata only.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "additionalProperties", false },
			{ "properties", json{
				{ "action", json{ { "type", "string" },
					{ "enum", json::array({ "start", "stop", "status", "checkpoint", "replay" }) },
					{ "default", "status" } } },
				{ "intervalMs", json{ { "type", "integer" }, { "minimum", 10 },
					{ "maximum", kMaximumRecordingDuration.count() } } },
				{ "allowNoPlayer", json{ { "type", "boolean" }, { "default", false } } },
				{ "correlationId", json{ { "type", "string" }, { "maxLength", 128 } } },
				{ "id", json{ { "type", "string" }, { "maxLength", 120 } } },
				{ "excludeUi", json{ { "type", "boolean" }, { "default", true } } },
				{ "settleMs", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", 60000 } } },
				{ "subrect", json{ { "type", "object" } } },
				{ "file", json{ { "type", "string" } } },
				{ "path", json{ { "type", "string" }, { "description", "legacy alias for a bare filename confined to the recordings root" } } },
				{ "restoreScene", json{ { "type", "boolean" }, { "default", false } } },
				{ "captureCheckpoints", json{ { "type", "boolean" }, { "default", true } } },
				{ "variant", json{ { "type", "string" }, { "default", "default" } } },
				{ "goldens", json{ { "type", "object" } } },
				{ "coupling", json{ { "type", "string" }, { "enum", json::array({ "anchored", "cell", "worldspace" }) } } },
				{ "force", json{ { "type", "boolean" }, { "default", false } } },
				{ "async", json{ { "type", "boolean" }, { "default", true } } },
				{ "repeat", json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 1000 } } },
				{ "continueOnError", json{ { "type", "boolean" }, { "default", false } } },
				{ "runId", json{ { "type", "integer" }, { "minimum", 1 } } },
			} },
		};
		return descriptor;
	}

	ToolDescriptor BuildRecordingsDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "recordings";
		descriptor.description =
			"Manage the confined recording library using the same parser as record/replay. "
			"list reports malformed entries individually; describe returns metadata; validate "
			"atomically changes meta.validated without running a test; delete removes only the "
			"explicit requested file.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "additionalProperties", false },
			{ "properties", json{
				{ "action", json{ { "type", "string" },
					{ "enum", json::array({ "list", "describe", "validate", "delete" }) },
					{ "default", "list" } } },
				{ "file", json{ { "type", "string" } } },
				{ "value", json{ { "type", "boolean" }, { "default", true } } },
				{ "force", json{ { "type", "boolean" }, { "default", false },
					{ "description", "explicitly accept a degraded foreign-game/VR metadata read or validation" } } },
			} },
		};
		return descriptor;
	}

	RecordingService::RecordingService(
		ToolRegistry& a_registry, EventBus& a_events,
		scenario::ScenarioService& a_scenarios,
		bool a_allowGameActions,
		RecordingConfiguration a_configuration,
		RecordingBackend a_backend) :
		state_(std::make_shared<State>(
			a_registry, a_events, a_scenarios, a_allowGameActions,
			std::move(a_configuration), std::move(a_backend)))
	{
		state_->Subscribe();
	}

	RecordingService::~RecordingService()
	{
		Shutdown();
	}

	bool RecordingService::Register()
	{
		return state_->Register();
	}

	void RecordingService::Shutdown() noexcept
	{
		if (state_)
			state_->Shutdown();
	}
}

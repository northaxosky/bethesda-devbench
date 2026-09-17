#include "GameTool.h"

#include "ToolExtensions.h"
#include "ToolPermissions.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace dvb::tools
{
	namespace
	{
		std::string LowerAscii(std::string a_value)
		{
			std::ranges::transform(a_value, a_value.begin(), [](unsigned char a_ch) {
				return static_cast<char>(a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return a_value;
		}

		bool EndsWithAsciiCaseInsensitive(
			std::string_view a_value, std::string_view a_suffix)
		{
			if (a_suffix.empty() || a_value.size() < a_suffix.size())
				return false;
			return LowerAscii(
				std::string(a_value.substr(a_value.size() - a_suffix.size()))) ==
			       LowerAscii(std::string(a_suffix));
		}

		std::string ComparableSaveName(
			std::string_view a_name, const GameSavePolicy& a_policy)
		{
			const auto separator = a_name.find_last_of("/\\");
			if (separator != std::string_view::npos)
				a_name.remove_prefix(separator + 1);
			if (EndsWithAsciiCaseInsensitive(a_name, a_policy.saveExtension))
				a_name.remove_suffix(a_policy.saveExtension.size());
			return LowerAscii(std::string(a_name));
		}

		bool SameSaveName(
			std::string_view a_left, std::string_view a_right,
			const GameSavePolicy& a_policy)
		{
			return ComparableSaveName(a_left, a_policy) ==
			       ComparableSaveName(a_right, a_policy);
		}

		bool SameOperationSaveName(
			std::string_view a_left, std::string_view a_right)
		{
			auto canonical = [](std::string_view a_value) {
				const auto separator = a_value.find_last_of("/\\");
				if (separator != std::string_view::npos)
					a_value.remove_prefix(separator + 1);
				std::string out = LowerAscii(std::string(a_value));
				for (const std::string_view extension :
					{ ".fos", ".f4se", ".ess", ".skse" })
					if (out.ends_with(extension))
					{
						out.resize(out.size() - extension.size());
						break;
					}
				return out;
			};
			return canonical(a_left) == canonical(a_right);
		}

		void RequireObject(const json& a_args)
		{
			if (!a_args.is_object())
				throw ToolError(400, "game arguments must be an object");
		}

		std::string ReadString(
			const json& a_args, std::string_view a_name, bool a_required, std::string a_default = {})
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
			{
				if (a_required)
					throw ToolError(400, std::format("missing required parameter '{}'", a_name));
				return a_default;
			}
			if (!it->is_string())
				throw ToolError(400, std::format("'{}' must be a string", a_name));
			return it->get<std::string>();
		}

		bool ReadBool(const json& a_args, std::string_view a_name, bool a_default = false)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return a_default;
			if (!it->is_boolean())
				throw ToolError(400, std::format("'{}' must be a boolean", a_name));
			return it->get<bool>();
		}

		std::optional<std::string> ReadDirectory(const json& a_args)
		{
			const auto it = a_args.find("dir");
			if (it == a_args.end())
				return std::nullopt;
			if (!it->is_string())
				throw ToolError(400, "'dir' must be a string");
			auto directory = it->get<std::string>();
			if (directory.empty())
				return std::nullopt;
			if (directory.size() > kMaxSaveDirectoryBytes)
				throw ToolError(400, std::format("'dir' exceeds {} bytes", kMaxSaveDirectoryBytes));
			if (directory.find('\0') != std::string::npos)
				throw ToolError(400, "'dir' must not contain an embedded NUL");
			return directory;
		}

		void ValidateOnly(const json& a_args, std::initializer_list<std::string_view> a_allowed)
		{
			for (const auto& [key, value] : a_args.items())
			{
				(void)value;
				if (std::ranges::find(a_allowed, key) == a_allowed.end())
					throw ToolError(400, std::format("unexpected parameter '{}' for game action", key));
			}
		}

		std::string ValidateSaveName(
			const json& a_args, const GameSavePolicy& a_policy)
		{
			const auto name = ReadString(a_args, "name", true);
			if (name.empty())
				throw ToolError(400, "'name' must not be empty");
			if (name.size() > kMaxSaveNameBytes)
				throw ToolError(400, std::format("'name' exceeds {} bytes", kMaxSaveNameBytes));
			if (name == "." || name == "..")
				throw ToolError(400, "'name' must be a save basename, not a path");
			if (name.ends_with('.') || name.ends_with(' '))
				throw ToolError(400, "'name' must not end with a dot or space");
			if (name.find('\0') != std::string::npos)
				throw ToolError(400, "'name' must not contain an embedded NUL");

			for (const unsigned char ch : name)
			{
				if (ch < 0x20 || ch >= 0x7F || std::string_view("<>:\"/\\|?*").contains(static_cast<char>(ch)))
					throw ToolError(400, "'name' must contain only portable ASCII filename characters");
			}

			const auto lower = LowerAscii(name);
			if (EndsWithAsciiCaseInsensitive(lower, a_policy.saveExtension) ||
				EndsWithAsciiCaseInsensitive(lower, a_policy.coSaveExtension))
				throw ToolError(400, "'name' must be a basename without a file extension");

			const auto dot = lower.find('.');
			const auto device = lower.substr(0, dot);
			const bool reserved =
				device == "con" || device == "prn" || device == "aux" || device == "nul" ||
				(device.size() == 4 &&
					((device.starts_with("com") || device.starts_with("lpt")) &&
						device[3] >= '1' && device[3] <= '9'));
			if (reserved)
				throw ToolError(400, "'name' is a reserved Windows filename");
			return name;
		}

		GameSaveList ReadSortedSaves(
			const GameBackend& a_backend, const std::optional<std::string>& a_directory = std::nullopt)
		{
			GameSaveList list;
			if (a_backend.listSavesWithOptions)
				list = a_backend.listSavesWithOptions({ .directory = a_directory });
			else if (!a_directory && a_backend.listSaves)
				list = a_backend.listSaves();
			else
				throw ToolError(503, "save listing is unavailable");
			std::ranges::sort(list.saves, [](const GameSaveInfo& a_left, const GameSaveInfo& a_right) {
				if (a_left.modifiedUnix != a_right.modifiedUnix)
				{
					if (!a_left.modifiedUnix)
						return false;
					if (!a_right.modifiedUnix)
						return true;
					return *a_left.modifiedUnix > *a_right.modifiedUnix;
				}
				const auto left = LowerAscii(a_left.name);
				const auto right = LowerAscii(a_right.name);
				return left == right ? a_left.name < a_right.name : left < right;
			});
			return list;
		}

		json MetadataToJson(const game::SaveMetadata& a_metadata, std::string_view a_name)
		{
			const auto lower = LowerAscii(std::string(a_name));
			const auto saveType =
				lower.starts_with("autosave")  ? "autosave" :
				lower.starts_with("quicksave") ? "quicksave" :
												 "save";
			return json{
				{ "characterName", a_metadata.characterName },
				{ "location", a_metadata.location },
				{ "playTime", a_metadata.playTime },
				{ "race", a_metadata.race },
				{ "level", a_metadata.level },
				{ "experience",
					json{
						{ "current", a_metadata.currentExperience },
						{ "required", a_metadata.requiredExperience },
					} },
				{ "saveNumber", a_metadata.saveNumber },
				{ "saveType", saveType },
				{ "screenshot",
					json{
						{ "width", a_metadata.screenshotWidth },
						{ "height", a_metadata.screenshotHeight },
					} },
				{ "fileTimeUnix",
					a_metadata.fileTimeUnix ?
						json(*a_metadata.fileTimeUnix) :
						json(nullptr) },
			};
		}

		json SaveToJson(const GameSaveInfo& a_save)
		{
			json value{
				{ "name", a_save.name },
				{ "mtimeUnix", a_save.modifiedUnix ? json(*a_save.modifiedUnix) : json(nullptr) },
			};
			if (a_save.metadata)
				value["meta"] = MetadataToJson(*a_save.metadata, a_save.name);
			if (a_save.metadataError)
				value["metaError"] = *a_save.metadataError;
			return value;
		}

		json HandleStatus(bool a_allowActions, const GameBackend& a_backend)
		{
			if (!a_backend.status)
				throw ToolError(503, "game status is unavailable");
			auto status = a_backend.status();
			if (!status.is_object())
				throw ToolError(500, "game status provider returned a non-object result");
			status["actionsAllowed"] = a_allowActions;
			return status;
		}

		json HandleList(const json&                 a_args,
			const GameBackend&                      a_backend,
			std::initializer_list<std::string_view> a_allowed)
		{
			ValidateOnly(a_args, a_allowed);
			const auto filter = ReadString(a_args, "filter", false);
			const auto directory = ReadDirectory(a_args);
			const auto detail = ReadBool(a_args, "detail");
			if (filter.size() > kMaxSaveFilterBytes)
				throw ToolError(400, std::format("'filter' exceeds {} bytes", kMaxSaveFilterBytes));
			if (filter.find('\0') != std::string::npos)
				throw ToolError(400, "'filter' must not contain an embedded NUL");

			std::int64_t limit = 100;
			if (const auto it = a_args.find("limit"); it != a_args.end())
			{
				if (!it->is_number_integer())
					throw ToolError(400, "'limit' must be an integer");
				limit = it->get<std::int64_t>();
				if (limit < 1 || limit > kMaxSaveListLimit)
					throw ToolError(
						400, std::format("'limit' must be between 1 and {}", kMaxSaveListLimit));
			}

			auto  list = ReadSortedSaves(a_backend, directory);
			auto& saves = list.saves;
			if (!filter.empty())
			{
				const auto needle = LowerAscii(filter);
				std::erase_if(saves, [&](const GameSaveInfo& a_save) {
					return LowerAscii(a_save.name).find(needle) == std::string::npos;
				});
			}

			const auto matched = saves.size();
			if (saves.size() > static_cast<std::size_t>(limit))
				saves.resize(static_cast<std::size_t>(limit));

			std::size_t metadataAvailable = 0;
			if (detail)
			{
				for (auto& save : saves)
				{
					if (!a_backend.readSaveMetadata)
					{
						save.metadataError =
							a_backend.savePolicy.gameName +
							" save metadata parsing is unavailable";
						continue;
					}
					auto parsed = a_backend.readSaveMetadata(list.directory, save.name);
					if (parsed.metadata)
					{
						save.metadata = std::move(parsed.metadata);
						++metadataAvailable;
					}
					else
					{
						save.metadataError = parsed.error.empty() ?
						                         a_backend.savePolicy.gameName +
													 " save metadata could not be read" :
						                         std::move(parsed.error);
					}
				}
			}

			json values = json::array();
			for (const auto& save : saves)
				values.push_back(SaveToJson(save));
			json out{
				{ "dir", std::move(list.directory) },
				{ "count", matched },
				{ "matched", matched },
				{ "returned", values.size() },
				{ "truncated", matched > values.size() },
				{ "saves", std::move(values) },
				{ "note",
					std::format(
						"{} {} saves sorted newest-first; names are extensionless basenames.",
						a_backend.savePolicy.gameName,
						a_backend.savePolicy.saveExtension) },
			};
			if (detail)
			{
				out["metaAvailable"] = metadataAvailable != 0;
				if (metadataAvailable != 0)
					out["metaNote"] = nullptr;
				else if (saves.empty())
					out["metaNote"] = "no saves matched 'filter'/'limit' -- nothing to read";
				else
					out["metaNote"] = std::format(
						"none of the returned saves' {} headers could be read; see per-save metaError",
						a_backend.savePolicy.saveExtension);
			}
			return out;
		}

		json Queue(
			const GameBackend& a_backend, GameOperationKind a_kind, std::string a_name,
			std::optional<std::string> a_directory)
		{
			if (!a_backend.queueOperation)
				throw ToolError(503, "game action dispatcher unavailable");
			const auto receipt =
				a_backend.queueOperation({ a_kind, a_name, std::move(a_directory) });
			if (receipt.operationId == 0)
				throw ToolError(500, "game action dispatcher returned an invalid operation id");

			const auto action = GameOperationKindName(a_kind);
			return json{
				{ "queued", true },
				{ "action", action },
				{ "name", std::move(a_name) },
				{ "operationId", receipt.operationId },
				{ "actionCursor", receipt.actionCursor },
				{ "eventCursor", receipt.actionCursor },
				{ "note",
					a_kind == GameOperationKind::kLoad ?
						"Queued means the native load request was accepted, not completed. Observe lifecycle events strictly after actionCursor and poll action='status'. A content-mismatch modal may block completion and is never answered automatically." :
						"Queued means the native save request was accepted, not completed. Observe lifecycle events strictly after actionCursor and poll action='status'." },
			};
		}

		json AdvanceTime(const json& a_args, const GameBackend& a_backend)
		{
			ValidateOnly(a_args, { "action", "hours" });
			const auto it = a_args.find("hours");
			if (it == a_args.end() || !it->is_number())
				throw ToolError(400, "'hours' must be a number");
			const auto hours = it->get<double>();
			if (!std::isfinite(hours) || hours == 0.0)
				throw ToolError(400, "'hours' must be finite and non-zero");
			if (std::abs(hours) > kMaxGameAdvanceHours)
				throw ToolError(400, std::format(
										 "'hours' must be within +/-{}",
										 kMaxGameAdvanceHours));
			if (!a_backend.advanceTime)
				throw ToolError(503, "calendar advancement is unavailable");

			const auto receipt = a_backend.advanceTime(hours);
			if (receipt.operationId == 0)
				throw ToolError(500, "calendar dispatcher returned an invalid operation id");
			if (!receipt.completed)
			{
				return json{
					{ "queued", true },
					{ "completed", false },
					{ "action", "advanceTime" },
					{ "hours", hours },
					{ "operationId", receipt.operationId },
					{ "actionCursor", receipt.actionCursor },
					{ "eventCursor", receipt.actionCursor },
					{ "note",
						"The main-thread calendar mutation started but completion was not observed within the response window. Poll game action='status'; do not retry while the operation is active." },
				};
			}
			if (!receipt.result || !receipt.result->is_object())
				throw ToolError(500, "calendar dispatcher returned no completed result");
			auto out = *receipt.result;
			out["queued"] = false;
			out["completed"] = true;
			out["action"] = "advanceTime";
			out["hours"] = hours;
			out["operationId"] = receipt.operationId;
			out["actionCursor"] = receipt.actionCursor;
			out["eventCursor"] = receipt.actionCursor;
			return out;
		}

		std::optional<std::string> ResolveMutationDirectory(
			const GameBackend& a_backend, const std::optional<std::string>& a_requested)
		{
			if (a_backend.resolveMutationDirectory)
				return a_backend.resolveMutationDirectory(a_requested);
			if (a_requested)
				throw ToolError(
					503, "native save directory validation is unavailable for mutations");
			return std::nullopt;
		}

		json HandleGame(const json& a_args, bool a_allowActions, const GameBackend& a_backend)
		{
			RequireObject(a_args);
			const auto action = ReadString(a_args, "action", false, "status");

			if (action == "status")
			{
				ValidateOnly(a_args, { "action" });
				return HandleStatus(a_allowActions, a_backend);
			}

			if (action == "list")
				return HandleList(a_args, a_backend, { "action", "filter", "limit", "detail", "dir" });

			if (action != "save" && action != "load" && action != "loadLast" &&
				action != "advanceTime")
				throw ToolError(400, std::format("unknown game action '{}'", action));
			RequireToolPermission(a_allowActions, ToolPermission::kGameActions);

			if (action == "advanceTime")
				return AdvanceTime(a_args, a_backend);

			if (action == "loadLast")
			{
				ValidateOnly(a_args, { "action", "dir" });
				const auto requestedDirectory = ReadDirectory(a_args);
				const auto directory =
					ResolveMutationDirectory(a_backend, requestedDirectory);
				auto list = ReadSortedSaves(a_backend, directory);
				if (list.saves.empty())
					throw ToolError(
						404, "no " + a_backend.savePolicy.gameName +
						         " saves are available to load");
				return Queue(
					a_backend, GameOperationKind::kLoad, list.saves.front().name, directory);
			}

			ValidateOnly(a_args, { "action", "name", "dir" });
			auto       name = ValidateSaveName(a_args, a_backend.savePolicy);
			const auto requestedDirectory = ReadDirectory(a_args);
			const auto directory =
				ResolveMutationDirectory(a_backend, requestedDirectory);
			auto       list = ReadSortedSaves(a_backend, directory);
			const auto existing = std::ranges::find_if(list.saves, [&](const GameSaveInfo& a_save) {
				return SameSaveName(a_save.name, name, a_backend.savePolicy);
			});
			if (action == "save")
			{
				if (existing != list.saves.end())
					throw ToolError(409, std::format("save '{}' already exists; refusing to overwrite", existing->name));
				return Queue(
					a_backend, GameOperationKind::kSave, std::move(name), directory);
			}

			if (existing == list.saves.end())
				throw ToolError(404, std::format("save '{}' was not found; use action='list' for valid names", name));
			return Queue(a_backend, GameOperationKind::kLoad, existing->name, directory);
		}
	}

	std::string_view GameOperationKindName(GameOperationKind a_kind)
	{
		switch (a_kind)
		{
			case GameOperationKind::kSave:
				return "save";
			case GameOperationKind::kLoad:
				return "load";
			case GameOperationKind::kAdvanceTime:
				return "advanceTime";
			case GameOperationKind::kWait:
				return "wait";
			case GameOperationKind::kSleep:
				return "sleep";
		}
		return "unknown";
	}

	std::string_view GameOperationPhaseName(GameOperationPhase a_phase)
	{
		switch (a_phase)
		{
			case GameOperationPhase::kReserved:
				return "reserved";
			case GameOperationPhase::kDispatching:
				return "dispatching";
			case GameOperationPhase::kQueued:
				return "queued";
			case GameOperationPhase::kStarted:
				return "started";
			case GameOperationPhase::kCompleted:
				return "completed";
			case GameOperationPhase::kSucceeded:
				return "succeeded";
			case GameOperationPhase::kFailed:
				return "failed";
			case GameOperationPhase::kCancelled:
				return "cancelled";
		}
		return "unknown";
	}

	std::optional<GameOperationSnapshot> GameOperationTracker::TryBegin(
		GameOperationKind a_kind,
		std::string       a_name,
		std::uint64_t     a_actionCursor)
	{
		const std::lock_guard lock{ mutex_ };
		if (operation_ && operation_->active)
			return std::nullopt;
		operation_ = GameOperationSnapshot{
			.operationId = ++nextOperationId_,
			.kind = a_kind,
			.name = std::move(a_name),
			.phase = GameOperationPhase::kReserved,
			.actionCursor = a_actionCursor,
			.active = true,
		};
		return operation_;
	}

	bool GameOperationTracker::MarkDispatching(std::uint64_t a_operationId)
	{
		const std::lock_guard lock{ mutex_ };
		if (!operation_ || !operation_->active || operation_->operationId != a_operationId ||
			operation_->phase != GameOperationPhase::kReserved)
			return false;
		operation_->phase = GameOperationPhase::kDispatching;
		return true;
	}

	bool GameOperationTracker::MarkQueued(std::uint64_t a_operationId)
	{
		const std::lock_guard lock{ mutex_ };
		if (!operation_ || !operation_->active || operation_->operationId != a_operationId)
			return false;
		if (operation_->phase == GameOperationPhase::kDispatching)
			operation_->phase = GameOperationPhase::kQueued;
		return true;
	}

	bool GameOperationTracker::MarkDispatchFailed(std::uint64_t a_operationId, std::string a_error)
	{
		const std::lock_guard lock{ mutex_ };
		if (!operation_ || !operation_->active || operation_->operationId != a_operationId)
			return false;
		operation_->phase = GameOperationPhase::kFailed;
		operation_->success = false;
		operation_->error = std::move(a_error);
		operation_->active = false;
		return true;
	}

	bool GameOperationTracker::MarkCancelled(std::uint64_t a_operationId, std::string a_error)
	{
		const std::lock_guard lock{ mutex_ };
		if (!operation_ || !operation_->active || operation_->operationId != a_operationId ||
			operation_->phase != GameOperationPhase::kReserved)
			return false;
		operation_->phase = GameOperationPhase::kCancelled;
		operation_->success = false;
		operation_->error = std::move(a_error);
		operation_->active = false;
		return true;
	}

	bool GameOperationTracker::MarkCompleted(std::uint64_t a_operationId, json a_result)
	{
		return MarkCompleted(a_operationId, true, std::move(a_result));
	}

	bool GameOperationTracker::MarkCompleted(
		std::uint64_t a_operationId, bool a_success, json a_result, std::string a_error)
	{
		const std::lock_guard lock{ mutex_ };
		if (!operation_ || !operation_->active || operation_->operationId != a_operationId ||
			(operation_->phase != GameOperationPhase::kDispatching &&
				operation_->phase != GameOperationPhase::kQueued &&
				operation_->phase != GameOperationPhase::kStarted))
			return false;
		operation_->phase =
			a_success ? GameOperationPhase::kSucceeded : GameOperationPhase::kFailed;
		operation_->success = a_success;
		operation_->result = std::move(a_result);
		operation_->error = std::move(a_error);
		operation_->active = false;
		return true;
	}

	bool GameOperationTracker::ObserveStarted(GameOperationKind a_kind,
		const std::optional<std::string>&                       a_name,
		std::uint64_t                                           a_eventCursor)
	{
		const std::lock_guard lock{ mutex_ };
		if (!operation_ || !operation_->active || operation_->kind != a_kind ||
			a_eventCursor <= operation_->actionCursor)
			return false;
		if (operation_->phase == GameOperationPhase::kReserved)
			return false;
		if (a_name && !SameOperationSaveName(operation_->name, *a_name))
			return false;
		operation_->phase = GameOperationPhase::kStarted;
		operation_->startedCursor = a_eventCursor;
		if (a_name)
			operation_->observedName = *a_name;
		return true;
	}

	bool GameOperationTracker::ObserveCompleted(GameOperationKind a_kind,
		const std::optional<std::string>&                         a_name,
		std::optional<bool>                                       a_success,
		std::uint64_t                                             a_eventCursor)
	{
		const std::lock_guard lock{ mutex_ };
		if (!operation_ || !operation_->active || operation_->kind != a_kind ||
			a_eventCursor <= operation_->actionCursor)
			return false;
		if (operation_->phase == GameOperationPhase::kReserved)
			return false;
		if (a_name && !SameOperationSaveName(operation_->name, *a_name))
			return false;
		if (!a_name && operation_->phase != GameOperationPhase::kStarted)
			return false;

		if (!a_success)
			operation_->phase = GameOperationPhase::kCompleted;
		else
			operation_->phase = *a_success ? GameOperationPhase::kSucceeded : GameOperationPhase::kFailed;
		operation_->completedCursor = a_eventCursor;
		operation_->success = a_success;
		if (a_name)
			operation_->observedName = *a_name;
		if (a_success == false)
			operation_->error = "native lifecycle reported failure";
		operation_->active = false;
		return true;
	}

	std::optional<GameOperationSnapshot> GameOperationTracker::GetSnapshot() const
	{
		const std::lock_guard lock{ mutex_ };
		return operation_;
	}

	void GameOperationTracker::Reset()
	{
		const std::lock_guard lock{ mutex_ };
		operation_.reset();
	}

	ToolDescriptor BuildGameDescriptor(const GameSavePolicy& a_policy)
	{
		ToolDescriptor descriptor;
		descriptor.name = "game";
		descriptor.description =
			std::format(
				"Inspect save/load status, list {} {} saves with optional bounded {}, "
				"queue a native save/load, or advance the native game calendar. ",
				a_policy.gameName, a_policy.saveExtension,
				a_policy.metadataDescription) +
			"Mutations require allowGameActions=true, serialize one in-flight operation, never overwrite "
			"an existing save, and return an actionCursor captured before native dispatch for lifecycle correlation. "
			"An explicit dir is read-only selection and must resolve to the native save directory for mutations. "
			"Loads never answer content-mismatch dialogs automatically; advanceTime is not wait or sleep.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "status", "list", "save", "load", "loadLast", "advanceTime" }) }, { "default", "status" } } },
								{ "name", json{ { "type", "string" }, { "minLength", 1 }, { "maxLength", kMaxSaveNameBytes }, { "description", "save/load: extensionless " + a_policy.gameName + " save basename" } } },
								{ "filter", json{ { "type", "string" }, { "maxLength", kMaxSaveFilterBytes }, { "description", "list: case-insensitive basename substring" } } },
								{ "limit", json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", kMaxSaveListLimit }, { "default", 100 } } },
								{ "detail", json{ { "type", "boolean" }, { "default", false }, { "description", "list: parse bounded metadata from each returned " + a_policy.saveExtension + " header" } } },
								{ "dir", json{ { "type", "string" }, { "maxLength", kMaxSaveDirectoryBytes }, { "description", "list: read an explicit save directory; mutations may only name the native effective directory" } } },
								{ "hours", json{ { "type", "number" }, { "minimum", -kMaxGameAdvanceHours }, { "maximum", kMaxGameAdvanceHours }, { "description", "advanceTime: finite non-zero signed game hours" } } },
							} },
			{ "additionalProperties", false },
		};
		return descriptor;
	}

	void RegisterGameTool(ToolRegistry& a_registry, bool a_allowActions, GameBackend a_backend)
	{
		auto sharedBackend = std::make_shared<GameBackend>(std::move(a_backend));
		a_registry.Register(BuildGameDescriptor(sharedBackend->savePolicy),
			[a_allowActions, sharedBackend](const json& a_args, const ToolContext&) {
				return HandleGame(a_args, a_allowActions, *sharedBackend);
			});
		ToolExtensions::Register("inspect", "saveLoad",
			json{
				{ "description", "Read native save/load readiness and the latest correlated operation." },
				{ "readOnly", true },
			},
			[a_allowActions, sharedBackend](const json& a_args, const ToolContext&) {
				RequireObject(a_args);
				ValidateOnly(a_args, { "kind" });
				return HandleStatus(a_allowActions, *sharedBackend);
			});
		ToolExtensions::Register("inspect", "saves",
			json{
				{ "description",
					"List " + sharedBackend->savePolicy.gameName + " " +
						sharedBackend->savePolicy.saveExtension +
						" saves with optional filter, limit, detail, and read-only directory selection." },
				{ "readOnly", true },
			},
			[sharedBackend](const json& a_args, const ToolContext&) {
				RequireObject(a_args);
				return HandleList(a_args, *sharedBackend, { "kind", "filter", "limit", "detail", "dir" });
			});
	}
}

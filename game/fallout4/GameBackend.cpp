#include "GameBackend.h"

#include "Lifecycle.h"
#include "MainThread.h"
#include "game/fallout4/data/Fallout4Calendar.h"
#include "game/fallout4/data/Fallout4SaveHeader.h"

#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <future>
#include <limits>

namespace dvb::fallout4
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr auto          kDispatchTimeout = std::chrono::milliseconds(5000);
		constexpr std::uint32_t kMutatingTaskMask =
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kAutoSave) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kForceSave) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kLoadMostRecentSave) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kQuickSave) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kQuickLoad) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kQuickNewSave) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kLoadGame) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kSysUtilLoadGame) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kMissingContentLoad) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kAutoSaveCommit) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kQuickSaveCommit) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kSaveAndQuit) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kConfirmModsLoad) |
			static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kSaveAndQuitToDesktop);

		enum class WorkPhase
		{
			kPending,
			kRunning,
			kCancelled,
			kFinished,
		};

		struct DispatchResult
		{
			bool        accepted = false;
			bool        completed = false;
			int         errorCode = 503;
			std::string error;
			json        result;
		};

		struct DispatchWork
		{
			std::atomic<WorkPhase>       phase{ WorkPhase::kPending };
			std::promise<DispatchResult> result;
		};

		struct TrackedDispatchReceipt
		{
			std::uint64_t                 operationId = 0;
			std::uint64_t                 actionCursor = 0;
			std::optional<DispatchResult> result;
		};

		std::string Utf8String(const fs::path& a_path)
		{
			const auto value = a_path.u8string();
			return { reinterpret_cast<const char*>(value.data()), value.size() };
		}

		std::string ResolveSaveDirectory()
		{
			const auto value = MainThread::RunAndWait([]() -> json {
				const auto manager = RE::BGSSaveLoadManager::GetSingleton();
				if (!manager)
					throw ToolError(503, "BGSSaveLoadManager is unavailable");
				std::array<char, 0x104> path{};
				manager->GetSaveDirectoryPath(path.data());
				path.back() = '\0';
				if (path.front() == '\0')
					throw ToolError(503, "Fallout 4 returned an empty save directory");
				return std::string(path.data());
			});
			return value.get<std::string>();
		}

		tools::GameSaveList EnumerateSaves(tools::GameSaveListRequest a_request)
		{
			const fs::path         directory = a_request.directory ?
			                                       fs::path(*a_request.directory) :
			                                       fs::path(ResolveSaveDirectory());
			std::error_code        ec;
			fs::directory_iterator it(directory, ec);
			if (ec)
				throw ToolError(503, std::format(
										 "could not enumerate Fallout 4 save directory '{}': {}",
										 Utf8String(directory), ec.message()));

			tools::GameSaveList          out{ .directory = Utf8String(directory) };
			const fs::directory_iterator end;
			for (; it != end;)
			{
				const auto& entry = *it;
				const bool  regular = entry.is_regular_file(ec);
				if (ec)
					throw ToolError(503, std::format(
											 "could not inspect save directory entry '{}': {}",
											 Utf8String(entry.path()), ec.message()));
				if (regular)
				{
					auto extension = Utf8String(entry.path().extension());
					std::ranges::transform(extension, extension.begin(), [](unsigned char a_ch) {
						return static_cast<char>(a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
					});
					if (extension == ".fos")
					{
						std::optional<std::int64_t> modifiedUnix;
						const auto                  modified = entry.last_write_time(ec);
						if (!ec)
						{
							const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(modified);
							modifiedUnix = std::chrono::duration_cast<std::chrono::seconds>(
								systemTime.time_since_epoch())
							                   .count();
						}
						else
						{
							throw ToolError(503, std::format(
													 "could not read save modification time '{}': {}",
													 Utf8String(entry.path()), ec.message()));
						}
						out.saves.push_back({
							.name = Utf8String(entry.path().stem()),
							.modifiedUnix = modifiedUnix,
						});
					}
				}
				it.increment(ec);
				if (ec)
					throw ToolError(503, std::format(
											 "could not continue enumerating Fallout 4 save directory '{}': {}",
											 Utf8String(directory), ec.message()));
			}
			return out;
		}

		tools::GameSaveList EnumerateNativeSaves()
		{
			return EnumerateSaves({});
		}

		tools::game::SaveMetadataResult ReadSaveMetadata(
			const std::string& a_directory, const std::string& a_name)
		{
			return tools::game::ReadFallout4SaveHeader(
				fs::path(a_directory) / (a_name + ".fos"));
		}

		fs::path NativeSaveDirectory(RE::BGSSaveLoadManager& a_manager)
		{
			std::array<char, 0x104> directory{};
			a_manager.GetSaveDirectoryPath(directory.data());
			directory.back() = '\0';
			if (directory.front() == '\0')
				throw ToolError(503, "Fallout 4 returned an empty save directory");
			return fs::path(directory.data());
		}

		bool SameDirectory(const fs::path& a_left, const fs::path& a_right)
		{
			std::error_code ec;
			if (fs::equivalent(a_left, a_right, ec))
				return true;

			auto Normalize = [](const fs::path& a_path) {
				std::error_code localError;
				auto            normalized = fs::weakly_canonical(a_path, localError);
				if (localError)
				{
					localError.clear();
					normalized = fs::absolute(a_path, localError);
					if (localError)
						normalized = a_path;
					normalized = normalized.lexically_normal();
				}
				auto value = Utf8String(normalized);
				std::ranges::transform(value, value.begin(), [](unsigned char a_ch) {
					return static_cast<char>(
						a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
				});
				while (value.size() > 3 && (value.back() == '/' || value.back() == '\\'))
					value.pop_back();
				return value;
			};
			return Normalize(a_left) == Normalize(a_right);
		}

		std::string ResolveMutationDirectory(
			const std::optional<std::string>& a_requestedDirectory)
		{
			const fs::path nativeDirectory{ ResolveSaveDirectory() };
			if (a_requestedDirectory &&
				!SameDirectory(nativeDirectory, fs::path(*a_requestedDirectory)))
				throw ToolError(409, std::format(
										 "directory '{}' is read-only selection and does not match Fallout 4's native save directory '{}'",
										 *a_requestedDirectory, Utf8String(nativeDirectory)));
			return Utf8String(nativeDirectory);
		}

		json OperationJson(const tools::GameOperationSnapshot& a_operation)
		{
			return json{
				{ "operationId", a_operation.operationId },
				{ "action", tools::GameOperationKindName(a_operation.kind) },
				{ "name", a_operation.name },
				{ "phase", tools::GameOperationPhaseName(a_operation.phase) },
				{ "active", a_operation.active },
				{ "actionCursor", a_operation.actionCursor },
				{ "startedCursor",
					a_operation.startedCursor ? json(*a_operation.startedCursor) : json(nullptr) },
				{ "completedCursor",
					a_operation.completedCursor ? json(*a_operation.completedCursor) : json(nullptr) },
				{ "success", a_operation.success ? json(*a_operation.success) : json(nullptr) },
				{ "observedName",
					a_operation.observedName ? json(*a_operation.observedName) : json(nullptr) },
				{ "result", a_operation.result ? *a_operation.result : json(nullptr) },
				{ "error", a_operation.error.empty() ? json(nullptr) : json(a_operation.error) },
			};
		}

		json ReadStatus()
		{
			const auto snapshot = Lifecycle::GetSnapshot();
			const bool ready =
				snapshot.gameDataReady && snapshot.gameLoaded && !snapshot.inMainMenu &&
				!snapshot.inLoadingMenu && !snapshot.loadInProgress &&
				snapshot.postLoadSucceeded != false;
			return json{
				{ "ready", ready },
				{ "canAdvanceTime", ready },
				{ "canLoad",
					snapshot.gameDataReady && !snapshot.inLoadingMenu && !snapshot.loadInProgress },
				{ "canSave", ready },
				{ "gameDataReady", snapshot.gameDataReady },
				{ "gameLoaded", snapshot.gameLoaded },
				{ "inMainMenu", snapshot.inMainMenu },
				{ "inLoadingMenu", snapshot.inLoadingMenu },
				{ "loadInProgress", snapshot.loadInProgress },
				{ "postLoadSucceeded",
					snapshot.postLoadSucceeded ? json(*snapshot.postLoadSucceeded) : json(nullptr) },
				{ "lastLifecycle",
					snapshot.lastEvent.empty() ? json(nullptr) : json(snapshot.lastEvent) },
				{ "eventCursor", snapshot.eventCursor },
				{ "operation",
					snapshot.operation ? OperationJson(*snapshot.operation) : json(nullptr) },
			};
		}

		bool SaveNameEquals(const char* a_nativeName, std::string_view a_requestedName)
		{
			if (!a_nativeName)
				return false;
			const std::string_view nativeName{ a_nativeName };
			if (nativeName.size() != a_requestedName.size())
				return false;
			return std::ranges::equal(nativeName, a_requestedName, [](unsigned char a_lhs, unsigned char a_rhs) {
				const auto lower = [](unsigned char a_ch) {
					return static_cast<unsigned char>(
						a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
				};
				return lower(a_lhs) == lower(a_rhs);
			});
		}

		RE::BGSSaveLoadFileEntry* FindNativeSaveEntry(
			RE::BGSSaveLoadManager& a_manager, std::string_view a_name)
		{
			for (auto* entry : a_manager.saveGameList)
			{
				if (entry && SaveNameEquals(entry->fileName, a_name))
					return entry;
			}
			return nullptr;
		}

		DispatchResult QueueNativeOperation(
			RE::BGSSaveLoadManager& a_manager, const tools::GameOperationRequest& a_request)
		{
			// AE Address Library evidence:
			// - ID 2228036 is the five-argument save enqueue routine (1.11.240 RVA
			//   0xBF9330), not the shortened three-argument prototype used in the RE
			//   reconstruction. Its body consumes R8D, R9D, and the fifth stack byte.
			//   Vanilla callers pass all five arguments: SaveNewGame uses
			//   {name=null, device=-1, flags=0, final=true}; BGSSaveLoadFileEntry
			//   supplies its explicit file name/device; RVA 0x5EFDBB supplies an
			//   explicit name with device=-1.
			// - ID 2228039 (1.11.240 RVA 0xBF98D0) is synchronous: after submitting
			//   work it spins at 0xBF9940 until the worker event is signaled. Calling it
			//   from an F4SE main-thread task deadlocks load progress.
			// - The vanilla UI instead calls BGSSaveLoadFileEntry::LoadData, then the
			//   non-blocking QueueLoadEntry routine at RVA 0xBFEBF0 / ID 4483966. That
			//   routine sets kLoadGame, stores the manager-owned entry, and copies its
			//   name/device into save-load-thread-owned storage before returning.
			// The save and synchronous-load bodies were also compared against 1.11.221
			// by harness/fndiff.py. ID 4483966 is identical between 1.11.221
			// (RVA 0xBFE860) and 1.11.240; its role is established by its AE callers.
			if (a_request.kind == tools::GameOperationKind::kSave)
			{
				using QueueSave_t = bool (*)(RE::BGSSaveLoadManager*, const char*, std::uint32_t, std::uint32_t, bool);
				static REL::Relocation<QueueSave_t> queueSave{ REL::ID(2228036) };
				if (!queueSave(
						std::addressof(a_manager), a_request.name.c_str(), 0xFFFFFFFFu, 0, true))
					return {
						.accepted = false,
						.errorCode = 503,
						.error = "Fallout 4 rejected the native 'save' request",
					};
				return { .accepted = true };
			}

			auto* entry = FindNativeSaveEntry(a_manager, a_request.name);
			if (!entry)
			{
				// Direct BuildSaveGameList calls are used by vanilla manager/UI paths.
				// Player id zero builds the all-characters view needed by a named load.
				a_manager.BuildSaveGameList(0);
				entry = FindNativeSaveEntry(a_manager, a_request.name);
			}
			if (!entry)
				return {
					.accepted = false,
					.errorCode = 503,
					.error = std::format(
						"Fallout 4 did not expose save '{}' in its native save list",
						a_request.name),
				};

			// This is the same preparation sequence used by both vanilla callers of
			// QueueLoadEntry. The entry remains owned by BGSSaveLoadManager::saveGameList.
			entry->LoadData();
			if (entry->corrupt)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = std::format(
						"save '{}' is marked corrupt by Fallout 4", a_request.name),
				};

			using QueueLoadEntry_t =
				void (*)(RE::BGSSaveLoadManager*, RE::BGSSaveLoadFileEntry*);
			static REL::Relocation<QueueLoadEntry_t> queueLoadEntry{ REL::ID(4483966) };
			queueLoadEntry(std::addressof(a_manager), entry);

			const auto loadTask =
				static_cast<std::uint32_t>(RE::BGSSaveLoadManager::QUEUED_TASK::kLoadGame);
			if (a_manager.queuedEntryToLoad != entry || (a_manager.queuedTasks & loadTask) == 0)
				return {
					.accepted = false,
					.errorCode = 503,
					.error = "Fallout 4 did not retain the queued native load request",
				};
			return { .accepted = true };
		}

		DispatchResult DispatchNative(const tools::GameOperationRequest& a_request)
		{
			const auto lifecycle = Lifecycle::GetSnapshot();
			if (!lifecycle.gameDataReady)
				return {
					.accepted = false,
					.errorCode = 503,
					.error = "game actions are unavailable until F4SE gameDataReady",
				};
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = "a game load is already in progress",
				};
			if (a_request.kind == tools::GameOperationKind::kSave &&
				(lifecycle.inMainMenu || !RE::PlayerCharacter::GetSingleton()))
				return {
					.accepted = false,
					.errorCode = 503,
					.error = "saving requires a loaded playable world",
				};

			auto* manager = RE::BGSSaveLoadManager::GetSingleton();
			if (!manager)
				return {
					.accepted = false,
					.errorCode = 503,
					.error = "BGSSaveLoadManager is unavailable",
				};
			if ((manager->queuedTasks & kMutatingTaskMask) != 0 || manager->saveOperationDelayCounter != 0 ||
				manager->queuedEntryToLoad || manager->saveLoadTasksThread.busy)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = "the native save/load manager is busy",
				};

			const auto nativeDirectory = NativeSaveDirectory(*manager);
			if (a_request.directory &&
				!SameDirectory(nativeDirectory, fs::path(*a_request.directory)))
				return {
					.accepted = false,
					.errorCode = 409,
					.error = std::format(
						"directory '{}' is read-only selection and does not match Fallout 4's native save directory '{}'",
						*a_request.directory, Utf8String(nativeDirectory)),
				};

			std::error_code ec;
			const auto      savePath =
				nativeDirectory / (a_request.name + ".fos");
			const bool exists = fs::exists(savePath, ec);
			if (ec)
				return {
					.accepted = false,
					.errorCode = 503,
					.error = std::format(
						"could not inspect save path '{}': {}",
						Utf8String(savePath), ec.message()),
				};
			if (a_request.kind == tools::GameOperationKind::kSave)
			{
				const auto sidecarPath =
					nativeDirectory / (a_request.name + ".f4se");
				const bool sidecarExists = fs::exists(sidecarPath, ec);
				if (ec)
					return {
						.accepted = false,
						.errorCode = 503,
						.error = std::format(
							"could not inspect save sidecar path '{}': {}",
							Utf8String(sidecarPath), ec.message()),
					};
				if (exists || sidecarExists)
					return {
						.accepted = false,
						.errorCode = 409,
						.error = std::format(
							"save '{}' appeared before dispatch; refusing to overwrite",
							a_request.name),
					};
			}
			if (a_request.kind == tools::GameOperationKind::kLoad && !exists)
				return {
					.accepted = false,
					.errorCode = 404,
					.error =
						std::format("save '{}' disappeared before dispatch", a_request.name),
				};

			return QueueNativeOperation(*manager, a_request);
		}

		DispatchResult DispatchCalendarAdvance(double a_hours)
		{
			const auto lifecycle = Lifecycle::GetSnapshot();
			if (!lifecycle.gameDataReady || !lifecycle.gameLoaded || lifecycle.inMainMenu)
				return {
					.accepted = false,
					.errorCode = 503,
					.error = "calendar advancement requires a loaded playable world",
				};
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = "calendar advancement is unavailable while a game load is in progress",
				};

			auto* calendar = RE::Calendar::GetSingleton();
			if (!calendar || !calendar->gameYear || !calendar->gameMonth ||
				!calendar->gameDay || !calendar->gameHour || !calendar->gameDaysPassed)
				return {
					.accepted = false,
					.errorCode = 503,
					.error = "Fallout 4 Calendar globals are unavailable",
				};

			const auto ReadInt = [](const RE::TESGlobal& a_global) -> std::optional<std::int32_t> {
				const auto value = static_cast<double>(a_global.value);
				if (!std::isfinite(value) ||
					std::trunc(value) != value ||
					value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
					value > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
					return std::nullopt;
				return static_cast<std::int32_t>(value);
			};
			const auto year = ReadInt(*calendar->gameYear);
			const auto month = ReadInt(*calendar->gameMonth);
			const auto day = ReadInt(*calendar->gameDay);
			if (!year || !month || !day)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = "Fallout 4 Calendar date globals are invalid",
				};

			const auto advanced = tools::game::AdvanceFallout4Calendar(
				{
					.rawYear = *year,
					.month = *month,
					.day = *day,
					.hour = calendar->gameHour->value,
					.daysPassed = calendar->gameDaysPassed->value,
					.rawDaysPassed = calendar->rawDaysPassed,
				},
				a_hours);
			if (!advanced.value)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = std::format(
						"Fallout 4 Calendar state could not be advanced: {}",
						advanced.error),
				};

			const auto& state = advanced.value->state;
			const auto  displayYear = tools::game::Fallout4DisplayYear(state.rawYear);
			if (!displayYear)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = "Fallout 4 Calendar raw year cannot be displayed",
				};

			calendar->gameYear->value = static_cast<float>(state.rawYear);
			calendar->gameMonth->value = static_cast<float>(state.month);
			calendar->gameDay->value = static_cast<float>(state.day);
			calendar->gameHour->value = static_cast<float>(state.hour);
			calendar->gameDaysPassed->value = static_cast<float>(state.daysPassed);
			calendar->rawDaysPassed = static_cast<float>(state.rawDaysPassed);

			return {
				.accepted = true,
				.completed = true,
				.errorCode = 0,
				.result = json{
					{ "gameHour", calendar->gameHour->value },
					{ "daysPassed", calendar->gameDaysPassed->value },
					{ "day", calendar->gameDay->value },
					{ "month", calendar->gameMonth->value },
					{ "year", *displayYear },
					{ "rawYear", state.rawYear },
					{ "dayDelta", advanced.value->dayDelta },
				},
			};
		}

		TrackedDispatchReceipt DispatchTracked(
			tools::GameOperationKind a_kind, std::string a_name,
			std::function<DispatchResult()> a_dispatch)
		{
			auto operation = Lifecycle::BeginOperation(a_kind, std::move(a_name));
			if (!operation)
				throw ToolError(409, "another DevBench game operation is still active");

			const auto operationId = operation->operationId;
			const auto tasks = F4SE::GetTaskInterface();
			if (!tasks)
			{
				Lifecycle::MarkOperationDispatchFailed(
					operationId, "F4SE TaskInterface unavailable");
				throw ToolError(503, "F4SE TaskInterface unavailable");
			}

			auto work = std::make_shared<DispatchWork>();
			auto future = work->result.get_future();
			try
			{
				tasks->AddTask(
					[work, dispatch = std::move(a_dispatch), operationId]() {
						auto expected = WorkPhase::kPending;
						if (!work->phase.compare_exchange_strong(
								expected, WorkPhase::kRunning, std::memory_order_acq_rel))
							return;

						DispatchResult result;
						if (!Lifecycle::MarkOperationDispatching(operationId))
						{
							result = {
								.accepted = false,
								.errorCode = 409,
								.error = "game operation no longer owns the dispatch slot",
							};
						}
						else
						{
							try
							{
								result = dispatch();
							}
							catch (const std::exception& a_exception)
							{
								result = {
									.accepted = false,
									.errorCode = 500,
									.error = a_exception.what(),
								};
							}
							catch (...)
							{
								result = {
									.accepted = false,
									.errorCode = 500,
									.error = "native game action dispatch failed",
								};
							}
						}

						if (result.accepted)
						{
							if (result.completed)
							{
								if (!Lifecycle::CompleteOperation(
										operationId, result.result))
								{
									result = {
										.accepted = false,
										.errorCode = 409,
										.error = "game operation lost ownership before completion",
									};
									Lifecycle::MarkOperationDispatchFailed(
										operationId, result.error);
								}
							}
							else
								Lifecycle::MarkOperationQueued(operationId);
						}
						else
							Lifecycle::MarkOperationDispatchFailed(
								operationId, result.error);
						work->phase.store(WorkPhase::kFinished, std::memory_order_release);
						try
						{
							work->result.set_value(std::move(result));
						}
						catch (...)
						{}
					});
			}
			catch (const std::exception& a_exception)
			{
				Lifecycle::MarkOperationDispatchFailed(
					operationId, a_exception.what());
				throw ToolError(503, std::format("game action could not be queued: {}", a_exception.what()));
			}

			if (future.wait_for(kDispatchTimeout) != std::future_status::ready)
			{
				auto expected = WorkPhase::kPending;
				if (work->phase.compare_exchange_strong(
						expected, WorkPhase::kCancelled, std::memory_order_acq_rel))
				{
					Lifecycle::CancelOperation(
						operationId,
						"main-thread dispatch timed out and was cancelled before it started");
					throw ToolError(504,
						"main-thread game action did not start within 5000ms and was cancelled before execution");
				}
				return {
					.operationId = operationId,
					.actionCursor = operation->actionCursor,
				};
			}

			auto result = future.get();
			if (!result.accepted)
				throw ToolError(result.errorCode, result.error);
			return {
				.operationId = operationId,
				.actionCursor = operation->actionCursor,
				.result = std::move(result),
			};
		}

		tools::GameQueueReceipt QueueOperation(tools::GameOperationRequest a_request)
		{
			const auto kind = a_request.kind;
			const auto name = a_request.name;
			auto       receipt = DispatchTracked(
				kind, name,
				[request = std::move(a_request)]() {
					return DispatchNative(request);
				});
			if (!receipt.result)
				throw ToolError(504, std::format(
										 "main-thread game action started but did not acknowledge within 5000ms; "
										 "operation {} ownership is retained, poll game action='status'",
										 receipt.operationId));
			return {
				.operationId = receipt.operationId,
				.actionCursor = receipt.actionCursor,
			};
		}

		tools::GameAdvanceReceipt AdvanceTime(double a_hours)
		{
			auto receipt = DispatchTracked(
				tools::GameOperationKind::kAdvanceTime,
				std::format("{} hours", a_hours),
				[a_hours]() {
					return DispatchCalendarAdvance(a_hours);
				});
			if (!receipt.result)
				return {
					.operationId = receipt.operationId,
					.actionCursor = receipt.actionCursor,
					.completed = false,
				};
			return {
				.operationId = receipt.operationId,
				.actionCursor = receipt.actionCursor,
				.completed = receipt.result->completed,
				.result = std::move(receipt.result->result),
			};
		}
	}

	tools::GameBackend MakeGameBackend()
	{
		return tools::GameBackend{
			.listSaves = &EnumerateNativeSaves,
			.status = &ReadStatus,
			.queueOperation = &QueueOperation,
			.listSavesWithOptions = &EnumerateSaves,
			.readSaveMetadata = &ReadSaveMetadata,
			.resolveMutationDirectory = &ResolveMutationDirectory,
			.advanceTime = &AdvanceTime,
		};
	}
}

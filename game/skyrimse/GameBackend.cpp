#include "GameBackend.h"

#include "Lifecycle.h"
#include "MainThread.h"
#include "save/SkyrimCalendar.h"
#include "save/SkyrimSaveHeader.h"

#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <future>
#include <limits>
#include <ranges>

namespace dvb::skyrimse
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr auto kDispatchTimeout = std::chrono::milliseconds(5000);

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

		std::string ResolveSaveDirectoryOnMainThread()
		{
			const auto logDirectory = SKSE::log::log_directory();
			if (!logDirectory)
				throw ToolError(503, "SKSE log directory is unavailable");

			fs::path local = "Saves";
			if (const auto ini = RE::INISettingCollection::GetSingleton())
			{
				if (const auto setting = ini->GetSetting("sLocalSavePath:General"))
				{
					if (const auto path = setting->GetString(); path && *path)
						local = path;
				}
			}
			const auto directory =
				local.is_absolute() ? local : logDirectory->parent_path() / local;
			return Utf8String(directory);
		}

		std::string ResolveSaveDirectory()
		{
			const auto value = MainThread::RunAndWait([]() -> json {
				return ResolveSaveDirectoryOnMainThread();
			});
			return value.get<std::string>();
		}

		tools::GameSaveList EnumerateSaves(tools::GameSaveListRequest a_request)
		{
			const fs::path directory = a_request.directory ?
			                               fs::path(*a_request.directory) :
			                               fs::path(ResolveSaveDirectory());
			std::error_code ec;
			fs::directory_iterator it(directory, ec);
			if (ec)
				throw ToolError(503, std::format(
										 "could not enumerate Skyrim save directory '{}': {}",
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
						return static_cast<char>(
							a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
					});
					if (extension == ".ess")
					{
						const auto modified = entry.last_write_time(ec);
						if (ec)
							throw ToolError(503, std::format(
													 "could not read save modification time '{}': {}",
													 Utf8String(entry.path()), ec.message()));
						const auto systemTime =
							std::chrono::clock_cast<std::chrono::system_clock>(modified);
						out.saves.push_back({
							.name = Utf8String(entry.path().stem()),
							.modifiedUnix = std::chrono::duration_cast<std::chrono::seconds>(
								systemTime.time_since_epoch())
							                    .count(),
						});
					}
				}
				it.increment(ec);
				if (ec)
					throw ToolError(503, std::format(
											 "could not continue enumerating Skyrim save directory '{}': {}",
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
			return save::ReadSkyrimSaveHeader(
				fs::path(a_directory) / (a_name + ".ess"));
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
										 "directory '{}' is read-only selection and does not match Skyrim's native save directory '{}'",
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
					snapshot.gameDataReady && !snapshot.inLoadingMenu &&
						!snapshot.loadInProgress },
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

		bool NativeSaveLoadBusy()
		{
			if (const auto game = RE::BGSSaveLoadGame::GetSingleton();
				game && (game->GetSaveGameLoading() || game->GetSaveGameSaving()))
				return true;
			if (const auto manager = RE::BGSSaveLoadManager::GetSingleton())
			{
				const auto& runtime = manager->GetRuntimeData();
				return runtime.thread.isBusy ||
				       runtime.thread.asyncSaveLoadOperationQueue.numEntries != 0 ||
				       runtime.unk370.numEntries != 0;
			}
			return false;
		}

		DispatchResult QueueNativeOperation(
			RE::BGSSaveLoadManager& a_manager, const tools::GameOperationRequest& a_request)
		{
			// These are the exact request entrypoints wrapped by CommonLibSSE-NG's
			// BGSSaveLoadManager::Save/Load methods. They enqueue work into the native
			// save/load manager and return its acceptance result. They are deliberately
			// not console "save"/"load" commands, which synchronously freeze the VM from
			// the console drain and can deadlock.
			if (a_request.kind == tools::GameOperationKind::kSave)
			{
				using QueueSave_t =
					bool (*)(RE::BGSSaveLoadManager*, std::int32_t, std::uint32_t, const char*);
				static REL::Relocation<QueueSave_t> queueSave{
					RELOCATION_ID(34818, 35727)
				};
				if (!queueSave(
						std::addressof(a_manager), 2, 0, a_request.name.c_str()))
					return {
						.accepted = false,
						.errorCode = 503,
						.error = "Skyrim rejected the native save request",
					};
				return { .accepted = true };
			}

			using QueueLoad_t =
				bool (*)(RE::BGSSaveLoadManager*, const char*, std::int32_t, std::uint32_t, bool);
			static REL::Relocation<QueueLoad_t> queueLoad{
				RELOCATION_ID(34819, 35728)
			};
			if (!queueLoad(
					std::addressof(a_manager), a_request.name.c_str(), -1, 0, false))
				return {
					.accepted = false,
					.errorCode = 503,
					.error = "Skyrim rejected the native load request",
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
					.error = "game actions are unavailable until SKSE dataLoaded",
				};
			if (lifecycle.inLoadingMenu || lifecycle.loadInProgress)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = "a game load is already in progress",
				};
			if (a_request.kind == tools::GameOperationKind::kSave &&
				(lifecycle.inMainMenu || !lifecycle.gameLoaded ||
					!RE::PlayerCharacter::GetSingleton() ||
					!RE::PlayerCharacter::GetSingleton()->Get3D()))
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
			if (NativeSaveLoadBusy())
				return {
					.accepted = false,
					.errorCode = 409,
					.error = "the native save/load manager is busy",
				};

			const fs::path nativeDirectory{ ResolveSaveDirectoryOnMainThread() };
			if (a_request.directory &&
				!SameDirectory(nativeDirectory, fs::path(*a_request.directory)))
				return {
					.accepted = false,
					.errorCode = 409,
					.error = std::format(
						"directory '{}' is read-only selection and does not match Skyrim's native save directory '{}'",
						*a_request.directory, Utf8String(nativeDirectory)),
				};

			std::error_code ec;
			const auto      savePath = nativeDirectory / (a_request.name + ".ess");
			const bool      exists = fs::exists(savePath, ec);
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
				const auto sidecarPath = nativeDirectory / (a_request.name + ".skse");
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
			else if (!exists)
			{
				return {
					.accepted = false,
					.errorCode = 404,
					.error =
						std::format("save '{}' disappeared before dispatch", a_request.name),
				};
			}

			return QueueNativeOperation(*manager, a_request);
		}

		std::optional<std::int32_t> ReadIntegerGlobal(const RE::TESGlobal* a_global)
		{
			if (!a_global)
				return std::nullopt;
			const auto value = static_cast<double>(a_global->value);
			if (!std::isfinite(value) || std::trunc(value) != value ||
				value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
				value > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
				return std::nullopt;
			return static_cast<std::int32_t>(value);
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
					.error = "Skyrim Calendar globals are unavailable",
				};

			const auto year = ReadIntegerGlobal(calendar->gameYear);
			const auto month = ReadIntegerGlobal(calendar->gameMonth);
			const auto day = ReadIntegerGlobal(calendar->gameDay);
			const auto hoursPerDay = static_cast<double>(RE::Calendar::GetHoursPerDay());
			if (!year || !month || !day)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = "Skyrim Calendar date globals are invalid",
				};

			const auto advanced = save::AdvanceSkyrimCalendar(
				{
					.year = *year,
					.month = *month,
					.day = *day,
					.hour = calendar->gameHour->value,
					.daysPassed = calendar->gameDaysPassed->value,
					.rawDaysPassed = calendar->rawDaysPassed,
					.hoursPerDay = hoursPerDay,
				},
				a_hours);
			if (!advanced.value)
				return {
					.accepted = false,
					.errorCode = 409,
					.error = std::format(
						"Skyrim Calendar state could not be advanced: {}",
						advanced.error),
				};

			const auto& state = advanced.value->state;
			calendar->gameYear->value = static_cast<float>(state.year);
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
					{ "year", calendar->gameYear->value },
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
			const auto tasks = SKSE::GetTaskInterface();
			if (!tasks)
			{
				Lifecycle::MarkOperationDispatchFailed(
					operationId, "SKSE TaskInterface unavailable");
				throw ToolError(503, "SKSE TaskInterface unavailable");
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
							{
								Lifecycle::MarkOperationQueued(operationId);
							}
						}
						else
						{
							Lifecycle::MarkOperationDispatchFailed(
								operationId, result.error);
						}
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
				throw ToolError(
					503,
					std::format(
						"game action could not be queued: {}", a_exception.what()));
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
					throw ToolError(
						504,
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
			auto receipt = DispatchTracked(
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
			.savePolicy = {
				.gameName = "Skyrim",
				.saveExtension = ".ess",
				.coSaveExtension = ".skse",
				.metadataDescription = "TESV save header metadata",
			},
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

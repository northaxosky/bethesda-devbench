#pragma once

#include "ToolRegistry.h"
#include "tools/game/SaveMetadata.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::tools
{
	inline constexpr std::size_t  kMaxSaveNameBytes = 200;
	inline constexpr std::size_t  kMaxSaveFilterBytes = 200;
	inline constexpr std::size_t  kMaxSaveDirectoryBytes = 32767;
	inline constexpr std::int64_t kMaxSaveListLimit = 500;
	inline constexpr double       kMaxGameAdvanceHours = 100000.0;

	struct GameSavePolicy
	{
		std::string gameName = "Fallout 4";
		std::string saveExtension = ".fos";
		std::string coSaveExtension = ".f4se";
		std::string metadataDescription = "save header metadata";
	};

	enum class GameOperationKind
	{
		kSave,
		kLoad,
		kAdvanceTime,
		kWait,
		kSleep,
	};

	enum class GameOperationPhase
	{
		kReserved,
		kDispatching,
		kQueued,
		kStarted,
		kCompleted,
		kSucceeded,
		kFailed,
		kCancelled,
	};

	struct GameSaveInfo
	{
		std::string                       name;
		std::optional<std::int64_t>       modifiedUnix;
		std::optional<game::SaveMetadata> metadata;
		std::optional<std::string>        metadataError;
	};

	struct GameSaveList
	{
		std::string               directory;
		std::vector<GameSaveInfo> saves;
	};

	struct GameSaveListRequest
	{
		std::optional<std::string> directory;
	};

	struct GameOperationRequest
	{
		GameOperationKind          kind = GameOperationKind::kSave;
		std::string                name;
		std::optional<std::string> directory;
	};

	struct GameQueueReceipt
	{
		std::uint64_t operationId = 0;
		std::uint64_t actionCursor = 0;
	};

	struct GameAdvanceReceipt
	{
		std::uint64_t       operationId = 0;
		std::uint64_t       actionCursor = 0;
		bool                completed = false;
		std::optional<json> result;
	};

	struct GameOperationSnapshot
	{
		std::uint64_t                operationId = 0;
		GameOperationKind            kind = GameOperationKind::kSave;
		std::string                  name;
		GameOperationPhase           phase = GameOperationPhase::kReserved;
		std::uint64_t                actionCursor = 0;
		std::optional<std::uint64_t> startedCursor;
		std::optional<std::uint64_t> completedCursor;
		std::optional<bool>          success;
		std::optional<std::string>   observedName;
		std::optional<json>          result;
		std::string                  error;
		bool                         active = false;
	};

	// Tracks one game mutation at a time. A completed/failed operation remains
	// available as the latest status, while only an active operation blocks the next
	// mutation.
	class GameOperationTracker
	{
	public:
		std::optional<GameOperationSnapshot> TryBegin(
			GameOperationKind a_kind,
			std::string       a_name,
			std::uint64_t     a_actionCursor);
		bool                                 MarkDispatching(std::uint64_t a_operationId);
		bool                                 MarkQueued(std::uint64_t a_operationId);
		bool                                 MarkDispatchFailed(std::uint64_t a_operationId, std::string a_error);
		bool                                 MarkCancelled(std::uint64_t a_operationId, std::string a_error);
		bool                                 MarkCompleted(std::uint64_t a_operationId, json a_result);
		bool                                 MarkCompleted(std::uint64_t a_operationId, bool a_success,
			json a_result, std::string a_error = {});
		bool                                 ObserveStarted(GameOperationKind a_kind,
			const std::optional<std::string>& a_name,
			std::uint64_t                     a_eventCursor);
		bool                                 ObserveCompleted(GameOperationKind a_kind,
			const std::optional<std::string>&   a_name,
			std::optional<bool>                 a_success,
			std::uint64_t                       a_eventCursor);
		std::optional<GameOperationSnapshot> GetSnapshot() const;
		void                                 Reset();

	private:
		mutable std::mutex                   mutex_;
		std::uint64_t                        nextOperationId_ = 0;
		std::optional<GameOperationSnapshot> operation_;
	};

	std::string_view GameOperationKindName(GameOperationKind a_kind);
	std::string_view GameOperationPhaseName(GameOperationPhase a_phase);

	struct GameBackend
	{
		GameSavePolicy                                           savePolicy;
		std::function<GameSaveList()>                         listSaves;
		std::function<json()>                                 status;
		std::function<GameQueueReceipt(GameOperationRequest)> queueOperation;
		std::function<GameSaveList(GameSaveListRequest)>      listSavesWithOptions;
		std::function<game::SaveMetadataResult(
			const std::string&, const std::string&)>
			readSaveMetadata;
		std::function<std::string(
			const std::optional<std::string>&)>
												  resolveMutationDirectory;
		std::function<GameAdvanceReceipt(double)> advanceTime;
	};

	ToolDescriptor BuildGameDescriptor(const GameSavePolicy& a_policy = {});
	void           RegisterGameTool(ToolRegistry& a_registry, bool a_allowActions, GameBackend a_backend);
}

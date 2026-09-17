#pragma once

#include "Json.h"
#include "tools/GameTool.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dvb
{
	class EventBus;
}

namespace dvb::skyrimse::Lifecycle
{
	struct Snapshot
	{
		bool                                        gameDataReady = false;
		bool                                        gameLoaded = false;
		bool                                        loadInProgress = false;
		bool                                        inMainMenu = false;
		bool                                        inLoadingMenu = false;
		bool                                        menuTrackingReady = false;
		bool                                        cellTrackingReady = false;
		std::optional<bool>                         postLoadSucceeded;
		std::uint64_t                               eventCursor = 0;
		std::optional<tools::GameOperationSnapshot> operation;
		std::string                                 lastEvent;
		std::vector<std::string>                    openMenus;
	};

	void     Initialize(EventBus& a_events);
	void     Reset();
	void     OnSKSEMessage(std::uint32_t a_type, std::uint32_t a_dataLen, const void* a_data);
	Snapshot GetSnapshot();

	std::optional<tools::GameOperationSnapshot> BeginOperation(
		tools::GameOperationKind a_kind, std::string a_name);
	bool MarkOperationDispatching(std::uint64_t a_operationId);
	bool MarkOperationQueued(std::uint64_t a_operationId);
	bool MarkOperationDispatchFailed(std::uint64_t a_operationId, std::string a_error);
	bool CancelOperation(std::uint64_t a_operationId, std::string a_error);
	bool CompleteOperation(std::uint64_t a_operationId, json a_result);
	bool CompleteOperation(std::uint64_t a_operationId, bool a_success,
		json a_result, std::string a_error = {});
}

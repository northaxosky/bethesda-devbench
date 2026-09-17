#include "RestBackend.h"

#include "game/skyrimse/Lifecycle.h"

#include <format>
#include <utility>

namespace dvb::skyrimse
{
	namespace
	{
		tools::GameOperationKind OperationKind(tools::rest::RestKind a_kind)
		{
			return a_kind == tools::rest::RestKind::kSleep ?
			           tools::GameOperationKind::kSleep :
			           tools::GameOperationKind::kWait;
		}

		std::string OperationName(
			tools::rest::RestKind a_kind, int a_hours,
			const std::optional<std::string>& a_target)
		{
			auto name = std::format("{} {} hour{}",
				a_kind == tools::rest::RestKind::kSleep ? "sleep" : "wait",
				a_hours, a_hours == 1 ? "" : "s");
			if (a_target)
				name += std::format(" at {}", *a_target);
			return name;
		}

		std::string FailureReason(const tools::rest::RestResult& a_result)
		{
			if (!a_result.message.empty())
				return a_result.message;
			if (a_result.interrupted)
				return "native wait/sleep was interrupted before the requested duration completed";
			return std::format(
				"native wait/sleep outcome was {}",
				tools::rest::RestResultStatusName(a_result.status));
		}
	}

	tools::rest::RestOperationCoordinator MakeRestOperationCoordinator()
	{
		return {
			.tryBegin =
				[](tools::rest::RestKind a_kind, int a_hours,
					const std::optional<std::string>& a_target)
				-> std::optional<tools::rest::RestOperationReservation> {
				auto operation = Lifecycle::BeginOperation(
					OperationKind(a_kind), OperationName(a_kind, a_hours, a_target));
				if (!operation)
					return std::nullopt;
				return tools::rest::RestOperationReservation{
					.operationId = operation->operationId,
					.actionCursor = operation->actionCursor,
				};
			},
			.markDispatching =
				[](std::uint64_t a_operationId) {
					return Lifecycle::MarkOperationDispatching(a_operationId);
				},
			.markQueued =
				[](std::uint64_t a_operationId) {
					return Lifecycle::MarkOperationQueued(a_operationId);
				},
			.finish =
				[](std::uint64_t                   a_operationId,
					const tools::rest::RestResult& a_result) {
					const bool success = tools::rest::RestResultSucceeded(a_result);
					return Lifecycle::CompleteOperation(
						a_operationId, success,
						tools::rest::RestResultToJson(a_result),
						success ? std::string{} : FailureReason(a_result));
				},
			.fail =
				[](std::uint64_t a_operationId, std::string a_error) {
					return Lifecycle::MarkOperationDispatchFailed(
						a_operationId, std::move(a_error));
				},
			.cancelReserved =
				[](std::uint64_t a_operationId, std::string a_error) {
					return Lifecycle::CancelOperation(
						a_operationId, std::move(a_error));
				},
		};
	}
}

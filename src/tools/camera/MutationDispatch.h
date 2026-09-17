#pragma once

#include "ToolRegistry.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

namespace dvb::tools
{
	// A queued mutation owns one generation. A timeout can revoke a still-pending
	// generation, so the eventual main-thread task becomes a no-op. Once execution
	// starts, its outcome is deliberately reported as uncertain and must not be retried.
	class CancelableMutationRunner
	{
	public:
		using Submit = std::function<bool(std::function<void()>)>;

		explicit CancelableMutationRunner(
			Submit                    a_submit,
			std::chrono::milliseconds a_timeout = std::chrono::milliseconds(5000)) :
			submit_(std::move(a_submit)),
			timeout_(a_timeout)
		{}

		json Run(std::string_view a_label, std::function<json()> a_mutation)
		{
			enum class Phase
			{
				kReserved,
				kRunning,
				kCancelled,
				kFinished,
			};

			struct Work
			{
				std::uint64_t      generation = 0;
				std::atomic<Phase> phase{ Phase::kReserved };
				std::promise<json> result;
			};

			const auto generation =
				nextGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
			auto work = std::make_shared<Work>();
			work->generation = generation;
			auto future = work->result.get_future();

			bool submitted = false;
			try
			{
				submitted = submit_ && submit_(
										   [work, mutation = std::move(a_mutation)]() mutable {
											   auto expected = Phase::kReserved;
											   if (!work->phase.compare_exchange_strong(
													   expected, Phase::kRunning, std::memory_order_acq_rel))
												   return;
											   try
											   {
												   auto result = mutation();
												   work->phase.store(Phase::kFinished, std::memory_order_release);
												   work->result.set_value(std::move(result));
											   }
											   catch (...)
											   {
												   work->phase.store(Phase::kFinished, std::memory_order_release);
												   try
												   {
													   work->result.set_exception(std::current_exception());
												   }
												   catch (...)
												   {}
											   }
										   });
			}
			catch (const std::exception& a_exception)
			{
				throw ToolError(503,
					std::format(
						"{} generation {} could not be queued: {}",
						a_label, generation, a_exception.what()));
			}
			if (!submitted)
				throw ToolError(503,
					std::format(
						"{} generation {} could not be queued", a_label, generation));

			if (future.wait_for(timeout_) == std::future_status::ready)
				return future.get();

			auto expected = Phase::kReserved;
			if (work->phase.compare_exchange_strong(
					expected, Phase::kCancelled, std::memory_order_acq_rel))
				throw ToolError(504,
					std::format(
						"{} generation {} timed out and was cancelled before execution",
						a_label, generation));

			if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
				return future.get();

			throw ToolError(504,
				std::format(
					"{} generation {} started but did not acknowledge within {}ms; "
					"the outcome is uncertain and must not be retried",
					a_label, generation, timeout_.count()));
		}

	private:
		Submit                     submit_;
		std::chrono::milliseconds  timeout_;
		std::atomic<std::uint64_t> nextGeneration_{ 0 };
	};
}

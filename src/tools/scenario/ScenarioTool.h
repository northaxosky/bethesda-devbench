#pragma once

#include "EventBus.h"
#include "ToolRegistry.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace dvb::tools::scenario
{
	struct ScenarioConfig
	{
		std::size_t               historyLimit = 128;
		std::size_t               maxExpandedSteps = 10000;
		std::size_t               maxTranscriptBytes = 4 * 1024 * 1024;
		std::chrono::milliseconds maxRunTime{ 10 * 60 * 1000 };
		std::chrono::milliseconds maxStepTimeout{ 5 * 60 * 1000 };
		std::chrono::milliseconds maxWait{ 5 * 60 * 1000 };
		std::chrono::milliseconds maxPoll{ 10 * 1000 };
		std::chrono::milliseconds defaultWaitForTimeout{ 60 * 1000 };
		std::chrono::milliseconds defaultWaitUntilTimeout{ 30 * 1000 };
		std::chrono::milliseconds defaultWaitForPoll{ 100 };
		std::chrono::milliseconds defaultWaitUntilPoll{ 250 };
	};

	ToolDescriptor BuildScenarioDescriptor(const ScenarioConfig& a_config = {});

	struct ScenarioRunOptions
	{
		// Additional fields copied onto the final run result. Recording replay uses
		// this to expose restoration/coupling decisions without introducing another
		// run registry.
		json resultMetadata = json::object();

		// Called exactly once after the result is committed to history. The callback
		// runs on the scenario worker and must not call Shutdown().
		std::function<void(std::uint64_t, const json&)> onCompleted;

		// Called after the run is entered into history and before the worker is
		// notified. Useful for publishing an ordered domain-specific started event.
		std::function<void(std::uint64_t)> onSubmitted;

		// Invoked on the worker after status/elapsed fields are final but before the
		// result is committed to history.
		std::function<void(json&)> finalizeResult;

		// Owner used for run-scoped input operations. Empty selects
		// "scenario:<runId>".
		std::string owner;
	};

	/// Owns the single scenario worker and bounded run history. Keep this service
	/// alive at least as long as the ToolRegistry that contains its handler.
	class ScenarioService
	{
	public:
		ScenarioService(
			ToolRegistry&  a_registry,
			EventBus&      a_events,
			ScenarioConfig a_config = {});
		~ScenarioService();

		ScenarioService(const ScenarioService&) = delete;
		ScenarioService& operator=(const ScenarioService&) = delete;
		ScenarioService(ScenarioService&&) = delete;
		ScenarioService& operator=(ScenarioService&&) = delete;

		/// Register (or replace) the "scenario" descriptor and handler.
		bool Register();

		/// Submit through the same worker/history used by the public scenario tool.
		/// The arguments use the action='run' contract; status/cancel remain available
		/// through Status/Cancel and the registered handler.
		json Run(
			const json&              a_args,
			const ToolContext&       a_context = {},
			ScenarioRunOptions       a_options = {});
		json Status(std::uint64_t a_runId);
		json Cancel(std::uint64_t a_runId);

		/// Cooperatively cancel an active run and join the owned worker. Mutating
		/// callbacks already executing cannot be preempted and are allowed to return.
		void Shutdown();

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}

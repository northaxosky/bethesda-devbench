#include "ToolActionWorker.h"

#include "ToolRegistry.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace dvb::skyrimse::ui
{
	namespace
	{
		struct Request
		{
			std::string tool;
			json        args;
			std::string clientId;
			bool        recordToggle = false;
		};

		std::mutex              g_mutex;
		std::condition_variable g_cv;
		std::deque<Request>     g_queue;
		ToolRegistry*           g_registry = nullptr;
		std::jthread            g_worker;

		void Worker(std::stop_token a_stop)
		{
			for (;;)
			{
				Request       request;
				ToolRegistry* registry = nullptr;
				{
					std::unique_lock lock{ g_mutex };
					g_cv.wait(lock, [&] { return a_stop.stop_requested() || !g_queue.empty(); });
					if (a_stop.stop_requested() && g_queue.empty())
						return;
					request = std::move(g_queue.front());
					g_queue.pop_front();
					registry = g_registry;
				}
				if (!registry)
					continue;
				auto args = std::move(request.args);
				if (request.recordToggle)
				{
					const auto status = registry->Invoke(
						"record", json{ { "action", "status" } },
						ToolContext{ request.clientId });
					if (!status.ok)
					{
						logs::warn(
							"devbench: in-game {} status failed ({}): {}", request.clientId,
							status.errorCode, status.errorMessage);
						continue;
					}
					args = json{
						{ "action",
							status.value.value("recording", false) ? "stop" : "start" },
					};
				}
				const auto result = registry->Invoke(
					request.tool, args, ToolContext{ request.clientId });
				if (!result.ok)
					logs::warn(
						"devbench: in-game {} action failed ({}): {}", request.clientId,
						result.errorCode, result.errorMessage);
			}
		}
	}

	void StartToolActionWorker(ToolRegistry& a_registry)
	{
		const std::lock_guard lock{ g_mutex };
		g_registry = std::addressof(a_registry);
		if (!g_worker.joinable())
			g_worker = std::jthread(&Worker);
	}

	bool QueueToolAction(
		std::string a_tool, json a_args, std::string a_clientId)
	{
		{
			const std::lock_guard lock{ g_mutex };
			if (!g_registry || !g_worker.joinable() ||
				g_worker.get_stop_token().stop_requested())
				return false;
			g_queue.push_back(Request{
				.tool = std::move(a_tool),
				.args = std::move(a_args),
				.clientId = std::move(a_clientId),
				.recordToggle = false,
			});
		}
		g_cv.notify_one();
		return true;
	}

	bool QueueRecordToggle()
	{
		{
			const std::lock_guard lock{ g_mutex };
			if (!g_registry || !g_worker.joinable() ||
				g_worker.get_stop_token().stop_requested())
				return false;
			g_queue.push_back(Request{
				.tool = "record",
				.args = json::object(),
				.clientId = "hotkey",
				.recordToggle = true,
			});
		}
		g_cv.notify_one();
		return true;
	}

	void ShutdownToolActionWorker() noexcept
	{
		{
			const std::lock_guard lock{ g_mutex };
			g_registry = nullptr;
			g_queue.clear();
			if (g_worker.joinable())
				g_worker.request_stop();
		}
		g_cv.notify_all();
		if (g_worker.joinable())
			g_worker.join();
	}
}

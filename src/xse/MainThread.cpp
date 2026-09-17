#include "MainThread.h"

#include "GameState.h"
#include "ToolRegistry.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>

namespace
{
	std::atomic<int> g_lastTaskFrame{ -1 };
	std::atomic<int> g_pendingTasks{ 0 };
}

namespace dvb::MainThread
{
	json RunAndWait(std::function<json()> a_fn, std::chrono::milliseconds a_timeout, const std::atomic<bool>* a_keepWaiting)
	{
		auto* task = F4SE::GetTaskInterface();
		if (!task)
			throw ToolError(500, "F4SE TaskInterface unavailable");

		// Shared so the promise remains valid if the caller abandons or times out its wait.
		auto promise = std::make_shared<std::promise<json>>();
		auto future = promise->get_future();

		g_pendingTasks.fetch_add(1, std::memory_order_relaxed);
		task->AddTask([fn = std::move(a_fn), promise]() {
			struct Finally
			{
				~Finally()
				{
					g_lastTaskFrame.store(game::CurrentFrame(), std::memory_order_relaxed);
					g_pendingTasks.fetch_sub(1, std::memory_order_relaxed);
				}
			} finally;
			try
			{
				promise->set_value(fn());
			}
			catch (...)
			{
				try
				{
					promise->set_exception(std::current_exception());
				}
				catch (...)
				{
				}
			}
		});

		const int      frameAtStart = game::CurrentFrame();
		constexpr auto kSlice = std::chrono::milliseconds(100);
		auto           remaining = a_timeout;
		auto           status = std::future_status::timeout;
		while (remaining.count() > 0)
		{
			if (a_keepWaiting && !a_keepWaiting->load(std::memory_order_relaxed))
				return json(nullptr);
			const auto slice = std::min(remaining, kSlice);
			status = future.wait_for(slice);
			if (status == std::future_status::ready)
				break;
			remaining -= slice;
		}
		if (status != std::future_status::ready)
		{
			const int frameNow = game::CurrentFrame();
			if (frameAtStart < 0 || frameNow < 0)
				throw ToolError(504, std::format("main-thread task did not run within {}ms", a_timeout.count()));
			if (frameNow == frameAtStart)
				throw ToolError(504, std::format(
										 "main-thread task did not run within {}ms and the game frame counter has not advanced -- main thread hung or the game is fully paused; if no pause menu is open, only a process restart recovers",
										 a_timeout.count()));
			throw ToolError(504, std::format(
									 "main-thread task did not run within {}ms ({} frames elapsed -- main thread busy, a retry may succeed)",
									 a_timeout.count(), frameNow - frameAtStart));
		}

		return future.get();
	}

	int LastCompletedFrame()
	{
		return g_lastTaskFrame.load(std::memory_order_relaxed);
	}

	int PendingTasks()
	{
		return g_pendingTasks.load(std::memory_order_relaxed);
	}
}

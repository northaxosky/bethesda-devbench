#include "MainThread.h"

#include "GameState.h"
#include "MainThreadTask.h"
#include "RuntimeContext.h"
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
		const auto& runtime = GetRuntimeContext();
		if (!runtime.enqueueTask)
			throw ToolError(
				500, std::format("{} TaskInterface unavailable", runtime.profile.extenderName));
		if (a_keepWaiting && !a_keepWaiting->load(std::memory_order_relaxed))
			return json(nullptr);

		const auto deadline = QueuedTask::Clock::now() + a_timeout;
		auto       invocation = std::make_shared<QueuedTask>(std::move(a_fn), deadline);
		auto       future = invocation->GetFuture();

		g_pendingTasks.fetch_add(1, std::memory_order_relaxed);
		const bool queued = runtime.enqueueTask([invocation]() {
			struct Finally
			{
				~Finally()
				{
					g_lastTaskFrame.store(game::CurrentFrame(), std::memory_order_relaxed);
					g_pendingTasks.fetch_sub(1, std::memory_order_relaxed);
				}
			} finally;
			invocation->Run();
		});
		if (!queued)
		{
			g_pendingTasks.fetch_sub(1, std::memory_order_relaxed);
			invocation->Abandon();
			throw ToolError(
				503, std::format("{} task enqueue failed", runtime.profile.extenderName));
		}

		const int      frameAtStart = game::CurrentFrame();
		constexpr auto kSlice = std::chrono::milliseconds(100);
		while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
		{
			if (a_keepWaiting && !a_keepWaiting->load(std::memory_order_relaxed))
			{
				invocation->Abandon();
				if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
					return future.get();
				return json(nullptr);
			}
			const auto now = QueuedTask::Clock::now();
			if (now >= deadline)
				break;
			future.wait_until(std::min(deadline, now + kSlice));
		}
		if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
		{
			const bool abandoned = invocation->Abandon();
			if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
				return future.get();
			if (!abandoned)
				throw ToolError(504, std::format(
					"main-thread task did not finish within {}ms; it already started and may still complete",
					a_timeout.count()));
			const int frameNow = game::CurrentFrame();
			if (frameAtStart < 0 || frameNow < 0)
				throw ToolError(504, std::format(
					"main-thread task did not start within {}ms; queued task abandoned",
					a_timeout.count()));
			if (frameNow == frameAtStart)
				throw ToolError(504, std::format(
					"main-thread task did not start within {}ms; queued task abandoned and the game frame counter has not advanced -- main thread hung or the game is fully paused; if no pause menu is open, only a process restart recovers",
					a_timeout.count()));
			throw ToolError(504, std::format(
				"main-thread task did not start within {}ms; queued task abandoned ({} frames elapsed -- main thread busy)",
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

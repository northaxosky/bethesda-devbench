#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

namespace dvb::waits
{
	class FrameAwaiter
	{
	public:
		using FrameSource = std::function<int()>;  ///< -1 when unresolved

		struct Result
		{
			int  frame = -1;
			bool advanced = false;
		};

		explicit FrameAwaiter(FrameSource a_frame);

		FrameAwaiter(const FrameAwaiter&) = delete;
		FrameAwaiter& operator=(const FrameAwaiter&) = delete;

		std::optional<Result> Wait(int a_after, std::chrono::milliseconds a_wait);

		void Shutdown();

	private:
		FrameSource             frame_;
		std::mutex              mutex_;
		std::condition_variable wake_;
		bool                    stopping_ = false;
	};
}

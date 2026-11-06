#pragma once

#include "EventAwaiter.h"
#include "FrameAwaiter.h"

namespace httplib
{
	class Server;
}

namespace dvb::waits
{
	/// Long-poll /api/await/{events,frame}; waitMs is clamped to kMaxWait.
	class AwaitService
	{
	public:
		static constexpr std::chrono::milliseconds kMaxWait{ 25000 };

		AwaitService(EventBus& a_events, FrameAwaiter::FrameSource a_frame);

		void Mount(httplib::Server& a_http);

		void Shutdown();

	private:
		EventAwaiter events_;
		FrameAwaiter frames_;
	};
}

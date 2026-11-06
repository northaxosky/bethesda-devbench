#pragma once

#include "EventAwaiter.h"
#include "FrameAwaiter.h"

namespace httplib
{
	class Server;
}

namespace dvb::waits
{
	/// REST long-poll endpoints that return on the awaited change instead of being
	/// polled:
	///   GET /api/await/events?since=N&waitMs=M  -> {headSeq, oldestSeq, events}
	///   GET /api/await/frame?after=F&waitMs=M   -> {frame, advanced}
	/// waitMs defaults to 0 and is clamped to [0, kMaxWait].
	class AwaitService
	{
	public:
		static constexpr std::chrono::milliseconds kMaxWait{ 25000 };

		AwaitService(EventBus& a_events, FrameAwaiter::FrameSource a_frame, FrameAwaiter::Submitter a_submit);

		void Mount(httplib::Server& a_http);

		/// Wakes every blocked request; call before stopping the HTTP server.
		void Shutdown();

	private:
		EventAwaiter events_;
		FrameAwaiter frames_;
	};
}

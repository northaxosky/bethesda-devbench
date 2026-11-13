#include "AwaitService.h"

#include "Json.h"

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>

namespace dvb::waits
{
	namespace
	{
		void Write(httplib::Response& a_res, int a_status, const json& a_body)
		{
			a_res.status = a_status;
			a_res.set_content(Dump(a_body), "application/json");
		}

		void Reject(httplib::Response& a_res, int a_status, std::string a_message)
		{
			Write(a_res, a_status, json{ { "code", a_status }, { "error", std::move(a_message) } });
		}

		template <class T>
		std::optional<T> ParseParam(const httplib::Request& a_req, const char* a_name)
		{
			const auto text = a_req.get_param_value(a_name);
			T          value{};
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (error != std::errc{} || end != text.data() + text.size())
				return std::nullopt;
			return value;
		}

		// Absent -> 0; malformed -> nullopt so the caller can answer 400.
		std::optional<std::chrono::milliseconds> ParseWait(const httplib::Request& a_req)
		{
			std::int64_t ms = 0;
			if (a_req.has_param("waitMs"))
			{
				const auto parsed = ParseParam<std::int64_t>(a_req, "waitMs");
				if (!parsed)
					return std::nullopt;
				ms = *parsed;
			}
			return std::clamp(std::chrono::milliseconds(ms), std::chrono::milliseconds(0), AwaitService::kMaxWait);
		}

		json EventJson(const EventBus::Event& a_event)
		{
			return json{ { "seq", a_event.seq }, { "frame", a_event.frame }, { "topic", a_event.topic }, { "data", a_event.payload } };
		}
	}

	AwaitService::AwaitService(EventBus& a_events, FrameAwaiter::FrameSource a_frame) :
		events_(a_events), frames_(std::move(a_frame))
	{}

	void AwaitService::Shutdown()
	{
		events_.Shutdown();
		frames_.Shutdown();
	}

	void AwaitService::Mount(httplib::Server& a_http)
	{
		a_http.Get("/api/await/events", [this](const httplib::Request& a_req, httplib::Response& a_res) {
			const auto since = a_req.has_param("since") ? ParseParam<std::uint64_t>(a_req, "since") : std::nullopt;
			if (!since)
				return Reject(a_res, 400, "'since' is required and must be an unsigned integer");
			const auto wait = ParseWait(a_req);
			if (!wait)
				return Reject(a_res, 400, "'waitMs' must be an integer");
			const auto result = events_.Wait(*since, *wait);
			auto       events = json::array();
			for (const auto& event : result.events)
				events.push_back(EventJson(event));
			Write(a_res, 200, json{ { "headSeq", result.headSeq }, { "oldestSeq", result.oldestSeq }, { "events", std::move(events) } });
		});

		a_http.Get("/api/await/frame", [this](const httplib::Request& a_req, httplib::Response& a_res) {
			const auto after = a_req.has_param("after") ? ParseParam<int>(a_req, "after") : std::nullopt;
			if (!after)
				return Reject(a_res, 400, "'after' is required and must be an integer");
			const auto wait = ParseWait(a_req);
			if (!wait)
				return Reject(a_res, 400, "'waitMs' must be an integer");
			const auto result = frames_.Wait(*after, *wait);
			if (!result)
				return Reject(a_res, 503, "game frame counter is unavailable");
			Write(a_res, 200, json{ { "frame", result->frame }, { "advanced", result->advanced } });
		});
	}
}

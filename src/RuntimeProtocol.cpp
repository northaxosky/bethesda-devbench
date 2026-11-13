#include "RuntimeProtocol.h"

#include "Json.h"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>

namespace dvb
{
	std::string MakeInstanceId(std::uint32_t a_pid, std::uint64_t a_creationTime)
	{
		return std::format("{}-{:016X}", a_pid, a_creationTime);
	}

	void MountRequestGuard(httplib::Server& a_server, std::string a_instanceId, int a_port)
	{
		const auto port = std::to_string(a_port);
		// pre_request runs after routing and the body read, so a refusal is not a reset.
		a_server.set_pre_request_handler(
			[instanceId = std::move(a_instanceId),
				hosts = std::array{ "127.0.0.1:" + port, "localhost:" + port, "[::1]:" + port }](
				const httplib::Request& a_request, httplib::Response& a_response) {
				const auto refuse = [&](int a_status, const char* a_message, json a_extra = json::object()) {
					a_response.status = a_status;
					a_extra["code"] = a_status;
					a_extra["error"] = a_message;
					a_response.set_content(Dump(a_extra), "application/json");
					return httplib::Server::HandlerResponse::Handled;
				};
				const auto lower = [](std::string a_text) {
					std::ranges::transform(a_text, a_text.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
					return a_text;
				};

				// Host is case-insensitive; a rebound DNS name fails this match.
				if (std::ranges::find(hosts, lower(a_request.get_header_value("Host"))) == hosts.end())
					return refuse(403, "DevBench only serves loopback Host headers");
				if (a_request.has_header("Origin"))
					return refuse(403, "DevBench refuses browser-originated requests");
				if (a_request.method == "POST")
				{
					// A text/plain POST skips CORS preflight, so JSON is required.
					auto type = a_request.get_header_value("Content-Type");
					type = lower(type.substr(0, type.find(';')));
					type.erase(type.find_last_not_of(" 	") + 1);
					if (type != "application/json")
						return refuse(415, "POST requires Content-Type: application/json");
				}

				const bool        guardedRoute = a_request.path.starts_with("/api/") ||
				                                 a_request.path == "/mcp" || a_request.path.starts_with("/mcp/");
				const std::string header(kInstanceHeader);
				if (!guardedRoute || !a_request.has_header(header))
					return httplib::Server::HandlerResponse::Unhandled;
				if (a_request.get_header_value_count(header) == 1 &&
					a_request.get_header_value(header) == instanceId)
					return httplib::Server::HandlerResponse::Unhandled;
				return refuse(409, "DevBench instance changed; this request was not dispatched",
					json{ { "instanceId", instanceId } });
			});
	}
}

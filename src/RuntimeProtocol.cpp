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

	void MountInstanceGuard(httplib::Server& a_server, std::string a_instanceId)
	{
		a_server.set_pre_request_handler(
			[instanceId = std::move(a_instanceId)](const httplib::Request& a_request, httplib::Response& a_response) {
				const bool        guardedRoute = a_request.path.starts_with("/api/") ||
			                                     a_request.path == "/mcp" || a_request.path.starts_with("/mcp/");
				const std::string header(kInstanceHeader);
				if (!guardedRoute || !a_request.has_header(header))
					return httplib::Server::HandlerResponse::Unhandled;
				if (a_request.get_header_value_count(header) == 1 &&
					a_request.get_header_value(header) == instanceId)
					return httplib::Server::HandlerResponse::Unhandled;

				a_response.status = 409;
				a_response.set_content(
					json{
						{ "code", 409 },
						{ "error", "DevBench instance changed; this request was not dispatched" },
						{ "instanceId", instanceId },
					}
						.dump(),
					"application/json");
				return httplib::Server::HandlerResponse::Handled;
			});
	}

	void MountBrowserGuard(httplib::Server& a_server, int a_port)
	{
		const auto port = std::to_string(a_port);
		a_server.set_pre_routing_handler(
			[hosts = std::array{ "127.0.0.1:" + port, "localhost:" + port, "[::1]:" + port }](
				const httplib::Request& a_request, httplib::Response& a_response) {
				const auto refuse = [&](int a_status, const char* a_message) {
					a_response.status = a_status;
					a_response.set_content(json{ { "code", a_status }, { "error", a_message } }.dump(), "application/json");
					return httplib::Server::HandlerResponse::Handled;
				};

				// Host is case-insensitive; a rebound DNS name fails this match.
				auto host = a_request.get_header_value("Host");
				std::ranges::transform(host, host.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
				if (std::ranges::find(hosts, host) == hosts.end())
					return refuse(403, "DevBench only serves loopback Host headers");
				if (a_request.has_header("Origin"))
					return refuse(403, "DevBench refuses browser-originated requests");
				if (a_request.method == "POST")
				{
					// A text/plain POST skips CORS preflight, so JSON is required.
					auto type = a_request.get_header_value("Content-Type");
					type = type.substr(0, type.find(';'));
					std::ranges::transform(type, type.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
					type.erase(type.find_last_not_of(" 	") + 1);
					if (type != "application/json")
						return refuse(415, "POST requires Content-Type: application/json");
				}
				return httplib::Server::HandlerResponse::Unhandled;
			});
	}
}

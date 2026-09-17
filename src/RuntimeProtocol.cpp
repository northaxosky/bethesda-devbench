#include "RuntimeProtocol.h"

#include "Json.h"

#include <httplib.h>

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
}

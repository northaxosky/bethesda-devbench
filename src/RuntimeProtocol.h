#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace httplib
{
	class Server;
}

namespace dvb
{
	inline constexpr std::string_view kInstanceHeader = "X-DevBench-Instance";

	std::string MakeInstanceId(std::uint32_t a_pid, std::uint64_t a_creationTime);
	// Refuses browser-originated traffic (foreign Host, any Origin, non-JSON POST),
	// then requests addressed to another instance. Replaces any prior request guard.
	void MountRequestGuard(httplib::Server& a_server, std::string a_instanceId, int a_port);
}

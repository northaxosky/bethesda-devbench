// Engine text that is not UTF-8 made dump() throw, so the whole REST reply was an empty 500.

#include "test_framework.h"

#include "EventBus.h"
#include "RestAdapter.h"
#include "ToolRegistry.h"

#include <httplib.h>

#include <chrono>
#include <thread>

using dvb::json;

TEST_CASE("a tool result with invalid UTF-8 reaches REST as a 200 with U+FFFD")
{
	dvb::ToolRegistry registry;
	registry.Register({ "name", "engine text" }, [](const json&, const dvb::ToolContext&) {
		return json{ { "name", "Shrine of R\xE1n" } };
	});
	dvb::EventBus events;
	dvb::RestAdapter adapter(registry, events);
	httplib::Server  server;
	adapter.Mount(server);
	const int port = server.bind_to_any_port("127.0.0.1");
	CHECK(port > 0);
	std::thread worker([&] { server.listen_after_bind(); });
	const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	httplib::Client client("127.0.0.1", port);
	client.set_read_timeout(2);
	const auto reply = client.Post("/api/tool/name", "{}", "application/json");
	server.stop();
	worker.join();

	CHECK(reply && reply->status == 200);
	if (reply)
		CHECK(json::parse(reply->body)["name"] == "Shrine of R\xEF\xBF\xBD"
												  "n");
}

#include "test_framework.h"

#include "RuntimeProtocol.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
	class GuardedServer
	{
	public:
		explicit GuardedServer(std::string a_identity)
		{
			server_.new_task_queue = [] { return new httplib::ThreadPool(1); };
			dvb::MountInstanceGuard(server_, std::move(a_identity));
			auto handler = [this](const httplib::Request&, httplib::Response& a_response) {
				++calls;
				a_response.set_content("{}", "application/json");
			};
			server_.Post("/api/tool/mutate", handler);
			server_.Post("/mcp", handler);
			port = server_.bind_to_any_port("127.0.0.1");
			if (port < 1)
				throw std::runtime_error("could not bind the local instance-guard fixture");
			worker_ = std::thread([this] { server_.listen_after_bind(); });
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!server_.is_running() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (!server_.is_running())
			{
				server_.stop();
				worker_.join();
				throw std::runtime_error("instance-guard fixture did not start");
			}
		}

		~GuardedServer()
		{
			server_.stop();
			if (worker_.joinable())
				worker_.join();
		}

		int              port = 0;
		std::atomic<int> calls{ 0 };

	private:
		httplib::Server server_;
		std::thread     worker_;
	};
}

TEST_CASE("instance guards reject stale and ambiguous requests before tool dispatch")
{
	GuardedServer   server("123-0000000000000042");
	httplib::Client client("127.0.0.1", server.port);
	client.set_connection_timeout(2);
	client.set_read_timeout(2);
	const std::string header(dvb::kInstanceHeader);

	auto valid = client.Post("/api/tool/mutate", { { header, "123-0000000000000042" } }, "{}", "application/json");
	CHECK(valid && valid->status == 200);
	CHECK(server.calls == 1);

	auto stale = client.Post("/api/tool/mutate", { { header, "123-0000000000000041" } }, "{}", "application/json");
	CHECK(stale && stale->status == 409);
	CHECK(server.calls == 1);

	auto empty = client.Post("/api/tool/mutate", { { header, "" } }, "{}", "application/json");
	CHECK(empty && empty->status == 409);
	CHECK(server.calls == 1);

	auto duplicate = client.Post("/api/tool/mutate",
		{ { header, "123-0000000000000042" }, { header, "123-0000000000000041" } }, "{}", "application/json");
	CHECK(duplicate && duplicate->status == 409);
	CHECK(server.calls == 1);

	auto mcp = client.Post("/mcp", { { header, "123-0000000000000041" } }, "{}", "application/json");
	CHECK(mcp && mcp->status == 409);
	CHECK(server.calls == 1);

	auto legacy = client.Post("/api/tool/mutate", "{}", "application/json");
	CHECK(legacy && legacy->status == 200);
	CHECK(server.calls == 2);
}

TEST_CASE("reused PIDs cannot satisfy a previous process instance guard")
{
	const auto      oldIdentity = dvb::MakeInstanceId(42, 0x123456789ABCDEF0);
	const auto      newIdentity = dvb::MakeInstanceId(42, 0x123456789ABCDEF1);
	GuardedServer   server(newIdentity);
	httplib::Client client("127.0.0.1", server.port);
	client.set_connection_timeout(2);
	client.set_read_timeout(2);
	const std::string header(dvb::kInstanceHeader);
	auto              rejected = client.Post("/api/tool/mutate", { { header, oldIdentity } }, "{}", "application/json");
	CHECK(rejected && rejected->status == 409);
	CHECK(server.calls == 0);
	auto accepted = client.Post("/api/tool/mutate", { { header, newIdentity } }, "{}", "application/json");
	CHECK(accepted && accepted->status == 200);
	CHECK(server.calls == 1);
}

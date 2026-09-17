#include "Server.h"

#include "Compatibility.h"
#include "McpAdapter.h"
#include "RestAdapter.h"
#include "RuntimePaths.h"
#include "RuntimeProtocol.h"
#include "RuntimeContext.h"
#include "Version.h"
#include "io/WindowsPaths.h"

#include <httplib.h>
#include <mcp_server.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

namespace
{
	std::atomic<int> g_boundPort{ 0 };

	std::string Utf8Path(const std::filesystem::path& a_path)
	{
		const auto bytes = a_path.u8string();
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}

	struct ProcessIdentity
	{
		std::uint32_t         pid;
		std::string           instanceId;
		std::filesystem::path executable;
		std::filesystem::path plugin;
		std::string           runtime;
	};

	const ProcessIdentity& Identity()
	{
		static const auto identity = [] {
			FILETIME created{}, exited{}, kernel{}, user{};
			if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
				throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "GetProcessTimes");
			const auto pid = ::GetCurrentProcessId();
			const auto ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
			const auto& runtime = dvb::GetRuntimeContext();
			return ProcessIdentity{
				pid,
				dvb::MakeInstanceId(pid, ticks),
				runtime.executablePath,
				runtime.pluginPath,
				runtime.runtimeVersion,
			};
		}();
		return identity;
	}

	std::filesystem::path ExternalStateDirectory()
	{
		PWSTR      allocated = nullptr;
		const auto result = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &allocated);
		if (FAILED(result))
			throw std::runtime_error(std::format("LocalAppData lookup failed: 0x{:08X}", static_cast<std::uint32_t>(result)));
		const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> path(allocated, &::CoTaskMemFree);
		return std::filesystem::path(path.get()) /
		       dvb::CurrentGameProfile().externalStateDirectory;
	}

	void WriteDiscoveryFile(const std::filesystem::path& a_path, const dvb::json& a_value)
	{
		try
		{
			std::filesystem::create_directories(a_path.parent_path());
			const std::filesystem::path temporary = a_path.wstring() + L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
			{
				std::ofstream out;
				out.exceptions(std::ios::failbit | std::ios::badbit);
				out.open(temporary, std::ios::trunc);
				out << a_value.dump(2) << '\n';
				out.close();
			}
			if (!::MoveFileExW(temporary.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "replace discovery file");
		}
		catch (const std::system_error& a_error)
		{
			logs::error("devbench: could not publish {}: {}", Utf8Path(a_path), a_error.what());
		}
	}

	// Process-wide registry pointer so dvb::RunTool can invoke tools from an eventual in-game menu
	// without a Server reference. Atomic: written on the native lifecycle thread
	// (Start/Stop), read on the render thread (RunTool). Set/cleared by Server::Start/Stop.
	std::atomic<dvb::ToolRegistry*> g_registry{ nullptr };

	// Throttle for ListRecordingsCached: a menu redraws every frame, and listing fully parses every
	// recording file, so cache the result and refresh at most ~once/second (or on invalidation).
	std::mutex                            g_recCacheMtx;
	dvb::json                             g_recCache;
	std::chrono::steady_clock::time_point g_recCacheAt{};
	bool                                  g_recCacheValid = false;

	// True if 127.0.0.1:port can be bound (i.e. it's free). WSAStartup is ref-counted,
	// so pairing it with WSACleanup here is safe whether or not winsock is already up.
	bool PortAvailable(const std::string& a_host, int a_port)
	{
		WSADATA      wsa;
		const bool   started = ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
		bool         available = true;
		const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s != INVALID_SOCKET)
		{
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = ::htons(static_cast<u_short>(a_port));
			::inet_pton(AF_INET, a_host.c_str(), &addr.sin_addr);
			available = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
			::closesocket(s);
		}
		if (started)
			::WSACleanup();
		return available;
	}

	// Publish the actually-bound port so fixed-URL clients can discover a non-default
	// choice (when auto-iteration moved off the configured port).
	void WriteRuntimeInfo(int a_port)
	{
		auto identity = dvb::InstanceIdentity();
		identity["port"] = a_port;
		WriteDiscoveryFile(
			dvb::CurrentGameProfile().pluginDataDirectory / "runtime.json", identity);
		try
		{
			WriteDiscoveryFile(ExternalStateDirectory() / L"runtime.json", identity);
		}
		catch (const std::exception& a_error)
		{
			logs::error("devbench: external runtime discovery unavailable: {}", a_error.what());
		}
	}

	// Mirror of GET /api/tools's mcp_bridge block onto disk, for a caller who found devbench
	// via its files rather than a live REST call (e.g. reading the install directory directly).
	void WriteBridgeInfo()
	{
		WriteDiscoveryFile(
			dvb::CurrentGameProfile().pluginDataDirectory / "mcp-bridge.json",
			dvb::BridgeDiscoveryInfo());
	}
}

namespace dvb
{
	Server::Server(std::string a_host, int a_port) :
		m_host(std::move(a_host)), m_port(a_port)
	{}

	Server::~Server()
	{
		Stop();
	}

	bool Server::Start()
	{
		if (m_mcp)
			return true;

		// Find a free port starting at the configured one (a second instance or an
		// occupied port just moves to the next). The bound port is written to
		// runtime.json so fixed-URL clients can discover a non-default choice.
		constexpr int kMaxTries = 16;
		int           chosen = m_port;
		for (int i = 0; i < kMaxTries && m_port + i <= 65535; ++i)
		{
			if (PortAvailable(m_host, m_port + i))
			{
				chosen = m_port + i;
				break;
			}
		}

		// This cpp-mcp revision takes a configuration struct (host/port are no
		// longer positional ctor args).
		mcp::server::configuration cfg;
		cfg.host = m_host;
		cfg.port = chosen;
		cfg.name = "devbench";
		cfg.version = DEVBENCH_VERSION_STRING;

		m_mcp = std::make_unique<mcp::server>(cfg);
		m_mcp->set_server_info(cfg.name, cfg.version);
		// tools.listChanged: tools are added at runtime (cross-plugin consumers register kinds/menus),
		// so advertise the capability and emit notifications/tools/list_changed when the set changes —
		// a client that connected before a mod (or the game) finished loading then refreshes its list.
		m_mcp->set_capabilities(json{ { "tools", json{ { "listChanged", true } } }, { "logging", json::object() } });

		// MCP tools + notifications.
		m_mcpAdapter = std::make_unique<McpAdapter>(m_registry, m_events, *m_mcp);
		m_mcpAdapter->Wire();

		// REST facade on the same httplib server (constructed in mcp::server's ctor,
		// so http() is valid here; cpp-mcp adds its own routes during start()).
		m_restAdapter = std::make_unique<RestAdapter>(m_registry, m_events);
		if (auto* http = m_mcp->http())
		{
			MountInstanceGuard(*http, Identity().instanceId);
			// httplib's default 5s read timeout applies even to a bodyless POST (no
			// Content-Length/chunked header) — it waits for a body that will never come
			// before giving up with an empty 400. Shorten this so that mistake fails fast
			// instead of stalling; GET /api/health is the real fix for liveness checks.
			http->set_read_timeout(2, 0);
			m_restAdapter->Mount(*http);
		}
		else
			logs::warn("{}", "devbench: cpp-mcp http() returned null; REST facade unavailable");

		// Publish the chosen port before start() spawns the listener, so a health/inspect
		// hit racing startup reads the right port rather than 0.
		g_boundPort.store(chosen);
		g_registry.store(&m_registry);        // reachable by dvb::RunTool (the in-game menu) while up
		const bool ok = m_mcp->start(false);  // non-blocking; spawns the listener thread
		if (ok)
		{
			WriteRuntimeInfo(chosen);
			WriteBridgeInfo();
			if (chosen != m_port)
				logs::info("devbench: configured port {} busy → bound {}", m_port, chosen);
		}
		else
		{
			// Tear down the constructed-but-not-listening members: the `if (m_mcp)` guard at
			// the top treats a non-null m_mcp as "already started", so leaving them set would
			// make a later Start() return true without a live listener. Reset the port too, or
			// it would advertise a live bridge for a server that never came up.
			g_boundPort.store(0);
			g_registry.store(nullptr);
			m_restAdapter.reset();
			m_mcpAdapter.reset();
			m_mcp.reset();
		}
		logs::info("devbench: server on {}:{} — {}", m_host, chosen, ok ? "listening (mcp + rest)" : "FAILED to start");
		return ok;
	}

	void Server::Stop()
	{
		if (m_mcp)
		{
			m_mcp->stop();
			m_mcp.reset();
		}
		m_restAdapter.reset();
		m_mcpAdapter.reset();
		g_boundPort.store(0);
		g_registry.store(nullptr);
	}

	bool Server::Running() const
	{
		return m_mcp && m_mcp->is_running();
	}

	int BoundPort()
	{
		return g_boundPort.load();
	}

	void SetProcessRegistry(ToolRegistry* a_registry)
	{
		g_registry.store(a_registry);
	}

	json RunTool(const std::string& a_name, const json& a_args)
	{
		// Load once: a concurrent Stop() must not null the pointer between the check and the call.
		ToolRegistry* registry = g_registry.load();
		if (!registry)
			return json{ { "error", "devbench server not running" }, { "code", 503 } };
		ToolContext ctx{ "ui" };
		ctx.internal = true;  // menu-driven; don't log each call
		const ToolResult r = registry->Invoke(a_name, a_args, ctx);
		return r.ok ? r.value : json{ { "error", r.errorMessage }, { "code", r.errorCode } };
	}

	json ListRecordingsCached()
	{
		using namespace std::chrono;
		std::lock_guard lock(g_recCacheMtx);
		if (const auto now = steady_clock::now(); !g_recCacheValid || now - g_recCacheAt > seconds(1))
		{
			g_recCache = RunTool("recordings", json{ { "action", "list" } });
			g_recCacheAt = now;
			g_recCacheValid = true;
		}
		return g_recCache;
	}

	void InvalidateRecordingsCache()
	{
		std::lock_guard lock(g_recCacheMtx);
		g_recCacheValid = false;  // next ListRecordingsCached() re-parses
	}

	void OpenRecordingsFolder()
	{
		const std::string dir = ListRecordingsCached().value("dir", std::string{});
		if (dir.empty())
			return;
		std::error_code             ec;
		const std::filesystem::path abs = std::filesystem::absolute(dir, ec);
		::ShellExecuteW(nullptr, L"open", abs.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	void OpenRecordingFile(const std::string& a_file)
	{
		const std::string dir = ListRecordingsCached().value("dir", std::string{});
		if (dir.empty() || a_file.empty())
			return;
		std::error_code             ec;
		const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(dir) / a_file, ec);
		const std::wstring          args = L"/select,\"" + abs.wstring() + L"\"";
		::ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
	}

	std::string ExecutableName()
	{
		return Utf8Path(Identity().executable.filename());
	}

	json InstanceIdentity()
	{
		const auto& identity = Identity();
		const auto& profile = CurrentGameProfile();
		return json{
			{ "pid", identity.pid },
			{ "port", BoundPort() },
			{ "game", profile.id },
			{ "gameId", profile.id },
			{ "gameName", profile.displayName },
			{ "runtimeVariant", profile.runtimeVariant },
			{ "vr", profile.vr },
			{ "extender", profile.extenderName },
			{ "exe", ExecutableName() },
			{ "instanceId", identity.instanceId },
			{ "exePath", Utf8Path(identity.executable) },
			{ "runtime", identity.runtime },
			{ "dllPath", Utf8Path(identity.plugin) },
			{ "compatibility", CompatibilityMetadata() },
		};
	}

	json BridgeDiscoveryInfo()
	{
		const auto&       profile = CurrentGameProfile();
		const std::string game = profile.id;
		const auto        directory = Identity().plugin.parent_path() / L"devbench";
		const auto        executable = directory / L"devbench-bridge.exe";
		const auto        helper = directory / L"platform" / L"windows-session.ps1";
		const auto        shim = directory / L"platform" / L"LaunchShim.cs";
		const auto        launcher = directory / L"platform" / L"devbench-launch.exe";
		const std::string exePath = Utf8Path(executable);
		const std::string name = "devbench-" + game;
		std::error_code   ec;
		const bool        available = std::filesystem::is_regular_file(executable, ec);
		if (ec && ec != std::errc::no_such_file_or_directory)
			logs::error("devbench: could not inspect bridge executable: {}", ec.message());
		ec.clear();
		const bool controllerAvailable = available && std::filesystem::is_regular_file(helper, ec) &&
		                                 std::filesystem::is_regular_file(shim, ec) && std::filesystem::is_regular_file(launcher, ec);
		if (ec && ec != std::errc::no_such_file_or_directory)
			logs::error("devbench: could not inspect session helper: {}", ec.message());
		return json{
			{ "available", available },
			{ "controllerAvailable", controllerAvailable },
			{ "exePath", exePath },
			{ "args", json::array({ "--game", game }) },
			{ "mcpJsonSnippet",
				json{ { "mcpServers", json{ { name, json{ { "command", exePath }, { "args", json::array({ "--game", game }) } } } } } } },
			{ "installCommand", std::format("\"{}\" setup --game {}", exePath, game) },
			{ "compatibility", CompatibilityMetadata() },
			{ "note",
				"Add mcpJsonSnippet to your MCP client's config (e.g. .mcp.json), or run installCommand "
				"to print the same thing. Use a physical on-disk bridge path outside the MO2 virtual view. "
				"Session control also requires platform/windows-session.ps1 and a machine-local session config." },
		};
	}

	std::filesystem::path RuntimeGameDirectory()
	{
		return Identity().executable.parent_path();
	}

	std::filesystem::path RuntimePluginDirectory()
	{
		return Identity().plugin.parent_path();
	}
}

#include "HostApi.h"

#include "Compatibility.h"
#include "DevBenchAPI.h"
#include "EventBus.h"
#include "GameState.h"
#include "Json.h"
#include "Log.h"
#include "ToolExtensions.h"
#include "ToolRegistry.h"
#include "tools/PingTool.h"

#include <ctime>
#include <mutex>

namespace dvb::HostApi
{
	namespace
	{
		ToolRegistry* g_registry = nullptr;
		EventBus*     g_events = nullptr;

		// Registrant ledger: who asked for the C-ABI interface, and what they registered
		// through it. Both grow only (append-only, process lifetime) — a plugin unregistering
		// mid-session isn't a thing the C-ABI supports, so there's nothing to remove. One mutex
		// covers both since they're written from the same call sites and read together by
		// `inspect kind=registrants`.
		std::mutex                g_ledgerMutex;
		std::vector<Consumer>     g_consumers;
		std::vector<Registration> g_registrations;

		void NoteConsumer(const char* a_sender)
		{
			std::lock_guard<std::mutex> lock(g_ledgerMutex);
			g_consumers.push_back(Consumer{
				a_sender ? std::string(a_sender) : std::string("<?>"),
				static_cast<long long>(std::time(nullptr)),
				static_cast<std::uint32_t>(game::CurrentFrame()),
			});
		}

		void NoteRegistration(std::string a_kind, std::string a_name, bool a_replacedExisting)
		{
			std::lock_guard<std::mutex> lock(g_ledgerMutex);
			g_registrations.push_back(Registration{
				std::move(a_kind),
				std::move(a_name),
				static_cast<long long>(std::time(nullptr)),
				static_cast<std::uint32_t>(game::CurrentFrame()),
				a_replacedExisting,
			});
		}

		// Wrap a consumer's C callback (fn + ctx) as a ToolHandler: args in as a JSON string, result
		// collected via our host-owned sink. Nothing C++ crosses the DLL boundary — only const char*
		// and function pointers. Shared by RegisterTool and RegisterMenuHandler.
		ToolHandler MakeHandler(DevBenchAPI::ToolFn a_handler, void* a_ctx)
		{
			return [a_handler, a_ctx](const json& a_args, const ToolContext&) -> json {
				std::string          result;
				DevBenchAPI::WriteFn write = +[](void* a_sink, const char* a_json) {
					*static_cast<std::string*>(a_sink) = a_json ? a_json : "";
				};
				a_handler(a_ctx, a_args.dump().c_str(), &result, write);
				if (result.empty())
					return json::object();
				try
				{
					return json::parse(result);
				}
				catch (...)
				{
					return json{ { "raw", result } };
				}
			};
		}

		// Concrete implementation of the published C-ABI interface, wired to the
		// host's registry + bus.
		struct Interface : DevBenchAPI::IDevBenchInterface001
		{
			unsigned int GetBuildNumber() override
			{
				return kHostApiCompatibilityFloor;
			}

			bool RegisterTool(const char* a_name, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				if (!g_registry || !a_name || !a_handler)
					return false;

				json desc = json::object();
				if (a_descriptorJson)
				{
					try
					{
						desc = json::parse(a_descriptorJson);
					}
					catch (...)
					{
					}
				}
				ToolDescriptor d;
				d.name = a_name;
				d.description = desc.value("description", std::string{});
				d.inputSchema = desc.value("inputSchema", DefaultInputSchema());
				d.readOnly = desc.value("readOnly", false);

				const bool isNew = g_registry->Register(std::move(d), MakeHandler(a_handler, a_ctx));
				NoteRegistration("tool", a_name, !isNew);
				return isNew;
			}

			bool RegisterMenuHandler(const char* a_menuName, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				// Menus are just the first base tool that accepts extensions — delegate.
				return RegisterToolExtension("menu", a_menuName, a_descriptorJson, a_handler, a_ctx);
			}

			bool RegisterToolExtension(const char* a_baseTool, const char* a_key, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				if (!a_baseTool || !*a_baseTool || !a_key || !*a_key || !a_handler)
					return false;
				json desc = json::object();
				if (a_descriptorJson)
				{
					try
					{
						desc = json::parse(a_descriptorJson);
					}
					catch (...)
					{
					}
				}
				if (!desc.is_object())  // a scalar/array descriptor would break the descriptor object contract
					desc = json::object();
				const bool isNew = ToolExtensions::Register(a_baseTool, a_key, std::move(desc), MakeHandler(a_handler, a_ctx));
				NoteRegistration("extension", std::string(a_baseTool) + ":" + a_key, !isNew);
				return isNew;
			}

			void EmitEvent(const char* a_topic, const char* a_payloadJson) override
			{
				if (!g_events || !a_topic)
					return;
				json payload = json::object();
				if (a_payloadJson)
				{
					try
					{
						payload = json::parse(a_payloadJson);
					}
					catch (...)
					{
					}
				}
				g_events->Publish(a_topic, std::move(payload));
			}
		};

		Interface g_interface;

		void* GetApi(unsigned int a_revision)
		{
			// Only revision 1 exists; future revisions return a derived interface.
			return a_revision >= 1 ? static_cast<DevBenchAPI::IDevBenchInterface001*>(&g_interface) : nullptr;
		}

		// Self-test: register a trivial tool THROUGH the public interface, proving the
		// C-callback + JSON round-trip path end to end without a separate consumer.
		void RegisterSelfTest()
		{
			const auto descriptor = tools::BuildPingDescriptor();
			const json document{
				{ "description", descriptor.description },
				{ "inputSchema", descriptor.inputSchema },
				{ "readOnly", descriptor.readOnly },
			};
			const auto descriptorJson = document.dump();
			g_interface.RegisterTool(descriptor.name.c_str(), descriptorJson.c_str(), +[](void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write) {
					const std::string args = (a_argsJson && *a_argsJson) ? a_argsJson : "{}";
					const std::string out = R"({"pong":true,"echo":)" + args + "}";
					a_write(a_sink, out.c_str()); }, nullptr);
		}

	}

	void Init(ToolRegistry& a_registry, EventBus& a_events)
	{
		g_registry = &a_registry;
		g_events = &a_events;
		RegisterSelfTest();
	}

	void Reset()
	{
		g_registry = nullptr;
		g_events = nullptr;
	}

	void OnInterfaceRequest(const Message& a_message)
	{
		if (a_message.type == DevBenchAPI::DevBenchMessage::kMessage_GetInterface &&
			a_message.data)
		{
			const_cast<DevBenchAPI::DevBenchMessage*>(
				static_cast<const DevBenchAPI::DevBenchMessage*>(a_message.data))
				->GetApiFunction = GetApi;
			NoteConsumer(a_message.sender);
			logs::info(
				"devbench: provided plugin interface to {}",
				a_message.sender ? a_message.sender : "<?>");
		}
	}

	std::vector<Consumer> Consumers()
	{
		std::lock_guard<std::mutex> lock(g_ledgerMutex);
		return g_consumers;
	}

	std::vector<Registration> Registrations()
	{
		std::lock_guard<std::mutex> lock(g_ledgerMutex);
		return g_registrations;
	}
}

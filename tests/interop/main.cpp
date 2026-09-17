#include "DevBenchAPI.h"

#include <F4SE/F4SE.h>
#include <RE/I/IMessageBoxCallback.h>
#include <RE/M/MessageMenuManager.h>
#include <RE/W/WARNING_TYPES.h>
#include <REX/REX.h>

#include <nlohmann/json.hpp>

#include <Windows.h>
#undef ERROR

#include <atomic>
#include <cstdint>
#include <string>

namespace
{
	using json = nlohmann::json;

	constexpr auto kValidationKey = "devbench.validation";
	constexpr auto kDialogHeader = "DevBench validation";
	constexpr auto kDialogBody =
		"This benign runtime validation dialog makes no gameplay or save changes.";
	constexpr auto kDialogButton = "Close";

	enum class DialogError : std::uint8_t
	{
		kNone,
		kTaskInterfaceUnavailable,
		kMessageMenuManagerUnavailable,
		kCreateException,
		kCreateUnknownException,
		kQueueException,
		kQueueUnknownException,
	};

	struct RegistrationState
	{
		std::atomic<unsigned int> hostBuild{ 0 };
		std::atomic<bool>         attempted{ false };
		std::atomic<bool>         menu{ false };
		std::atomic<bool>         inspect{ false };
		std::atomic<bool>         captureAttempted{ false };
		std::atomic<bool>         capture{ false };
	};

	struct DialogState
	{
		std::atomic<std::uint64_t> requestCount{ 0 };
		std::atomic<std::uint64_t> callbackCount{ 0 };
		std::atomic<std::uint64_t> destroyedCount{ 0 };
		std::atomic<std::int32_t>  lastIndex{ -1 };
		std::atomic<bool>          pending{ false };
		std::atomic<bool>          queued{ false };
		std::atomic<bool>          created{ false };
		std::atomic<DialogError>   error{ DialogError::kNone };
	};

	RegistrationState g_registration;
	DialogState       g_dialog;
	std::atomic<bool> g_postPostLoadHandled{ false };

	const char* DialogErrorText(DialogError a_error) noexcept
	{
		switch (a_error)
		{
			case DialogError::kNone:
				return "";
			case DialogError::kTaskInterfaceUnavailable:
				return "F4SE TaskInterface is unavailable";
			case DialogError::kMessageMenuManagerUnavailable:
				return "MessageMenuManager is unavailable";
			case DialogError::kCreateException:
				return "MessageMenuManager::Create raised an exception";
			case DialogError::kCreateUnknownException:
				return "MessageMenuManager::Create raised an unknown exception";
			case DialogError::kQueueException:
				return "queuing the F4SE main-thread dialog task raised an exception";
			case DialogError::kQueueUnknownException:
				return "queuing the F4SE main-thread dialog task raised an unknown exception";
		}
		return "unknown dialog error";
	}

	json DialogJson()
	{
		const auto error = g_dialog.error.load(std::memory_order_acquire);
		return json{
			{ "requestCount", g_dialog.requestCount.load(std::memory_order_acquire) },
			{ "callbackCount", g_dialog.callbackCount.load(std::memory_order_acquire) },
			{ "destroyedCount", g_dialog.destroyedCount.load(std::memory_order_acquire) },
			{ "lastIndex", g_dialog.lastIndex.load(std::memory_order_acquire) },
			{ "pending", g_dialog.pending.load(std::memory_order_acquire) },
			{ "queued", g_dialog.queued.load(std::memory_order_acquire) },
			{ "created", g_dialog.created.load(std::memory_order_acquire) },
			{ "error", DialogErrorText(error) },
			{ "expectedHeader", kDialogHeader },
			{ "expectedBody", kDialogBody },
			{ "expectedButton", kDialogButton },
			{ "expectedIndex", 0 },
		};
	}

	class CountingMessageBoxCallback final : public RE::IMessageBoxCallback
	{
	public:
		~CountingMessageBoxCallback() override
		{
			g_dialog.destroyedCount.fetch_add(1, std::memory_order_acq_rel);
			g_dialog.pending.store(false, std::memory_order_release);
		}

		void operator()(std::uint8_t a_buttonIdx) override
		{
			g_dialog.lastIndex.store(a_buttonIdx, std::memory_order_release);
			g_dialog.callbackCount.fetch_add(1, std::memory_order_acq_rel);
			g_dialog.pending.store(false, std::memory_order_release);
		}
	};
	static_assert(
		sizeof(CountingMessageBoxCallback) == sizeof(RE::IMessageBoxCallback));

	json ParseArguments(const char* a_argsJson)
	{
		if (!a_argsJson || !*a_argsJson)
			return json::object();
		try
		{
			return json::parse(a_argsJson);
		}
		catch (const json::exception&)
		{
			return json{ { "invalidJson", true }, { "raw", a_argsJson } };
		}
	}

	json RegistrationJson()
	{
		return json{
			{ "attempted", g_registration.attempted.load(std::memory_order_acquire) },
			{ "menu", g_registration.menu.load(std::memory_order_acquire) },
			{ "inspect", g_registration.inspect.load(std::memory_order_acquire) },
			{ "captureAttempted",
				g_registration.captureAttempted.load(std::memory_order_acquire) },
			{ "capture", g_registration.capture.load(std::memory_order_acquire) },
		};
	}

	void WriteResult(void* a_sink, DevBenchAPI::WriteFn a_write, json a_result)
	{
		if (!a_write)
			return;
		const auto encoded = a_result.dump();
		a_write(a_sink, encoded.c_str());
	}

	void ValidationHandler(
		void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		auto* api = g_devBenchInterface;
		if (!api)
		{
			WriteResult(a_sink, a_write,
				json{
					{ "ok", false },
					{ "error", "devbench C-ABI interface is no longer available" },
					{ "argsEcho", ParseArguments(a_argsJson) },
				});
			return;
		}

		DWORD foregroundProcessId = 0;
		if (const auto foregroundWindow = ::GetForegroundWindow())
			::GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);

		WriteResult(a_sink, a_write,
			json{
				{ "ok", true },
				{ "fixture", "devbench-interop" },
				{ "key", kValidationKey },
				// Deliberately call through the acquired virtual interface on every
				// invocation. This is the value crossing the real DLL boundary, not a
				// fixture-side metadata constant.
				{ "hostBuild", api->GetBuildNumber() },
				{ "foregroundProcessId", foregroundProcessId },
				{ "currentProcessId", ::GetCurrentProcessId() },
				{ "registrations", RegistrationJson() },
				{ "dialog", DialogJson() },
				{ "argsEcho", ParseArguments(a_argsJson) },
			});
	}

	void MenuValidationHandler(
		void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		auto* api = g_devBenchInterface;
		auto  args = ParseArguments(a_argsJson);
		if (!api)
		{
			WriteResult(a_sink, a_write,
				json{
					{ "ok", false },
					{ "error", "devbench C-ABI interface is no longer available" },
					{ "dialog", DialogJson() },
					{ "argsEcho", std::move(args) },
				});
			return;
		}

		const auto command = args.find("command");
		if (command == args.end() || !command->is_string() ||
			command->get_ref<const std::string&>() != "showDialog")
		{
			WriteResult(a_sink, a_write,
				json{
					{ "ok", false },
					{ "fixture", "devbench-interop" },
					{ "key", kValidationKey },
					{ "hostBuild", api->GetBuildNumber() },
					{ "registrations", RegistrationJson() },
					{ "dialog", DialogJson() },
					{ "argsEcho", std::move(args) },
					{ "error", "menu command must be exactly 'showDialog'" },
				});
			return;
		}

		const auto tasks = F4SE::GetTaskInterface();
		if (!tasks)
		{
			g_dialog.error.store(
				DialogError::kTaskInterfaceUnavailable, std::memory_order_release);
			REX::ERROR(
				"devbench-interop: showDialog rejected because F4SE TaskInterface is unavailable");
			WriteResult(a_sink, a_write,
				json{
					{ "ok", false },
					{ "fixture", "devbench-interop" },
					{ "key", kValidationKey },
					{ "hostBuild", api->GetBuildNumber() },
					{ "registrations", RegistrationJson() },
					{ "dialog", DialogJson() },
					{ "argsEcho", std::move(args) },
					{ "error", DialogErrorText(DialogError::kTaskInterfaceUnavailable) },
				});
			return;
		}

		bool expected = false;
		if (!g_dialog.pending.compare_exchange_strong(
				expected, true, std::memory_order_acq_rel))
		{
			WriteResult(a_sink, a_write,
				json{
					{ "ok", false },
					{ "fixture", "devbench-interop" },
					{ "key", kValidationKey },
					{ "hostBuild", api->GetBuildNumber() },
					{ "registrations", RegistrationJson() },
					{ "dialog", DialogJson() },
					{ "argsEcho", std::move(args) },
					{ "error", "a validation dialog is already pending" },
				});
			return;
		}

		g_dialog.requestCount.fetch_add(1, std::memory_order_acq_rel);
		g_dialog.lastIndex.store(-1, std::memory_order_release);
		g_dialog.created.store(false, std::memory_order_release);
		g_dialog.error.store(DialogError::kNone, std::memory_order_release);
		g_dialog.queued.store(true, std::memory_order_release);

		bool accepted = false;
		try
		{
			tasks->AddTask([] {
				g_dialog.queued.store(false, std::memory_order_release);
				try
				{
					auto* manager = RE::MessageMenuManager::GetSingleton();
					if (!manager)
					{
						g_dialog.error.store(
							DialogError::kMessageMenuManagerUnavailable,
							std::memory_order_release);
						g_dialog.pending.store(false, std::memory_order_release);
						REX::ERROR(
							"devbench-interop: MessageMenuManager is unavailable on the main thread");
						return;
					}

					RE::BSTSmartPointer<CountingMessageBoxCallback> callback{
						new CountingMessageBoxCallback()
					};
					manager->Create(
						kDialogHeader, kDialogBody, callback.get(),
						RE::WARNING_TYPES::kDefault, kDialogButton, nullptr, nullptr,
						nullptr, true);
					g_dialog.created.store(true, std::memory_order_release);
					REX::INFO(
						"devbench-interop: native validation dialog Create returned successfully");
				}
				catch (const std::exception& a_exception)
				{
					g_dialog.error.store(
						DialogError::kCreateException, std::memory_order_release);
					g_dialog.pending.store(false, std::memory_order_release);
					REX::ERROR(
						"devbench-interop: native validation dialog Create failed: {}",
						a_exception.what());
				}
				catch (...)
				{
					g_dialog.error.store(
						DialogError::kCreateUnknownException, std::memory_order_release);
					g_dialog.pending.store(false, std::memory_order_release);
					REX::ERROR(
						"devbench-interop: native validation dialog Create failed");
				}
			});
			accepted = true;
		}
		catch (const std::exception& a_exception)
		{
			g_dialog.queued.store(false, std::memory_order_release);
			g_dialog.error.store(DialogError::kQueueException, std::memory_order_release);
			g_dialog.pending.store(false, std::memory_order_release);
			REX::ERROR(
				"devbench-interop: queuing native validation dialog failed: {}",
				a_exception.what());
		}
		catch (...)
		{
			g_dialog.queued.store(false, std::memory_order_release);
			g_dialog.error.store(
				DialogError::kQueueUnknownException, std::memory_order_release);
			g_dialog.pending.store(false, std::memory_order_release);
			REX::ERROR("devbench-interop: queuing native validation dialog failed");
		}

		WriteResult(a_sink, a_write,
			json{
				{ "ok", accepted },
				{ "fixture", "devbench-interop" },
				{ "key", kValidationKey },
				{ "hostBuild", api->GetBuildNumber() },
				{ "registrations", RegistrationJson() },
				{ "dialog", DialogJson() },
				{ "argsEcho", std::move(args) },
				{ "error",
					accepted ? "" :
							   DialogErrorText(
								   g_dialog.error.load(std::memory_order_acquire)) },
			});
	}

#if defined(DEVBENCH_INTEROP_ENABLE_CAPTURE)
	void CaptureValidationHandler(
		void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		auto* api = g_devBenchInterface;
		auto  args = ParseArguments(a_argsJson);
		bool  emitted = false;
		if (api && args.is_object())
		{
			const auto request = args.find("requestId");
			if (request != args.end() && request->is_string() &&
				!request->get_ref<const std::string&>().empty())
			{
				const json event{
					{ "requestId", *request },
					{ "ok", false },
					{ "provider", kValidationKey },
					{ "validationOnly", true },
					{ "error",
						"devbench-interop validation-only provider intentionally failed before "
						"artifact creation; it is not a renderer" },
				};
				const auto encoded = event.dump();
				api->EmitEvent("capture.ready", encoded.c_str());
				emitted = true;
			}
		}

		WriteResult(a_sink, a_write,
			json{
				{ "ok", false },
				{ "fixture", "devbench-interop" },
				{ "key", kValidationKey },
				{ "hostBuild", api ? api->GetBuildNumber() : 0U },
				{ "registrations", RegistrationJson() },
				{ "argsEcho", std::move(args) },
				{ "captureReadyEmitted", emitted },
				{ "validationOnly", true },
				{ "error",
					emitted ?
						"validation-only capture failure emitted; no image or artifact was created" :
						"capture validation request did not contain a non-empty requestId" },
			});
	}
#endif

	void RegisterValidationExtensions() noexcept
	{
		if (g_postPostLoadHandled.exchange(true, std::memory_order_acq_rel))
			return;

		try
		{
			auto* api = DevBenchAPI::GetDevBenchInterface001();
			if (!api)
			{
				REX::ERROR(
					"devbench-interop: '{}' host did not provide interface revision 1",
					DevBenchAPI::DevBenchPluginName);
				return;
			}

			const auto build = api->GetBuildNumber();
			g_registration.hostBuild.store(build, std::memory_order_release);
			if (build < DevBenchAPI::kMenuHandlerCompatibility)
			{
				REX::ERROR(
					"devbench-interop: host build {} is below menu compatibility gate {}; "
					"nothing registered",
					build, DevBenchAPI::kMenuHandlerCompatibility);
				return;
			}
			if (build < DevBenchAPI::kToolExtensionCompatibility)
			{
				REX::ERROR(
					"devbench-interop: host build {} is below extension compatibility gate {}; "
					"nothing registered",
					build, DevBenchAPI::kToolExtensionCompatibility);
				return;
			}

			g_registration.attempted.store(true, std::memory_order_release);
			constexpr auto menuDescriptor = R"json({
				"description":"C-ABI validation menu extension. command 'showDialog' queues one benign native MessageMenuManager dialog that makes no gameplay or save changes.",
				"inputSchema":{
					"type":"object",
					"properties":{
						"action":{"type":"string","const":"invoke"},
						"name":{"type":"string","const":"devbench.validation"},
						"command":{"type":"string","enum":["showDialog"]}
					},
					"required":["action","name","command"],
					"additionalProperties":false
				},
				"validation":{
					"header":"DevBench validation",
					"body":"This benign runtime validation dialog makes no gameplay or save changes.",
					"button":"Close",
					"expectedCallbackIndex":0,
					"expectedCallbackCount":1,
					"expectedDestroyedCount":1
				},
				"validationOnly":true,
				"readOnly":false
			})json";
			constexpr auto inspectDescriptor = R"json({
				"description":"C-ABI validation-only inspect extension. Returns the live host build, registration outcomes, argument echo, and read-only native dialog callback state.",
				"validationOnly":true,
				"readOnly":true
			})json";

			g_registration.menu.store(
				api->RegisterMenuHandler(
					kValidationKey, menuDescriptor, &MenuValidationHandler, nullptr),
				std::memory_order_release);
			g_registration.inspect.store(
				api->RegisterToolExtension(
					"inspect", kValidationKey, inspectDescriptor, &ValidationHandler, nullptr),
				std::memory_order_release);

#if defined(DEVBENCH_INTEROP_ENABLE_CAPTURE)
			constexpr auto captureDescriptor = R"json({
				"description":"Validation-only C-ABI capture extension. It intentionally emits an immediate matching capture.ready failure and never renders, writes, copies, or advertises an image.",
				"validationOnly":true,
				"renderer":false,
				"producesArtifacts":false
			})json";
			g_registration.captureAttempted.store(true, std::memory_order_release);
			g_registration.capture.store(
				api->RegisterToolExtension("capture", kValidationKey, captureDescriptor,
					&CaptureValidationHandler, nullptr),
				std::memory_order_release);
#endif

			REX::INFO(
				"devbench-interop: host build {}; registrations menu={} inspect={} capture={}",
				build, g_registration.menu.load(std::memory_order_acquire),
				g_registration.inspect.load(std::memory_order_acquire),
				g_registration.capture.load(std::memory_order_acquire));
		}
		catch (const std::exception& a_exception)
		{
			REX::ERROR(
				"devbench-interop: C-ABI validation registration failed: {}", a_exception.what());
		}
		catch (...)
		{
			REX::ERROR("devbench-interop: C-ABI validation registration failed");
		}
	}

	void F4SEAPI OnF4SEMessage(F4SE::MessagingInterface::Message* a_message) noexcept
	{
		if (a_message && a_message->type == F4SE::MessagingInterface::kPostPostLoad)
			RegisterValidationExtensions();
	}

	bool InitPlugin(const F4SE::LoadInterface* a_f4se)
	{
		F4SE::InitInfo init{};
		init.logName = "devbench-interop";
		init.trampoline = false;
		init.hook = false;
		F4SE::Init(a_f4se, init);

		const auto messaging = F4SE::GetMessagingInterface();
		if (!messaging || !messaging->RegisterListener(&OnF4SEMessage))
		{
			REX::ERROR(
				"devbench-interop: F4SE lifecycle listener registration failed");
			return false;
		}
		REX::INFO(
			"devbench-interop loaded; waiting for kPostPostLoad C-ABI validation");
		return true;
	}
}

F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	if (const auto data = F4SE::PluginVersionData::GetSingleton())
	{
		a_info->infoVersion = F4SE::PluginInfo::kVersion;
		a_info->name = data->GetPluginName().data();
		a_info->version = data->GetPluginVersion().pack();
	}
	return !a_f4se->IsEditor();
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	try
	{
		return InitPlugin(a_f4se);
	}
	catch (const std::exception& a_exception)
	{
		REX::ERROR("devbench-interop: plugin load failed: {}", a_exception.what());
		return false;
	}
	catch (...)
	{
		REX::ERROR("devbench-interop: plugin load failed");
		return false;
	}
}

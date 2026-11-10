#include "ConsoleBackend.h"

#include "MainThread.h"
#include "tools/BoundedTextLog.h"

#include <array>
#include <atomic>
#include <cstring>
#include <optional>

namespace dvb::fallout4
{
	namespace
	{
		bool IsCaptureFence(std::string_view a_command)
		{
			return a_command.starts_with("DVBCAPBEGIN") ||
			       a_command.starts_with("DVBCAPEND");
		}

		// The console menu empties the engine buffer, which also caps at 64 KB.
		constexpr std::size_t kRecordedBytes = 1 << 20;

		struct CallSite
		{
			REL::ID                        function;  // contains the call
			std::array<std::ptrdiff_t, 3>  offset;    // OG, NG, AE; 0 = no site
			REL::ID                        callee;
			bool                           chainable;  // may wrap a foreign hook
		};

		const REL::ID kAddString{ 764, 2248593 };
		const REL::ID kPrint{ 799546, 2248592 };
		const REL::ID kPrintLine{ 1065300, 2248591 };
		const REL::ID kExecuteCommand{ 1061864, 2248537 };

		// NG and AE share ids and layout. OG's PrintLine only forwards to Print.
		const CallSite kPrintToAddString{ kPrint, { 0x5F, 0x9E, 0x9E }, kAddString, true };
		const CallSite kPrintLineToAddString{ kPrintLine, { 0, 0xA0, 0xA0 }, kAddString, true };
		const CallSite kEchoToPrintLine{ kExecuteCommand, { 0x76, 0x89, 0x89 }, kPrintLine, false };

		enum class SiteState
		{
			kExpected,
			kForeignHook,
			kChanged,
		};

		// A call into a stub outside the game's code is another plugin's hook.
		SiteState InspectCallSite(std::uintptr_t a_site, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			if (bytes[0] != 0xE8)
				return SiteState::kChanged;
			std::int32_t displacement;
			std::memcpy(&displacement, bytes + 1, sizeof(displacement));
			const auto destination = a_site + 5 + displacement;
			if (destination == a_target)
				return SiteState::kExpected;
			const auto text = REX::FModule::GetExecutingModule().GetSection(".text");
			const bool inGame = destination >= text.GetAddress() &&
			                    destination < text.GetAddress() + text.GetSize();
			return inGame ? SiteState::kChanged : SiteState::kForeignHook;
		}

		template <class>
		class CallHook;

		// Patches a call site that calls the expected function or a hook on it.
		template <class R, class... T>
		class CallHook<R(T...)>
		{
		public:
			CallHook() = default;
			CallHook(const CallHook&) = delete;
			CallHook& operator=(const CallHook&) = delete;

			~CallHook() { Disable(); }

			void Disable()
			{
				if (m_hook && m_hook->GetEnabled())
					m_hook->Disable();
			}

			bool Install(const char* a_name, const CallSite& a_site, R (*a_function)(T...))
			{
				const auto offset = a_site.offset[static_cast<std::size_t>(REX::FModule::GetRuntimeIndex())];
				if (!offset)
					return false;
				const auto address = a_site.function.address() + offset;
				switch (InspectCallSite(address, a_site.callee.address()))
				{
				case SiteState::kChanged:
					REX::WARN("devbench: {}: call site bytes differ, not patched", a_name);
					return false;
				case SiteState::kForeignHook:
					if (!a_site.chainable)
					{
						REX::WARN("devbench: {}: another plugin's hook, not patched", a_name);
						return false;
					}
					REX::INFO("devbench: {}: chaining onto another plugin's hook", a_name);
					break;
				case SiteState::kExpected:
					break;
				}
				m_hook.emplace(a_name, a_site.function, offset, a_function);
				if (!m_hook->Init() || !m_hook->Enable())
				{
					m_hook.reset();
					return false;
				}
				return true;
			}

			R operator()(T... a_args) const { return (*m_hook)(a_args...); }

		private:
			std::optional<REL::THook<R(T...)>> m_hook;
		};

		// Records console prints and keeps typed lines out of printf.
		class ConsolePrintHooks
		{
		public:
			ConsolePrintHooks()
			{
				s_active.store(this);
				m_recording = m_print.Install("devbench console print", kPrintToAddString, &OnAddString<&ConsolePrintHooks::m_print>);
				if (kPrintLineToAddString.offset[static_cast<std::size_t>(REX::FModule::GetRuntimeIndex())])
					m_recording = m_printLine.Install("devbench console printline", kPrintLineToAddString, &OnAddString<&ConsolePrintHooks::m_printLine>) && m_recording;
				m_echoLiteral = m_echo.Install("devbench console echo", kEchoToPrintLine, &OnEcho);
				if (!m_recording)
					REX::WARN("{}", "devbench: console print hooks unavailable; reading the engine buffer");
				if (!m_echoLiteral)
					REX::WARN("{}", "devbench: console echo not patched; commands containing '%' are refused");
			}

			~ConsolePrintHooks()
			{
				m_echo.Disable();
				m_printLine.Disable();
				m_print.Disable();
				s_active.store(nullptr);
			}

			ConsolePrintHooks(const ConsolePrintHooks&) = delete;
			ConsolePrintHooks& operator=(const ConsolePrintHooks&) = delete;

			bool Recording() const { return m_recording; }

			bool EchoIsLiteral() const { return m_echoLiteral; }

			std::string Recorded() const { return m_log.Snapshot(); }

		private:
			using AddStringFn = std::int64_t(RE::ConsoleLog*, const char*);
			// PrintLine(log, fmt, ...): the hooked call site passes only the line.
			using EchoFn = std::int32_t(RE::ConsoleLog*, const char*, const char*);

			// Each site forwards to its own original, which may be another hook.
			template <CallHook<AddStringFn> ConsolePrintHooks::*Site>
			static std::int64_t OnAddString(RE::ConsoleLog* a_log, const char* a_text)
			{
				auto* active = s_active.load();
				if (a_text)
					active->m_log.Append(a_text);
				return (active->*Site)(a_log, a_text);
			}

			static std::int32_t OnEcho(RE::ConsoleLog* a_log, const char* a_line, const char*)
			{
				return s_active.load()->m_echo(a_log, "%s", a_line);
			}

			tools::BoundedTextLog m_log{ kRecordedBytes };
			CallHook<AddStringFn> m_print;
			CallHook<AddStringFn> m_printLine;
			CallHook<EchoFn>      m_echo;
			bool                  m_recording = false;
			bool                  m_echoLiteral = false;

			static inline std::atomic<ConsolePrintHooks*> s_active{ nullptr };
		};

		tools::ConsoleBackend BuildBackend(
			EventBus* a_events,
			const ConsolePrintHooks* a_print = nullptr,
			std::shared_ptr<void> a_lifetime = {})
		{
			return tools::ConsoleBackend{
				.queueCommands =
					[a_events, lifetime = std::move(a_lifetime)](
						std::vector<std::string> a_commands,
						std::function<void()> a_completed) {
						(void)lifetime;
						const auto tasks = F4SE::GetTaskInterface();
						if (!tasks)
							return false;
						try
						{
							tasks->AddTask(
								[events = a_events,
								 commands = std::move(a_commands),
								 completed = std::move(a_completed)]() mutable {
									struct Complete
									{
										std::function<void()>& callback;
										~Complete()
										{
											if (callback)
												callback();
										}
									} complete{ completed };

									for (const auto& command : commands)
									{
										RE::Console::ExecuteCommand(command.c_str());
										if (events && !IsCaptureFence(command))
											events->Publish(
												"console.command",
												json{
													{ "command", command },
													{ "source", "broker" },
												});
									}
								});
							return true;
						}
						catch (const std::exception& a_exception)
						{
							REX::ERROR(
								"devbench: could not enqueue console command: {}",
								a_exception.what());
							return false;
						}
					},
				.readBuffer = [a_print] {
					if (a_print && a_print->Recording())
						return a_print->Recorded();
					const auto result =
						MainThread::RunAndWait([]() -> json {
							const auto consoleLog =
								RE::ConsoleLog::GetSingleton();
							if (!consoleLog)
								throw ToolError(
									503,
									"native console log is unavailable");
							const auto raw = consoleLog->buffer.c_str();
							return raw ?
							           json(std::string(raw)) :
							           json(std::string{});
						});
					return result.get<std::string>();
				},
				.validateCommand = [a_print](const std::string& a_command) {
					// The unpatched echo feeds the typed line to printf as a format.
					if (a_print && !a_print->EchoIsLiteral() &&
						a_command.find('%') != std::string::npos)
						throw ToolError(
							400,
							"commands containing '%' are refused: the console echo is not patched on this runtime");
				},
			};
		}
	}

	class ConsoleBackendLifetime::Impl
	{
	public:
		explicit Impl(EventBus& a_events) :
			m_events(a_events),
			m_hook(
				"devbench console observation",
				RE::VTABLE::Console[0],
				1,
				&OnConsoleCall)
		{
			Impl* expected = nullptr;
			if (!s_active.compare_exchange_strong(expected, this))
				throw ToolError(
					409, "console observation is already installed");
			if (!m_hook.Enable())
			{
				s_active.store(nullptr);
				throw ToolError(
					503, "could not install the console observation hook");
			}
			// Built only once this is the sole instance, so s_active cannot dangle.
			m_print.emplace();
		}

		~Impl()
		{
			// THookVFT restores the original vtable entry. Its return value is
			// meaningful, unlike THook::Disable on this CommonLib revision.
			m_hook.Disable();
			m_print.reset();
			Impl* expected = this;
			s_active.compare_exchange_strong(expected, nullptr);
		}

		void PublishTyped(std::string a_command)
		{
			if (!a_command.empty() && !IsCaptureFence(a_command))
				m_events.Publish(
					"console.command",
					json{
						{ "command", std::move(a_command) },
						{ "source", "typed" },
					});
		}

		EventBus& Events() const
		{
			return m_events;
		}

		const ConsolePrintHooks& Print() const
		{
			return *m_print;
		}

	private:
		using Params = Scaleform::GFx::FunctionHandler::Params;
		using Hook = REL::THookVFT<void(RE::Console*, const Params&)>;

		static void OnConsoleCall(
			RE::Console* a_console,
			const Params& a_params)
		{
			auto* active = s_active.load();
			std::string command;
			if (active &&
				reinterpret_cast<std::uintptr_t>(a_params.userData) == 0 &&
				a_params.argCount == 1 && a_params.args &&
				a_params.args[0].IsString())
			{
				if (const auto text = a_params.args[0].GetString(); text)
					command = text;
			}

			if (active)
				active->m_hook(a_console, a_params);
			if (active && !command.empty())
				active->PublishTyped(std::move(command));
		}

		EventBus& m_events;
		Hook      m_hook;
		std::optional<ConsolePrintHooks> m_print;

		static inline std::atomic<Impl*> s_active{ nullptr };
	};

	ConsoleBackendLifetime::ConsoleBackendLifetime(EventBus& a_events) :
		m_impl(std::make_shared<Impl>(a_events))
	{}

	ConsoleBackendLifetime::~ConsoleBackendLifetime() = default;

	tools::ConsoleBackend ConsoleBackendLifetime::Backend() const
	{
		return BuildBackend(
			&m_impl->Events(), &m_impl->Print(), std::static_pointer_cast<void>(m_impl));
	}

	tools::ConsoleBackend MakeConsoleBackend()
	{
		return BuildBackend(nullptr);
	}
}

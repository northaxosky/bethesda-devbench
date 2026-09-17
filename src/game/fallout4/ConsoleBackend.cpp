#include "ConsoleBackend.h"

#include "MainThread.h"

#include <atomic>

namespace dvb::fallout4
{
	namespace
	{
		bool IsCaptureFence(std::string_view a_command)
		{
			return a_command.starts_with("DVBCAPBEGIN") ||
			       a_command.starts_with("DVBCAPEND");
		}

		tools::ConsoleBackend BuildBackend(
			EventBus* a_events,
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
				.readBuffer = [] {
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
		}

		~Impl()
		{
			// THookVFT restores the original vtable entry. Its return value is
			// meaningful, unlike THook::Disable on this CommonLib revision.
			m_hook.Disable();
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

		static inline std::atomic<Impl*> s_active{ nullptr };
	};

	ConsoleBackendLifetime::ConsoleBackendLifetime(EventBus& a_events) :
		m_impl(std::make_shared<Impl>(a_events))
	{}

	ConsoleBackendLifetime::~ConsoleBackendLifetime() = default;

	tools::ConsoleBackend ConsoleBackendLifetime::Backend() const
	{
		return BuildBackend(
			&m_impl->Events(), std::static_pointer_cast<void>(m_impl));
	}

	tools::ConsoleBackend MakeConsoleBackend()
	{
		return BuildBackend(nullptr);
	}
}

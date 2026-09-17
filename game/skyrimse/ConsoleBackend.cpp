#include "ConsoleBackend.h"

#include "MainThread.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <ranges>

namespace dvb::skyrimse
{
	namespace
	{
		constexpr std::size_t kMaxPendingBrokerCommands = 256;

		bool IsCaptureFence(std::string_view a_command)
		{
			return a_command.starts_with("DVBCAPBEGIN") ||
			       a_command.starts_with("DVBCAPEND");
		}

		tools::ConsoleBackend BuildBackend(
			EventBus* a_events,
			std::shared_ptr<ConsoleBackendLifetime::Impl> a_impl = {});
	}

	class ConsoleBackendLifetime::Impl
	{
	public:
		explicit Impl(EventBus& a_events) :
			events_(a_events)
		{
			const std::lock_guard lock{ s_installMutex };
			if (s_active.load(std::memory_order_acquire))
				throw ToolError(409, "console observation is already installed");
			s_active.store(this, std::memory_order_release);
			if (s_hookInstalled)
				return;

			REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50157, 51084) };
			if (target.address() == 0 ||
				!SKSE::stl::install_context_hook(
					target.address(), 8, &OnConsoleCall, 8))
			{
				s_active.store(nullptr, std::memory_order_release);
				throw ToolError(503, "could not install the Skyrim console observation hook");
			}
			s_hookInstalled = true;
		}

		~Impl()
		{
			Impl* expected = this;
			s_active.compare_exchange_strong(
				expected, nullptr, std::memory_order_acq_rel);
		}

		EventBus& Events() const
		{
			return events_;
		}

		void MarkBroker(std::string_view a_command)
		{
			if (IsCaptureFence(a_command))
				return;
			const std::lock_guard lock{ pendingMutex_ };
			if (pendingBroker_.size() == kMaxPendingBrokerCommands)
				pendingBroker_.pop_front();
			pendingBroker_.emplace_back(a_command);
		}

		void PublishBroker(std::string a_command)
		{
			if (!IsCaptureFence(a_command))
				events_.Publish(
					"console.command",
					json{
						{ "command", std::move(a_command) },
						{ "source", "broker" },
					});
		}

	private:
		bool ConsumeBroker(std::string_view a_command)
		{
			const std::lock_guard lock{ pendingMutex_ };
			const auto found = std::ranges::find(pendingBroker_, a_command);
			if (found == pendingBroker_.end())
				return false;
			pendingBroker_.erase(found);
			return true;
		}

		void PublishObserved(std::string a_command)
		{
			if (a_command.empty() || IsCaptureFence(a_command) ||
				ConsumeBroker(a_command))
				return;
			events_.Publish(
				"console.command",
				json{
					{ "command", std::move(a_command) },
					{ "source", "typed" },
				});
		}

		static void OnConsoleCall(CONTEXT& a_context)
		{
			try
			{
				auto* active = s_active.load(std::memory_order_acquire);
				if (!active)
					return;
				const auto* args =
					reinterpret_cast<const RE::FxDelegateArgs*>(a_context.Rcx);
				if (!args || args->GetArgCount() < 1)
					return;
				const auto& value = (*args)[0];
				if (!value.IsString())
					return;
				if (const auto command = value.GetString(); command && *command)
					active->PublishObserved(command);
			}
			catch (...)
			{}
		}

		EventBus&              events_;
		std::mutex             pendingMutex_;
		std::deque<std::string> pendingBroker_;

		static inline std::mutex        s_installMutex;
		static inline std::atomic<Impl*> s_active{ nullptr };
		static inline bool              s_hookInstalled = false;
	};

	namespace
	{
		tools::ConsoleBackend BuildBackend(
			EventBus* a_events,
			std::shared_ptr<ConsoleBackendLifetime::Impl> a_impl)
		{
			return tools::ConsoleBackend{
				.queueCommands =
					[a_events, lifetime = std::move(a_impl)](
						std::vector<std::string> a_commands,
						std::function<void()> a_completed) {
						const auto tasks = SKSE::GetTaskInterface();
						if (!tasks)
							return false;
						try
						{
							tasks->AddTask(
								[events = a_events,
								 lifetime,
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
										if (lifetime)
											lifetime->MarkBroker(command);
										RE::Console::ExecuteCommand(command.c_str());
										if (lifetime)
											lifetime->PublishBroker(command);
										else if (events && !IsCaptureFence(command))
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
							SKSE::log::error(
								"devbench: could not enqueue Skyrim console command: {}",
								a_exception.what());
							return false;
						}
					},
				.readBuffer = [] {
					const auto result =
						MainThread::RunAndWait([]() -> json {
							const auto consoleLog = RE::ConsoleLog::GetSingleton();
							if (!consoleLog)
								throw ToolError(
									503, "native console log is unavailable");
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

	ConsoleBackendLifetime::ConsoleBackendLifetime(EventBus& a_events) :
		impl_(std::make_shared<Impl>(a_events))
	{}

	ConsoleBackendLifetime::~ConsoleBackendLifetime() = default;

	tools::ConsoleBackend ConsoleBackendLifetime::Backend() const
	{
		return BuildBackend(&impl_->Events(), impl_);
	}

	tools::ConsoleBackend MakeConsoleBackend()
	{
		return BuildBackend(nullptr);
	}
}

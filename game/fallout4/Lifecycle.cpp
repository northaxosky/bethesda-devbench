#include "Lifecycle.h"

#include "EventBus.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace dvb::fallout4::Lifecycle
{
	namespace
	{
		struct State
		{
			std::mutex                      mutex;
			EventBus*                       events = nullptr;
			bool                            gameDataReady = false;
			bool                            gameLoaded = false;
			bool                            loadInProgress = false;
			bool                            inMainMenu = false;
			bool                            inLoadingMenu = false;
			bool                            menuTrackingReady = false;
			std::optional<bool>             postLoadSucceeded;
			std::string                     lastEvent;
			std::unordered_set<std::string> openMenus;
		};

		State                       g_state;
		tools::GameOperationTracker g_operations;

		void LogMenuError(const char* a_detail = nullptr) noexcept
		{
			try
			{
				if (a_detail)
					REX::ERROR("devbench: lifecycle menu event failed: {}", a_detail);
				else
					REX::ERROR("devbench: lifecycle menu event failed");
			}
			catch (...)
			{}
		}

		std::uint64_t Publish(std::string_view a_event, json a_data = json::object())
		{
			EventBus* events = nullptr;
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.lastEvent.assign(a_event);
				events = g_state.events;
			}
			a_data["event"] = a_event;
			if (events)
			{
				events->Publish("lifecycle", std::move(a_data));
				return events->HeadSeq();
			}
			return 0;
		}

		std::optional<std::string> DecodeMessageName(std::uint32_t a_dataLen, const void* a_data)
		{
			constexpr std::size_t kMaxMessageNameBytes = 0x104;
			if (!a_data || reinterpret_cast<std::uintptr_t>(a_data) < 0x10000 ||
				a_dataLen == 0 || a_dataLen > kMaxMessageNameBytes)
				return std::nullopt;

			const auto bytes = std::string_view(
				static_cast<const char*>(a_data), static_cast<std::size_t>(a_dataLen));
			const auto terminator = bytes.find('\0');
			const auto name = bytes.substr(0, terminator);
			if (name.empty())
				return std::nullopt;
			if (std::ranges::any_of(name, [](unsigned char a_ch) {
					return a_ch < 0x20 || a_ch >= 0x7F;
				}))
				return std::nullopt;
			return std::string(name);
		}

		json NamePayload(const std::optional<std::string>& a_name)
		{
			if (!a_name)
				return json::object();
			return json{ { "name", *a_name } };
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				try
				{
					const std::string name = a_event.menuName.c_str() ? a_event.menuName.c_str() : "";
					EventBus*         events = nullptr;
					{
						const std::lock_guard lock{ g_state.mutex };
						if (a_event.opening)
							g_state.openMenus.insert(name);
						else
							g_state.openMenus.erase(name);
						if (name == "MainMenu")
							g_state.inMainMenu = a_event.opening;
						if (name == "LoadingMenu")
						{
							g_state.inLoadingMenu = a_event.opening;
							g_state.loadInProgress = a_event.opening;
						}
						events = g_state.events;
					}

					if (events)
						events->Publish("menu", json{ { "name", name }, { "opening", a_event.opening } });
					if (name == "MainMenu")
						Publish(a_event.opening ? "mainMenuEntered" : "mainMenuExited");
					else if (name == "LoadingMenu")
						Publish(a_event.opening ? "loadingMenuOpened" : "loadingMenuClosed");
				}
				catch (const std::exception& a_exception)
				{
					LogMenuError(a_exception.what());
				}
				catch (...)
				{
					LogMenuError();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		MenuSink g_menuSink;

		void InstallMenuSink()
		{
			{
				const std::lock_guard lock{ g_state.mutex };
				if (g_state.menuTrackingReady)
					return;
			}

			const auto ui = RE::UI::GetSingleton();
			if (!ui)
			{
				REX::WARN("devbench: UI singleton unavailable; lifecycle menu tracking deferred");
				return;
			}
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuSink);
			const bool            inMainMenu = ui->GetMenuOpen<RE::MainMenu>();
			const bool            inLoadingMenu = ui->GetMenuOpen<RE::LoadingMenu>();
			const std::lock_guard lock{ g_state.mutex };
			g_state.inMainMenu = inMainMenu;
			g_state.inLoadingMenu = inLoadingMenu;
			if (inMainMenu)
				g_state.openMenus.insert("MainMenu");
			if (inLoadingMenu)
				g_state.openMenus.insert("LoadingMenu");
			g_state.menuTrackingReady = true;
		}
	}

	void Initialize(EventBus& a_events)
	{
		const std::lock_guard lock{ g_state.mutex };
		g_state.events = &a_events;
	}

	void Reset()
	{
		{
			const std::lock_guard lock{ g_state.mutex };
			g_state.events = nullptr;
		}
		g_operations.Reset();
	}

	void OnF4SEMessage(std::uint32_t a_type, std::uint32_t a_dataLen, const void* a_data)
	{
		switch (a_type)
		{
			case F4SE::MessagingInterface::kPostLoad:
				Publish("postLoad");
				break;
			case F4SE::MessagingInterface::kPostPostLoad:
				Publish("postPostLoad");
				break;
			case F4SE::MessagingInterface::kPreLoadGame:
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.loadInProgress = true;
				g_state.postLoadSucceeded.reset();
			}
				InstallMenuSink();
				{
					const auto name = DecodeMessageName(a_dataLen, a_data);
					const auto cursor = Publish("preLoadGame", NamePayload(name));
					g_operations.ObserveStarted(tools::GameOperationKind::kLoad, name, cursor);
				}
				break;
			case F4SE::MessagingInterface::kPostLoadGame:
			{
				// F4SE encodes success in the pointer value and identifies the payload
				// with dataLen == 1; do not treat every post-load notification as success.
				const bool success = a_dataLen == 1 && a_data != nullptr;
				{
					const std::lock_guard lock{ g_state.mutex };
					g_state.postLoadSucceeded = success;
					// The loading menu remains a separate readiness gate after the save operation finishes.
					g_state.loadInProgress = false;
				}
				InstallMenuSink();
				const auto cursor = Publish("postLoadGame", json{ { "success", success } });
				g_operations.ObserveCompleted(
					tools::GameOperationKind::kLoad, std::nullopt, success, cursor);
			}
			break;
			case F4SE::MessagingInterface::kPreSaveGame:
			{
				const auto name = DecodeMessageName(a_dataLen, a_data);
				const auto cursor = Publish("preSaveGame", NamePayload(name));
				g_operations.ObserveStarted(tools::GameOperationKind::kSave, name, cursor);
			}
			break;
			case F4SE::MessagingInterface::kPostSaveGame:
			{
				const auto name = DecodeMessageName(a_dataLen, a_data);
				const auto cursor = Publish("postSaveGame", NamePayload(name));
				g_operations.ObserveCompleted(
					tools::GameOperationKind::kSave, name, std::nullopt, cursor);
			}
			break;
			case F4SE::MessagingInterface::kDeleteGame:
				Publish("deleteGame");
				break;
			case F4SE::MessagingInterface::kInputLoaded:
				Publish("inputLoaded");
				break;
			case F4SE::MessagingInterface::kNewGame:
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.loadInProgress = true;
				g_state.postLoadSucceeded.reset();
			}
				InstallMenuSink();
				Publish("newGame");
				break;
			case F4SE::MessagingInterface::kGameLoaded:
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.gameLoaded = true;
			}
				InstallMenuSink();
				Publish("gameLoaded");
				break;
			case F4SE::MessagingInterface::kGameDataReady:
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.gameDataReady = true;
			}
				InstallMenuSink();
				Publish("gameDataReady");
				break;
			default:
				break;
		}
	}

	Snapshot GetSnapshot()
	{
		EventBus* events = nullptr;
		Snapshot  snapshot;
		{
			const std::lock_guard lock{ g_state.mutex };
			snapshot.gameDataReady = g_state.gameDataReady;
			snapshot.gameLoaded = g_state.gameLoaded;
			snapshot.loadInProgress = g_state.loadInProgress;
			snapshot.inMainMenu = g_state.inMainMenu;
			snapshot.inLoadingMenu = g_state.inLoadingMenu;
			snapshot.menuTrackingReady = g_state.menuTrackingReady;
			snapshot.postLoadSucceeded = g_state.postLoadSucceeded;
			snapshot.lastEvent = g_state.lastEvent;
			snapshot.openMenus.assign(g_state.openMenus.begin(), g_state.openMenus.end());
			events = g_state.events;
		}
		snapshot.eventCursor = events ? events->HeadSeq() : 0;
		snapshot.operation = g_operations.GetSnapshot();
		std::ranges::sort(snapshot.openMenus);
		return snapshot;
	}

	std::optional<tools::GameOperationSnapshot> BeginOperation(
		tools::GameOperationKind a_kind, std::string a_name)
	{
		EventBus* events = nullptr;
		{
			const std::lock_guard lock{ g_state.mutex };
			events = g_state.events;
		}
		return g_operations.TryBegin(a_kind, std::move(a_name), events ? events->HeadSeq() : 0);
	}

	bool MarkOperationDispatching(std::uint64_t a_operationId)
	{
		return g_operations.MarkDispatching(a_operationId);
	}

	bool MarkOperationQueued(std::uint64_t a_operationId)
	{
		return g_operations.MarkQueued(a_operationId);
	}

	bool MarkOperationDispatchFailed(std::uint64_t a_operationId, std::string a_error)
	{
		return g_operations.MarkDispatchFailed(a_operationId, std::move(a_error));
	}

	bool CancelOperation(std::uint64_t a_operationId, std::string a_error)
	{
		return g_operations.MarkCancelled(a_operationId, std::move(a_error));
	}

	bool CompleteOperation(std::uint64_t a_operationId, json a_result)
	{
		return g_operations.MarkCompleted(a_operationId, std::move(a_result));
	}

	bool CompleteOperation(
		std::uint64_t a_operationId, bool a_success, json a_result, std::string a_error)
	{
		return g_operations.MarkCompleted(
			a_operationId, a_success, std::move(a_result), std::move(a_error));
	}
}

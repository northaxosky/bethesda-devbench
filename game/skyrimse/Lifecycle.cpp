#include "Lifecycle.h"

#include "EventBus.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_set>

namespace dvb::skyrimse::Lifecycle
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
			bool                            cellTrackingReady = false;
			std::optional<bool>             postLoadSucceeded;
			std::string                     lastEvent;
			std::unordered_set<std::string> openMenus;
		};

		State                       g_state;
		tools::GameOperationTracker g_operations;
		std::atomic<bool>           g_menuSinkInstalled{ false };
		std::atomic<bool>           g_cellSinkInstalled{ false };
		std::atomic<std::uint32_t>  g_lastPlayerCell{ 0 };

		void LogSinkError(std::string_view a_sink, const char* a_detail = nullptr) noexcept
		{
			try
			{
				if (a_detail)
					SKSE::log::error("devbench: Skyrim {} event failed: {}", a_sink, a_detail);
				else
					SKSE::log::error("devbench: Skyrim {} event failed", a_sink);
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
			if (!events)
				return 0;
			events->Publish("lifecycle", std::move(a_data));
			return events->HeadSeq();
		}

		std::optional<std::string> DecodeMessageName(const void* a_data)
		{
			constexpr std::size_t kMaxMessageNameBytes = 255;
			if (!a_data || reinterpret_cast<std::uintptr_t>(a_data) < 0x10000)
				return std::nullopt;

			const auto* raw = static_cast<const char*>(a_data);
			std::size_t length = 0;
			while (length < kMaxMessageNameBytes && raw[length] != '\0')
				++length;
			if (length == 0)
				return std::nullopt;
			return std::string(raw, length);
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
				const RE::MenuOpenCloseEvent* a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event)
					return RE::BSEventNotifyControl::kContinue;
				try
				{
					const std::string name =
						a_event->menuName.c_str() ? a_event->menuName.c_str() : "";
					EventBus* events = nullptr;
					{
						const std::lock_guard lock{ g_state.mutex };
						if (a_event->opening)
							g_state.openMenus.insert(name);
						else
							g_state.openMenus.erase(name);
						if (name == RE::MainMenu::MENU_NAME)
							g_state.inMainMenu = a_event->opening;
						if (name == RE::LoadingMenu::MENU_NAME)
						{
							g_state.inLoadingMenu = a_event->opening;
							if (a_event->opening)
								g_state.loadInProgress = true;
						}
						events = g_state.events;
					}

					if (events)
						events->Publish(
							"menu", json{ { "name", name }, { "opening", a_event->opening } });
					if (name == RE::MainMenu::MENU_NAME)
						Publish(a_event->opening ? "mainMenuEntered" : "mainMenuExited");
					else if (name == RE::LoadingMenu::MENU_NAME)
						Publish(a_event->opening ? "loadingMenuOpened" : "loadingMenuClosed");
				}
				catch (const std::exception& a_exception)
				{
					LogSinkError("menu", a_exception.what());
				}
				catch (...)
				{
					LogSinkError("menu");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class CellSink : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESCellFullyLoadedEvent*,
				RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
			{
				try
				{
					auto* player = RE::PlayerCharacter::GetSingleton();
					auto* cell = player ? player->GetParentCell() : nullptr;
					if (!cell)
						return RE::BSEventNotifyControl::kContinue;
					const auto formId = cell->GetFormID();
					if (g_lastPlayerCell.exchange(formId) == formId)
						return RE::BSEventNotifyControl::kContinue;

					EventBus* events = nullptr;
					{
						const std::lock_guard lock{ g_state.mutex };
						events = g_state.events;
					}
					if (events)
					{
						const auto editorId = cell->GetFormEditorID();
						events->Publish(
							"scene.cellLoaded",
							json{
								{ "cell", editorId && *editorId ? json(editorId) : json(nullptr) },
								{ "formId", formId },
								{ "formIdHex", std::format("0x{:08X}", formId) },
								{ "interior", cell->IsInteriorCell() },
							});
					}
				}
				catch (const std::exception& a_exception)
				{
					LogSinkError("cell", a_exception.what());
				}
				catch (...)
				{
					LogSinkError("cell");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		MenuSink g_menuSink;
		CellSink g_cellSink;

		void InstallEventSinks()
		{
			if (!g_menuSinkInstalled.load(std::memory_order_acquire))
			{
				if (auto* ui = RE::UI::GetSingleton())
				{
					bool expected = false;
					if (g_menuSinkInstalled.compare_exchange_strong(expected, true))
						ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);

					const bool inMainMenu = ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
					const bool inLoadingMenu = ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
					const std::lock_guard lock{ g_state.mutex };
					g_state.inMainMenu = inMainMenu;
					g_state.inLoadingMenu = inLoadingMenu;
					if (inMainMenu)
						g_state.openMenus.insert(std::string(RE::MainMenu::MENU_NAME));
					if (inLoadingMenu)
						g_state.openMenus.insert(std::string(RE::LoadingMenu::MENU_NAME));
					g_state.menuTrackingReady = true;
				}
			}
			else
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.menuTrackingReady = true;
			}

			if (!g_cellSinkInstalled.load(std::memory_order_acquire))
			{
				if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton())
				{
					bool expected = false;
					if (g_cellSinkInstalled.compare_exchange_strong(expected, true))
						holder->AddEventSink<RE::TESCellFullyLoadedEvent>(&g_cellSink);
					const std::lock_guard lock{ g_state.mutex };
					g_state.cellTrackingReady = true;
				}
			}
			else
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.cellTrackingReady = true;
			}
		}
	}

	void Initialize(EventBus& a_events)
	{
		{
			const std::lock_guard lock{ g_state.mutex };
			g_state.events = &a_events;
		}
		InstallEventSinks();
	}

	void Reset()
	{
		{
			const std::lock_guard lock{ g_state.mutex };
			g_state.events = nullptr;
			g_state.gameDataReady = false;
			g_state.gameLoaded = false;
			g_state.loadInProgress = false;
			g_state.inMainMenu = false;
			g_state.inLoadingMenu = false;
			g_state.menuTrackingReady = g_menuSinkInstalled.load(std::memory_order_acquire);
			g_state.cellTrackingReady = g_cellSinkInstalled.load(std::memory_order_acquire);
			g_state.postLoadSucceeded.reset();
			g_state.lastEvent.clear();
			g_state.openMenus.clear();
		}
		g_lastPlayerCell.store(0);
		g_operations.Reset();
	}

	void OnSKSEMessage(std::uint32_t a_type, std::uint32_t a_dataLen, const void* a_data)
	{
		(void)a_dataLen;
		switch (a_type)
		{
			case SKSE::MessagingInterface::kPostLoad:
				Publish("postLoad");
				break;
			case SKSE::MessagingInterface::kPostPostLoad:
				Publish("postPostLoad");
				break;
			case SKSE::MessagingInterface::kPreLoadGame:
			{
				{
					const std::lock_guard lock{ g_state.mutex };
					g_state.loadInProgress = true;
					g_state.gameLoaded = false;
					g_state.postLoadSucceeded.reset();
				}
				InstallEventSinks();
				const auto name = DecodeMessageName(a_data);
				const auto cursor = Publish("preLoadGame", NamePayload(name));
				g_operations.ObserveStarted(tools::GameOperationKind::kLoad, name, cursor);
				break;
			}
			case SKSE::MessagingInterface::kPostLoadGame:
			{
				// SKSE encodes load success in the pointer value. dataLen is not a
				// dependable discriminator for xSE lifecycle payloads.
				const bool success = a_data != nullptr;
				{
					const std::lock_guard lock{ g_state.mutex };
					g_state.loadInProgress = false;
					g_state.gameLoaded = success;
					g_state.postLoadSucceeded = success;
				}
				InstallEventSinks();
				const auto cursor = Publish("postLoadGame", json{ { "success", success } });
				g_operations.ObserveCompleted(
					tools::GameOperationKind::kLoad, std::nullopt, success, cursor);
				break;
			}
			case SKSE::MessagingInterface::kSaveGame:
			{
				const auto name = DecodeMessageName(a_data);
				const auto cursor = Publish("saveGame", NamePayload(name));
				// SKSE's save notification has no success result. Complete correlation
				// without manufacturing a successful outcome.
				g_operations.ObserveCompleted(
					tools::GameOperationKind::kSave, name, std::nullopt, cursor);
				break;
			}
			case SKSE::MessagingInterface::kDeleteGame:
				Publish("deleteGame", NamePayload(DecodeMessageName(a_data)));
				break;
			case SKSE::MessagingInterface::kInputLoaded:
				Publish("inputLoaded");
				break;
			case SKSE::MessagingInterface::kNewGame:
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.gameLoaded = true;
				g_state.loadInProgress = false;
				g_state.postLoadSucceeded = true;
			}
				InstallEventSinks();
				Publish("newGame");
				break;
			case SKSE::MessagingInterface::kDataLoaded:
			{
				const std::lock_guard lock{ g_state.mutex };
				g_state.gameDataReady = true;
			}
				InstallEventSinks();
				Publish("dataLoaded");
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
			snapshot.cellTrackingReady = g_state.cellTrackingReady;
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

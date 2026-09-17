#include "MenuBackend.h"

#include "MainThread.h"
#include "game/skyrimse/Lifecycle.h"
#include "tools/camera/MutationDispatch.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace dvb::skyrimse
{
	namespace
	{
		std::optional<tools::MenuDialogSnapshot> ReadLiveDialog()
		{
			const auto data = RE::MessageBoxMenu::GetCurrentMessageBoxData();
			if (!data)
				return std::nullopt;

			tools::MenuDialogFingerprint fingerprint{
				.headerText = {},
				.bodyText = data->bodyText.c_str() ? data->bodyText.c_str() : "",
			};
			fingerprint.buttons.reserve(data->buttonText.size());
			for (const auto& button : data->buttonText)
				fingerprint.buttons.emplace_back(button.c_str() ? button.c_str() : "");
			return tools::MenuDialogSnapshot{
				.fingerprint = std::move(fingerprint),
				.cancelIndex = data->cancelButtonIndex >= 0 ?
				                   std::optional<std::size_t>(
									   static_cast<std::size_t>(data->cancelButtonIndex)) :
				                   std::nullopt,
			};
		}

		json SnapshotJson(const tools::MenuDialogSnapshot& a_snapshot)
		{
			return json{
				{ "headerText", a_snapshot.fingerprint.headerText },
				{ "bodyText", a_snapshot.fingerprint.bodyText },
				{ "buttons", a_snapshot.fingerprint.buttons },
				{ "cancelIndex",
					a_snapshot.cancelIndex ? json(*a_snapshot.cancelIndex) : json(nullptr) },
			};
		}

		tools::MenuDialogSnapshot SnapshotFromJson(const json& a_value)
		{
			tools::MenuDialogSnapshot out{
				.fingerprint = {
					.headerText = a_value.at("headerText").get<std::string>(),
					.bodyText = a_value.at("bodyText").get<std::string>(),
					.buttons = a_value.at("buttons").get<std::vector<std::string>>(),
				},
			};
			if (!a_value.at("cancelIndex").is_null())
				out.cancelIndex = a_value.at("cancelIndex").get<std::size_t>();
			return out;
		}

		tools::CancelableMutationRunner::Submit MainThreadSubmitter()
		{
			return [](std::function<void()> a_task) {
				const auto tasks = SKSE::GetTaskInterface();
				if (!tasks)
					return false;
				try
				{
					tasks->AddTask(std::move(a_task));
					return true;
				}
				catch (const std::exception& a_exception)
				{
					logs::error(
						"devbench: could not enqueue Skyrim menu mutation: {}",
						a_exception.what());
					return false;
				}
			};
		}

		std::optional<tools::MenuDialogSnapshot> DescribeDialog()
		{
			const auto value = MainThread::RunAndWait([]() -> json {
				const auto dialog = ReadLiveDialog();
				return dialog ? SnapshotJson(*dialog) : json(nullptr);
			});
			return value.is_null() ?
			           std::nullopt :
			           std::optional<tools::MenuDialogSnapshot>(SnapshotFromJson(value));
		}

		json AcceptDialog(
			tools::CancelableMutationRunner& a_runner, tools::MenuAcceptRequest a_request)
		{
			return a_runner.Run("menu accept",
				[request = std::move(a_request)]() -> json {
					const auto dialog = ReadLiveDialog();
					if (!dialog)
						throw ToolError(409, "the observed MessageBoxMenu is no longer active");
					if (dialog->fingerprint != request.fingerprint)
						throw ToolError(
							409, "the active MessageBoxMenu changed before acceptance");
					if (request.index >= dialog->fingerprint.buttons.size())
						throw ToolError(
							409, "the active MessageBoxMenu button list changed before acceptance");
					RE::MessageBoxMenu::SelectOption(static_cast<std::int32_t>(request.index));
					return json{
						{ "queued", false },
						{ "completed", true },
						{ "accepted", true },
						{ "index", request.index },
					};
				});
		}

		json QueueMenuMessage(
			tools::CancelableMutationRunner& a_runner, std::string a_name,
			RE::UI_MESSAGE_TYPE a_type, std::string_view a_action)
		{
			return a_runner.Run(std::format("menu {}", a_action),
				[name = std::move(a_name), a_type, action = std::string(a_action)]() -> json {
					const auto queue = RE::UIMessageQueue::GetSingleton();
					if (!queue)
						throw ToolError(503, "UIMessageQueue is unavailable");
					queue->AddMessage(RE::BSFixedString(name), a_type, nullptr);
					return json{
						{ "queued", true },
						{ "acknowledged", true },
						{ "action", action },
						{ "name", name },
					};
				});
		}
	}

	tools::MenuBackend MakeMenuBackend(std::string)
	{
		auto runner =
			std::make_shared<tools::CancelableMutationRunner>(MainThreadSubmitter());
		return tools::MenuBackend{
			.gameName = "Skyrim Special Edition",
			.contextFreeOpenMenus = { std::string(RE::JournalMenu::MENU_NAME) },
			.dialogLimitations =
				"headerText is empty because Skyrim MessageBoxData exposes body text only",
			.listOpenMenus = [] { return Lifecycle::GetSnapshot().openMenus; },
			.describeDialog = &DescribeDialog,
			.acceptDialog = [runner](tools::MenuAcceptRequest a_request) { return AcceptDialog(*runner, std::move(a_request)); },
			.openMenu = [runner](std::string a_name) { return QueueMenuMessage(
														   *runner, std::move(a_name), RE::UI_MESSAGE_TYPE::kShow, "open"); },
			.closeMenu = [runner](std::string a_name) { return QueueMenuMessage(
															*runner, std::move(a_name), RE::UI_MESSAGE_TYPE::kHide, "close"); },
		};
	}
}

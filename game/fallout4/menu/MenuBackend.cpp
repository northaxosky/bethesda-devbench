#include "MenuBackend.h"

#include "MainThread.h"
#include "game/fallout4/Lifecycle.h"
#include "tools/camera/MutationDispatch.h"

#include <cstring>

namespace dvb::fallout4
{
	namespace
	{
		constexpr std::string_view kSupportedRuntime = "1.11.240.0";
		constexpr std::size_t      kAeCurrentMessageOffset = 0xF0;

		struct LiveDialog
		{
			Scaleform::Ptr<RE::MessageBoxMenu> menu;
			tools::MenuDialogSnapshot          snapshot;
		};

		void RequireSupportedRuntime(std::string_view a_runtimeVersion)
		{
			if (a_runtimeVersion != kSupportedRuntime)
				throw ToolError(503,
					std::format(
						"menu controls require Fallout 4 {}, current runtime is {}",
						kSupportedRuntime, a_runtimeVersion));
		}

		RE::MessageBoxData* CurrentMessageAE(RE::MessageBoxMenu* a_menu)
		{
			RE::MessageBoxData* message = nullptr;
			// AE 1.11.240 moved the live pointer to +0xF0; CommonLib's named +0xE8
			// member is wrong for this exact, server-gated runtime.
			std::memcpy(
				std::addressof(message),
				reinterpret_cast<const std::byte*>(a_menu) + kAeCurrentMessageOffset,
				sizeof(message));
			return message;
		}

		std::optional<LiveDialog> ReadLiveDialog()
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui)
				throw ToolError(503, "UI singleton is unavailable");
			auto menu = ui->GetMenu<RE::MessageBoxMenu>();
			if (!menu || !menu->OnStack())
				return std::nullopt;

			const auto message = CurrentMessageAE(menu.get());
			if (!message)
				return std::nullopt;

			tools::MenuDialogFingerprint fingerprint{
				.headerText =
					message->headerText.c_str() ? message->headerText.c_str() : "",
				.bodyText =
					message->bodyText.c_str() ? message->bodyText.c_str() : "",
			};
			fingerprint.buttons.reserve(message->buttonText.size());
			for (const auto& button : message->buttonText)
				fingerprint.buttons.emplace_back(button.c_str() ? button.c_str() : "");

			return LiveDialog{
				.menu = std::move(menu),
				.snapshot = {
					.fingerprint = std::move(fingerprint),
					.cancelIndex = std::nullopt,
				},
			};
		}

		json SnapshotJson(const tools::MenuDialogSnapshot& a_snapshot)
		{
			return json{
				{ "headerText", a_snapshot.fingerprint.headerText },
				{ "bodyText", a_snapshot.fingerprint.bodyText },
				{ "buttons", a_snapshot.fingerprint.buttons },
			};
		}

		tools::MenuDialogSnapshot SnapshotFromJson(const json& a_value)
		{
			return {
				.fingerprint = {
					.headerText = a_value.at("headerText").get<std::string>(),
					.bodyText = a_value.at("bodyText").get<std::string>(),
					.buttons = a_value.at("buttons").get<std::vector<std::string>>(),
				},
				.cancelIndex = std::nullopt,
			};
		}

		tools::CancelableMutationRunner::Submit MainThreadSubmitter()
		{
			return [](std::function<void()> a_task) {
				const auto tasks = F4SE::GetTaskInterface();
				if (!tasks)
					return false;
				try
				{
					tasks->AddTask(std::move(a_task));
					return true;
				}
				catch (const std::exception& a_exception)
				{
					REX::ERROR(
						"devbench: could not enqueue menu mutation: {}", a_exception.what());
					return false;
				}
			};
		}

		std::optional<tools::MenuDialogSnapshot> DescribeDialog(
			std::string_view a_runtimeVersion)
		{
			RequireSupportedRuntime(a_runtimeVersion);
			const auto result = MainThread::RunAndWait([]() -> json {
				const auto dialog = ReadLiveDialog();
				return dialog ? SnapshotJson(dialog->snapshot) : json(nullptr);
			});
			if (result.is_null())
				return std::nullopt;
			return SnapshotFromJson(result);
		}

		json AcceptDialog(
			std::string_view                 a_runtimeVersion,
			tools::CancelableMutationRunner& a_runner,
			tools::MenuAcceptRequest         a_request)
		{
			RequireSupportedRuntime(a_runtimeVersion);
			return a_runner.Run("menu accept",
				[request = std::move(a_request)]() -> json {
					const auto dialog = ReadLiveDialog();
					if (!dialog)
						throw ToolError(409, "the observed MessageBoxMenu is no longer active");
					if (dialog->snapshot.fingerprint != request.fingerprint)
						throw ToolError(
							409, "the active MessageBoxMenu changed before acceptance");
					if (request.index >= dialog->snapshot.fingerprint.buttons.size())
						throw ToolError(
							409, "the active MessageBoxMenu button list changed before acceptance");

					Scaleform::GFx::Value argument{
						static_cast<std::uint32_t>(request.index)
					};
					Scaleform::GFx::FunctionHandler::Params params{};
					params.args = std::addressof(argument);
					params.argCount = 1;
					params.userData = nullptr;  // MessageBoxMenu method index 0: OnButtonPress.
					static_cast<RE::IMenu*>(dialog->menu.get())->Call(params);
					return json{
						{ "queued", false },
						{ "completed", true },
						{ "accepted", true },
						{ "index", request.index },
					};
				});
		}

		json QueueMenuMessage(
			tools::CancelableMutationRunner& a_runner,
			std::string                      a_name,
			RE::UI_MESSAGE_TYPE              a_type,
			std::string_view                 a_action)
		{
			return a_runner.Run(std::format("menu {}", a_action),
				[name = std::move(a_name), a_type, action = std::string(a_action)]() -> json {
					const auto queue = RE::UIMessageQueue::GetSingleton();
					if (!queue)
						throw ToolError(503, "UIMessageQueue is unavailable");
					queue->AddMessage(RE::BSFixedString(name), a_type);
					return json{
						{ "queued", true },
						{ "acknowledged", true },
						{ "action", action },
						{ "name", name },
					};
				});
		}
	}

	tools::MenuBackend MakeMenuBackend(std::string a_runtimeVersion)
	{
		auto runner =
			std::make_shared<tools::CancelableMutationRunner>(MainThreadSubmitter());
		return tools::MenuBackend{
			.listOpenMenus = [] { return Lifecycle::GetSnapshot().openMenus; },
			.describeDialog = [runtime = a_runtimeVersion] { return DescribeDialog(runtime); },
			.acceptDialog =
				[runtime = a_runtimeVersion, runner](tools::MenuAcceptRequest a_request) {
					return AcceptDialog(runtime, *runner, std::move(a_request));
				},
			.openMenu = [runtime = a_runtimeVersion, runner](std::string a_name) {
				RequireSupportedRuntime(runtime);
				return QueueMenuMessage(
					*runner, std::move(a_name), RE::UI_MESSAGE_TYPE::kShow, "open"); },
			.closeMenu = [runtime = a_runtimeVersion, runner](std::string a_name) {
				RequireSupportedRuntime(runtime);
				return QueueMenuMessage(
					*runner, std::move(a_name), RE::UI_MESSAGE_TYPE::kHide, "close"); },
		};
	}
}

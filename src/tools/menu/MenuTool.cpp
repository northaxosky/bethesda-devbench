#include "MenuTool.h"

#include "ToolExtensions.h"
#include "tools/ToolPermissions.h"

#include <algorithm>
#include <array>

namespace dvb::tools
{
	namespace
	{
		void RequireObject(const json& a_args)
		{
			if (!a_args.is_object())
				throw ToolError(400, "menu arguments must be an object");
		}

		void ValidateOnly(
			const json& a_args, std::initializer_list<std::string_view> a_allowed)
		{
			for (const auto& [key, value] : a_args.items())
			{
				(void)value;
				if (std::ranges::find(a_allowed, key) == a_allowed.end())
					throw ToolError(
						400, std::format("unexpected parameter '{}' for menu action", key));
			}
		}

		std::string ReadString(
			const json& a_args, std::string_view a_name, bool a_required, std::string a_default = {})
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
			{
				if (a_required)
					throw ToolError(
						400, std::format("missing required parameter '{}'", a_name));
				return a_default;
			}
			if (!it->is_string())
				throw ToolError(400, std::format("'{}' must be a string", a_name));
			return it->get<std::string>();
		}

		std::size_t ReadIndex(const json& a_args)
		{
			const auto it = a_args.find("index");
			if (it == a_args.end())
				throw ToolError(400, "action 'accept' requires an explicit button 'index'");
			if (!it->is_number_integer())
				throw ToolError(400, "'index' must be a non-negative integer");
			const auto value = it->get<std::int64_t>();
			if (value < 0)
				throw ToolError(400, "'index' must be a non-negative integer");
			return static_cast<std::size_t>(value);
		}

		std::string ValidateName(const json& a_args)
		{
			auto name = ReadString(a_args, "name", true);
			if (name.empty())
				throw ToolError(400, "'name' must not be empty");
			if (name.size() > kMaxMenuNameBytes)
				throw ToolError(400, std::format("'name' exceeds {} bytes", kMaxMenuNameBytes));
			if (name.find('\0') != std::string::npos)
				throw ToolError(400, "'name' must not contain an embedded NUL");
			return name;
		}

		std::string LowerAscii(std::string_view a_value)
		{
			std::string out{ a_value };
			std::ranges::transform(out, out.begin(), [](unsigned char a_ch) {
				return static_cast<char>(
					a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return out;
		}

		bool ContainsAsciiCaseInsensitive(std::string_view a_value, std::string_view a_needle)
		{
			if (a_needle.empty())
				return false;
			return LowerAscii(a_value).contains(LowerAscii(a_needle));
		}

		std::string DescribeRegisteredMenus(const std::vector<std::string>& a_names)
		{
			if (a_names.empty())
				return "none currently registered";

			std::string out;
			for (const auto& name : a_names)
			{
				if (!out.empty())
					out += ", ";
				out += name;
			}
			return out;
		}

		json FingerprintJson(const MenuDialogFingerprint& a_fingerprint)
		{
			return json{
				{ "headerText", a_fingerprint.headerText },
				{ "bodyText", a_fingerprint.bodyText },
				{ "buttons", a_fingerprint.buttons },
			};
		}

		json SnapshotJson(const MenuDialogSnapshot& a_snapshot)
		{
			return json{
				{ "messageBoxOpen", true },
				{ "headerText", a_snapshot.fingerprint.headerText },
				{ "bodyText", a_snapshot.fingerprint.bodyText },
				{ "buttons", a_snapshot.fingerprint.buttons },
				{ "cancelIndex",
					a_snapshot.cancelIndex ? json(*a_snapshot.cancelIndex) : json(nullptr) },
				{ "fingerprint", FingerprintJson(a_snapshot.fingerprint) },
			};
		}

		json HandleMenu(const json& a_args, const ToolContext& a_context,
			bool a_allowControlActions, const MenuBackend& a_backend)
		{
			RequireObject(a_args);
			const auto action = ReadString(a_args, "action", false, "list");

			if (action == "list")
			{
				ValidateOnly(a_args, { "action" });
				if (!a_backend.listOpenMenus)
					throw ToolError(503, "menu inventory is unavailable");
				auto openMenus = a_backend.listOpenMenus();
				std::ranges::sort(openMenus);
				openMenus.erase(std::unique(openMenus.begin(), openMenus.end()), openMenus.end());
				return json{
					{ "openMenus", openMenus },
					{ "messageBoxOpen",
						std::ranges::find(openMenus, "MessageBoxMenu") != openMenus.end() },
					{ "registered", ToolExtensions::Keys("menu") },
				};
			}

			if (action == "describe")
			{
				ValidateOnly(a_args, { "action", "name" });
				const auto name = ReadString(a_args, "name", false);
				if (!name.empty())
				{
					const auto extension = ToolExtensions::Find("menu", name);
					if (!extension)
						throw ToolError(404,
							std::format(
								"no handler registered for menu '{}' (see menu list .registered)",
								name));
					return json{
						{ "registered", true },
						{ "name", name },
						{ "descriptor", extension->descriptor },
					};
				}
				if (!a_backend.describeDialog)
					throw ToolError(503, "message-box inspection is unavailable");
				const auto dialog = a_backend.describeDialog();
				return dialog ? SnapshotJson(*dialog) :
				                json{ { "messageBoxOpen", false } };
			}

			if (action == "invoke")
			{
				const auto name = ValidateName(a_args);
				RequireToolPermission(
					a_allowControlActions, ToolPermission::kControlActions);
				const auto extension = ToolExtensions::Find("menu", name);
				if (!extension)
					throw ToolError(404,
						std::format(
							"no handler registered for menu '{}' (see menu list .registered)",
							name));
				return extension->handler(a_args, a_context);
			}

			if (action == "accept")
			{
				ValidateOnly(a_args, { "action", "index", "matchBody" });
				const auto index = ReadIndex(a_args);
				const auto matchBody = ReadString(a_args, "matchBody", true);
				if (matchBody.empty())
					throw ToolError(
						400, "'matchBody' must identify the dialog being answered");
				if (matchBody.size() > kMaxDialogMatchBytes)
					throw ToolError(
						400, std::format("'matchBody' exceeds {} bytes", kMaxDialogMatchBytes));
				RequireToolPermission(
					a_allowControlActions, ToolPermission::kControlActions);
				if (!a_backend.describeDialog || !a_backend.acceptDialog)
					throw ToolError(503, "message-box acceptance is unavailable");

				const auto dialog = a_backend.describeDialog();
				if (!dialog)
					throw ToolError(409, "no active MessageBoxMenu");
				if (!ContainsAsciiCaseInsensitive(
						dialog->fingerprint.bodyText, matchBody))
					throw ToolError(409, "active dialog body did not match 'matchBody'");
				if (index >= dialog->fingerprint.buttons.size())
					throw ToolError(400,
						std::format(
							"'index' {} is outside the active dialog's {} buttons",
							index, dialog->fingerprint.buttons.size()));
				return a_backend.acceptDialog(
					MenuAcceptRequest{ .fingerprint = dialog->fingerprint, .index = index });
			}

			if (action == "open")
			{
				ValidateOnly(a_args, { "action", "name" });
				const auto name = ValidateName(a_args);
				RequireToolPermission(
					a_allowControlActions, ToolPermission::kControlActions);
				if (ToolExtensions::Find("menu", name))
					throw ToolError(400,
						std::format(
							"'{}' is a registered mod menu; use action='invoke'", name));
				if (!IsContextFreeMenuOpenTarget(
						name, a_backend.contextFreeOpenMenus))
					throw ToolError(422,
						std::format(
							"menu '{}' is not approved for context-free opening", name));
				if (!a_backend.openMenu)
					throw ToolError(503, "menu opening is unavailable");
				return a_backend.openMenu(name);
			}

			if (action == "close")
			{
				ValidateOnly(a_args, { "action", "name" });
				const auto name = ValidateName(a_args);
				RequireToolPermission(
					a_allowControlActions, ToolPermission::kControlActions);
				if (name == "MessageBoxMenu")
					throw ToolError(
						422, "MessageBoxMenu must be answered with action='accept'");
				if (!a_backend.closeMenu)
					throw ToolError(503, "menu closing is unavailable");
				return a_backend.closeMenu(name);
			}

			throw ToolError(400,
				std::format(
					"unknown menu action '{}' (list|describe|accept|open|close|invoke)",
					action));
		}
	}

	bool IsContextFreeMenuOpenTarget(
		std::string_view a_name, std::span<const std::string> a_allowlist)
	{
		return std::ranges::find(a_allowlist, a_name) != a_allowlist.end();
	}

	ToolDescriptor BuildMenuDescriptor(const MenuBackend* a_backend)
	{
		const auto registered = ToolExtensions::Keys("menu");
		const auto registeredSummary = DescribeRegisteredMenus(registered);
		const MenuBackend fallback;
		const auto&       backend = a_backend ? *a_backend : fallback;
		const auto approved = backend.contextFreeOpenMenus.empty() ?
		                          std::string("none") :
		                          DescribeRegisteredMenus(backend.contextFreeOpenMenus);

		ToolDescriptor descriptor;
		descriptor.name = "menu";
		descriptor.description =
			"Inspect and control " + backend.gameName +
			" menus. list returns tracked open menus and "
			"consumer-registered menu handlers. describe without a name snapshots the active "
			"MessageBoxMenu, including an exact text/button fingerprint; describe with a name "
			"returns a registered handler descriptor. accept requires both an explicit button "
			"index and matchBody, then rechecks the complete observed fingerprint immediately "
			"before dispatch so a changed dialog is never answered. " +
			backend.dialogLimitations +
			". open is restricted to adapter-approved context-free engine menus (" +
			approved + "); close hides a named non-message "
			"menu; invoke calls an existing ToolExtensions menu handler. Mutating actions require "
			"allowControlActions=true. Registered invoke targets: " +
			registeredSummary + ".";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "list", "describe", "accept", "open", "close", "invoke" }) }, { "default", "list" } } },
								{ "name", json{ { "type", "string" }, { "maxLength", kMaxMenuNameBytes }, { "description", "describe/invoke: registered menu (" + registeredSummary + "); open/close: engine menu" } } },
								{ "index", json{ { "type", "integer" }, { "minimum", 0 }, { "description", "accept: explicit 0-based button index" } } },
								{ "matchBody", json{ { "type", "string" }, { "minLength", 1 }, { "maxLength", kMaxDialogMatchBytes }, { "description", "accept: case-insensitive body substring identifying the expected dialog" } } },
							} },
		};
		return descriptor;
	}

	MenuService::MenuService(
		ToolRegistry& a_registry, bool a_allowControlActions, MenuBackend a_backend) :
		registry_(std::addressof(a_registry)),
		backend_(std::make_shared<MenuBackend>(std::move(a_backend))),
		handler_([a_allowControlActions, backend = backend_](
					 const json& a_args, const ToolContext& a_context) {
			return HandleMenu(a_args, a_context, a_allowControlActions, *backend);
		})
	{}

	void MenuService::Register()
	{
		registry_->Register(BuildMenuDescriptor(backend_.get()), handler_);
	}

	void MenuService::RefreshDescriptor()
	{
		Register();
	}

	std::shared_ptr<MenuService> RegisterMenuTool(
		ToolRegistry& a_registry, bool a_allowControlActions, MenuBackend a_backend)
	{
		auto service = std::make_shared<MenuService>(
			a_registry, a_allowControlActions, std::move(a_backend));
		service->Register();
		return service;
	}
}

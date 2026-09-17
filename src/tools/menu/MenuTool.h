#pragma once

#include "ToolRegistry.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dvb::tools
{
	inline constexpr std::size_t kMaxMenuNameBytes = 128;
	inline constexpr std::size_t kMaxDialogMatchBytes = 4096;

	struct MenuDialogFingerprint
	{
		std::string              headerText;
		std::string              bodyText;
		std::vector<std::string> buttons;

		bool operator==(const MenuDialogFingerprint&) const = default;
	};

	struct MenuDialogSnapshot
	{
		MenuDialogFingerprint      fingerprint;
		std::optional<std::size_t> cancelIndex;
	};

	struct MenuAcceptRequest
	{
		MenuDialogFingerprint fingerprint;
		std::size_t           index = 0;
	};

	struct MenuBackend
	{
		std::string                                        gameName = "game";
		std::vector<std::string>                           contextFreeOpenMenus;
		std::string                                        dialogLimitations =
			"cancelIndex may be unavailable on this runtime";
		std::function<std::vector<std::string>()>          listOpenMenus;
		std::function<std::optional<MenuDialogSnapshot>()> describeDialog;
		std::function<json(MenuAcceptRequest)>             acceptDialog;
		std::function<json(std::string)>                   openMenu;
		std::function<json(std::string)>                   closeMenu;
	};

	class MenuService final
	{
	public:
		MenuService(
			ToolRegistry& a_registry, bool a_allowControlActions, MenuBackend a_backend);

		void Register();
		void RefreshDescriptor();

	private:
		ToolRegistry*                registry_;
		std::shared_ptr<MenuBackend> backend_;
		ToolHandler                  handler_;
	};

	bool IsContextFreeMenuOpenTarget(
		std::string_view a_name, std::span<const std::string> a_allowlist);

	ToolDescriptor               BuildMenuDescriptor(const MenuBackend* a_backend = nullptr);
	std::shared_ptr<MenuService> RegisterMenuTool(
		ToolRegistry& a_registry, bool a_allowControlActions, MenuBackend a_backend);
}

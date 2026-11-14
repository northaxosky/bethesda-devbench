#pragma once

#include <algorithm>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Name-based: a pausing menu freezes the main thread.
namespace dvb::Modal
{
	// Runs on listener threads, so openMenus must be a lock-light snapshot.
	struct ModalSource
	{
		std::function<std::vector<std::string>()> openMenus;
		std::vector<std::string>                  nonBlockingMenus;
	};

	struct Blocker
	{
		std::string name;
		std::string hint;
	};

	inline constexpr std::string_view kMessageBox = "MessageBoxMenu";

	/// Menus the game profile lists as open during normal play.
	inline bool IsNonBlocking(
		const std::vector<std::string>& a_nonBlocking, std::string_view a_name)
	{
		return std::ranges::find(a_nonBlocking, a_name) != a_nonBlocking.end();
	}

	/// What the agent should do about a blocking menu.
	inline std::string HintFor(std::string_view a_name)
	{
		if (a_name == kMessageBox)
			return "menu action=describe to read it, then menu action=accept index=N (close does not dismiss a message box)";
		if (a_name == "Loading Menu" || a_name == "LoadingMenu")
			return "a load or door transition is in progress; wait for the modal.cleared event";
		if (a_name == "MainMenu" || a_name == "Main Menu")
			return "the game is at the main menu; load a save with game action=load or loadLast";
		return std::format("menu action=close name=\"{}\"", a_name);
	}

	/// The open menus that block, sorted by name, each with its hint.
	inline std::vector<Blocker> Blockers(
		const std::vector<std::string>& a_nonBlocking,
		const std::vector<std::string>& a_openMenus)
	{
		std::vector<Blocker> out;
		for (const auto& name : a_openMenus)
			if (!IsNonBlocking(a_nonBlocking, name))
				out.push_back({ name, HintFor(name) });
		std::ranges::sort(out, {}, &Blocker::name);
		return out;
	}
}

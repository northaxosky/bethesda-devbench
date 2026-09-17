#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::tools::input
{
	// Bethesda's PC keyboard devices report DirectInput scan codes.
	struct KeyboardKey
	{
		std::uint16_t scancode = 0;
		std::string   name;
	};

	std::optional<KeyboardKey>      ResolveKeyboardKey(std::string_view a_name);
	std::optional<KeyboardKey>      ResolveKeyboardKey(int a_scancode);
	const std::vector<KeyboardKey>& KeyboardKeyCatalog();

	struct KeyboardLease
	{
		KeyboardKey   key;
		std::string   owner;
		std::uint64_t generation = 0;
		std::int64_t  pressedAtMs = 0;
		std::int64_t  expiresAtMs = 0;
	};

	enum class KeyboardAcquireStatus
	{
		kAcquired,
		kAlreadyOwned,
		kConflict
	};

	struct KeyboardAcquireResult
	{
		KeyboardAcquireStatus status = KeyboardAcquireStatus::kConflict;
		KeyboardLease         lease;
	};

	// Pure ownership state. Native dispatch and synchronization live in InputService.
	class KeyboardLeaseTable
	{
	public:
		KeyboardAcquireResult Acquire(KeyboardKey a_key, std::string a_owner,
			std::int64_t a_nowMs, std::int64_t a_maxHoldMs);

		std::optional<KeyboardLease> Find(std::uint16_t a_scancode) const;
		std::optional<KeyboardLease> Remove(std::uint16_t a_scancode,
			std::string_view a_owner, bool a_force = false);
		std::optional<KeyboardLease> RemoveExact(
			std::uint16_t a_scancode, std::uint64_t a_generation);
		std::vector<KeyboardLease> Snapshot() const;
		std::vector<KeyboardLease> Expired(std::int64_t a_nowMs) const;
		std::size_t                Size() const noexcept { return leases_.size(); }

	private:
		std::vector<KeyboardLease> leases_;
		std::uint64_t              nextGeneration_ = 1;
	};
}

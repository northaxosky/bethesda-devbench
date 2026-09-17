#include "test_framework.h"

#include "tools/input/KeyboardInputState.h"

using namespace dvb::tools::input;

TEST_CASE("keyboard catalog resolves canonical names aliases and raw scan codes")
{
	const auto pageUp = ResolveKeyboardKey("Page-Up");
	CHECK(pageUp.has_value());
	CHECK(pageUp->name == "pageUp");
	CHECK(pageUp->scancode == 0xC9);

	const auto minus = ResolveKeyboardKey("-");
	CHECK(minus.has_value());
	CHECK(minus->name == "minus");

	const auto raw = ResolveKeyboardKey(0xAA);
	CHECK(raw.has_value());
	CHECK(raw->name == "scancode-170");
	CHECK(!ResolveKeyboardKey(0).has_value());
	CHECK(!ResolveKeyboardKey("not-a-key").has_value());
}

TEST_CASE("keyboard lease ownership conflicts and generations stay exact")
{
	KeyboardLeaseTable leases;
	const auto         key = *ResolveKeyboardKey("leftShift");

	const auto first = leases.Acquire(key, "owner-a", 1000, 500);
	CHECK(first.status == KeyboardAcquireStatus::kAcquired);
	CHECK(first.lease.generation != 0);

	const auto same = leases.Acquire(key, "owner-a", 1100, 900);
	CHECK(same.status == KeyboardAcquireStatus::kAlreadyOwned);
	CHECK(same.lease.generation == first.lease.generation);
	CHECK(same.lease.expiresAtMs == first.lease.expiresAtMs);

	const auto conflict = leases.Acquire(key, "owner-b", 1200, 500);
	CHECK(conflict.status == KeyboardAcquireStatus::kConflict);
	CHECK(!leases.RemoveExact(key.scancode, first.lease.generation + 1).has_value());
	CHECK(leases.Find(key.scancode).has_value());
	CHECK(leases.RemoveExact(key.scancode, first.lease.generation).has_value());

	const auto second = leases.Acquire(key, "owner-b", 2000, 500);
	CHECK(second.status == KeyboardAcquireStatus::kAcquired);
	CHECK(second.lease.generation > first.lease.generation);
}

TEST_CASE("keyboard lease expiration is bounded and non-destructive")
{
	KeyboardLeaseTable leases;
	const auto         a = leases.Acquire(*ResolveKeyboardKey("a"), "owner", 1000, 100);
	const auto         b = leases.Acquire(*ResolveKeyboardKey("b"), "owner", 1000, 300);
	CHECK(a.status == KeyboardAcquireStatus::kAcquired);
	CHECK(b.status == KeyboardAcquireStatus::kAcquired);

	const auto early = leases.Expired(1099);
	CHECK(early.empty());
	const auto expired = leases.Expired(1100);
	CHECK(expired.size() == 1);
	CHECK(expired.front().key.name == "a");
	CHECK(leases.Size() == 2);
}

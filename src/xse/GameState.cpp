#include "GameState.h"

#include <atomic>
#include <bit>

namespace dvb::game
{
	int CurrentFrame()
	{
		// AE 1.11.240, Fallout4.exe SHA-256
		// FDCEF37AC1230AF6D0B0050EB2142B139EF3A867B37B9211FB6EDFCC646072F8:
		// Main::Update (ID 2228917 / RVA 0xC339D0) unconditionally increments this
		// dword at RVA 0xC34149. ID 2664106 resolves the dword at RVA 0x2F31E88,
		// and binary-wide/source cross-checking found that single write xref. The
		// outer loop (ID 4484191 / RVA 0xC314F0) skips Main::Update and sleeps 50ms
		// on its inactive branch. This is therefore a wrapping main-update
		// generation, not a render-present counter and not a cadence guarantee.
		static const auto counter = []() noexcept -> std::uint32_t* {
			try
			{
				constexpr REL::Version supported{ 1, 11, 240, 0 };
				const auto             runtime = REX::FModule::GetExecutingModule().GetFileVersion();
				if (!REX::FModule::IsRuntimeAE() || runtime != supported)
				{
					logs::error(
						"devbench: frame source unavailable for runtime {}; only Fallout 4 AE "
						"1.11.240 is supported",
						runtime.string("."));
					return nullptr;
				}
				static REL::Relocation<std::uint32_t*> value{ REL::ID(2664106) };
				logs::info("{}",
					"devbench: using Fallout 4 main-update generation as frame source "
					"(Address Library ID 2664106)");
				return value.get();
			}
			catch (const std::exception& a_exception)
			{
				logs::error("devbench: frame source discovery failed: {}", a_exception.what());
				return nullptr;
			}
			catch (...)
			{
				logs::error(
					"{}", "devbench: frame source discovery failed with an unknown exception");
				return nullptr;
			}
		}();

		if (!counter)
			return -1;
		const auto value =
			std::atomic_ref<std::uint32_t>(*counter).load(std::memory_order_relaxed);
		return std::bit_cast<std::int32_t>(value);
	}
}

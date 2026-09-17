#include "Adapter.h"

#include "ConsoleBackend.h"
#include "GameBackend.h"
#include "InspectBackend.h"
#include "Lifecycle.h"
#include "camera/CameraBackend.h"
#include "capture/CaptureBackend.h"
#include "input/InputBackend.h"
#include "menu/MenuBackend.h"
#include "papyrus/PapyrusBackend.h"
#include "recording/RecordingBackend.h"
#include "rest/RestBackend.h"

namespace dvb::fallout4
{
	HostAdapter MakeHostAdapter(std::string a_runtimeVersion)
	{
		auto profile = Fallout4Profile();
		auto inspect = MakeInspectBackend(a_runtimeVersion);
		auto input = MakeInputBackend();
		input.gameName = profile.displayName;
		input.extenderName = profile.extenderName;
		auto menu = MakeMenuBackend(a_runtimeVersion);
		menu.gameName = profile.displayName;
		menu.contextFreeOpenMenus = profile.contextFreeMenus;
		auto recording = MakeRecordingBackend();
		recording.gameId = profile.id;
		recording.gameName = profile.displayName;
		recording.compatibleGameIds = profile.recordingGameIds;
		recording.compatibleRuntimeVariants =
			profile.recordingRuntimeCompatibility;
		recording.recordedOnVR = profile.vr;

		return HostAdapter{
			.profile = std::move(profile),
			.runtimeVersion = std::move(a_runtimeVersion),
			.console = [](EventBus& a_events) {
				auto lifetime =
					std::make_shared<ConsoleBackendLifetime>(a_events);
				return ConsoleBinding{
					.backend = lifetime->Backend(),
					.lifetime = std::move(lifetime),
				};
			},
			.game = MakeGameBackend(),
			.inspect = inspect,
			.input = std::move(input),
			.menu = std::move(menu),
			.camera = MakeCameraBackend(a_runtimeVersion),
			.papyrus = papyrus::MakePapyrusBackend(),
			.capture = capture::MakeCaptureBackend(inspect.scene),
			.recording = std::move(recording),
			.rest = MakeRestBackend(),
			.restOperations = MakeRestOperationCoordinator(),
			.initializeLifecycle =
				[](EventBus& a_events) { Lifecycle::Initialize(a_events); },
			.resetLifecycle = &Lifecycle::Reset,
			.ready = [] {
				const auto state = Lifecycle::GetSnapshot();
				return state.gameDataReady && state.gameLoaded &&
				       !state.inMainMenu && !state.inLoadingMenu &&
				       !state.loadInProgress &&
				       state.postLoadSucceeded != false;
			},
			.openMenus = [] { return Lifecycle::GetSnapshot().openMenus; },
		};
	}

	int CurrentFrame()
	{
		// AE 1.11.240, Fallout4.exe SHA-256
		// FDCEF37AC1230AF6D0B0050EB2142B139EF3A867B37B9211FB6EDFCC646072F8:
		// Main::Update increments this dword once per active update.
		static const auto counter = []() noexcept -> std::uint32_t* {
			try
			{
				constexpr REL::Version supported{ 1, 11, 240, 0 };
				const auto runtime =
					REX::FModule::GetExecutingModule().GetFileVersion();
				if (!REX::FModule::IsRuntimeAE() || runtime != supported)
				{
					REX::ERROR(
						"devbench: frame source unavailable for runtime {}; only "
						"Fallout 4 AE 1.11.240 is supported",
						runtime.string("."));
					return nullptr;
				}
				static REL::Relocation<std::uint32_t*> value{
					REL::ID(2664106)
				};
				REX::INFO(
					"devbench: using Fallout 4 main-update generation as frame "
					"source (Address Library ID 2664106)");
				return value.get();
			}
			catch (const std::exception& a_error)
			{
				REX::ERROR(
					"devbench: frame source discovery failed: {}",
					a_error.what());
				return nullptr;
			}
			catch (...)
			{
				REX::ERROR(
					"devbench: frame source discovery failed with an unknown "
					"exception");
				return nullptr;
			}
		}();
		if (!counter)
			return -1;
		const auto value = std::atomic_ref<std::uint32_t>(*counter).load(
			std::memory_order_relaxed);
		return std::bit_cast<std::int32_t>(value);
	}
}

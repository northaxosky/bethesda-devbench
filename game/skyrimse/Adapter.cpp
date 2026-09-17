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
#include "ui/Frontends.h"

#include <atomic>

// The pinned CommonLibVR-ng archive defines IsPressed in the same object as a
// partially modeled BSWin32KeyboardDevice vtable. Providing the leaf accessor
// here keeps the adapter on the public runtime-data seam and avoids pulling
// those unimplemented constructor/destructor slots into the plugin link.
bool RE::BSWin32KeyboardDevice::IsPressed(std::uint32_t a_keyCode) const
{
	const auto& keys = GetRuntimeData().curState;
	return a_keyCode < std::size(keys) && (keys[a_keyCode] & 0x80) != 0;
}

namespace dvb::skyrimse
{
	GameProfile CurrentProfile()
	{
		return SkyrimProfile(
			REL::Module::IsVR() ? RuntimeVariant::kSkyrimVR :
			REL::Module::IsAE() ? RuntimeVariant::kSkyrimAE :
			                      RuntimeVariant::kSkyrimSE);
	}

	HostAdapter MakeHostAdapter(std::string a_runtimeVersion)
	{
		auto profile = CurrentProfile();
		auto inspect = MakeInspectBackend(a_runtimeVersion);
		auto input = MakeInputBackend();
		input.gameName = profile.displayName;
		input.extenderName = profile.extenderName;
		auto menu = MakeMenuBackend(a_runtimeVersion);
		menu.gameName = profile.displayName;
		menu.contextFreeOpenMenus = profile.contextFreeMenus;
		auto recording = recording::MakeRecordingBackend();
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
			.onStarted =
				[](ToolRegistry& a_registry, EventBus&, const Config& a_config) {
					ui::ConfigureFrontends(a_registry, a_config);
				},
			.shutdown = [] {
				ui::ShutdownFrontends();
				ShutdownCamera();
			},
		};
	}

	int CurrentFrame()
	{
		static std::int32_t* counter = []() -> std::int32_t* {
			try
			{
				return reinterpret_cast<std::int32_t*>(
					REL::RelocationID(525008, 411489).address());
			}
			catch (...)
			{
				return nullptr;
			}
		}();
		return counter ?
		           std::atomic_ref<std::int32_t>(*counter).load(
					   std::memory_order_relaxed) :
		           -1;
	}
}

#include "CaptureBackend.h"

#include "GameState.h"
#include "MainThread.h"
#include "RuntimePaths.h"
#include "io/WindowsPaths.h"

#include <RE/Skyrim.h>

namespace dvb::skyrimse::capture
{
	tools::capture::CaptureBackend MakeCaptureBackend(
		std::function<json()> a_sceneSnapshot)
	{
		return tools::capture::CaptureBackend{
			.gameRoot = &RuntimeGameDirectory,
			.resolvePhysicalPath = &io::PhysicalFilePath,
			.queueNativeScreenshot = [] {
				return MainThread::RunAndWait([]() -> json {
					const auto controls = RE::MenuControls::GetSingleton();
					return controls && controls->QueueScreenshot();
				},
					std::chrono::milliseconds(1000))
				    .get<bool>();
			},
			.sceneSnapshot = std::move(a_sceneSnapshot),
			.currentFrame = &game::CurrentFrame,
		};
	}
}

#pragma once

#include "EventBus.h"
#include "ToolRegistry.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace dvb
{
	struct Config;
}

namespace dvb::tools::capture
{
	inline constexpr std::uintmax_t kMaxCaptureBytes = 256ULL * 1024ULL * 1024ULL;
	inline constexpr std::size_t    kMaxScannedImages = 4096;
	inline constexpr std::size_t    kMaxSidecarBytes = 64ULL * 1024ULL;
	inline constexpr int            kMaxScreenshotListLimit = 500;

	struct CaptureConfiguration
	{
		std::filesystem::path              captureDirectory;
		std::vector<std::filesystem::path> scanDirectories;
		std::chrono::milliseconds          timeout{ 8000 };
		std::chrono::milliseconds          settle{ 500 };
	};

	CaptureConfiguration CaptureConfigurationFromConfig(const Config& a_config);

	// All filesystem paths passed to these callbacks use the native std::filesystem
	// representation. JSON conversion to forward slashes happens only at the provider/API
	// boundary.
	struct CaptureBackend
	{
		// Process-visible game root used for provider output and native screenshot scans.
		std::function<std::filesystem::path()> gameRoot;

		// Resolve an existing process-visible path through its open file handle to the physical
		// backing path. This is required under MO2/USVFS so returned artifacts are usable by an
		// external controller.
		std::function<std::filesystem::path(const std::filesystem::path&)> resolvePhysicalPath;

		// Must queue MenuControls::QueueScreenshot on the game main thread. A true result only
		// acknowledges that screenshotQueued was set; it is not completion or a path guarantee.
		std::function<bool()> queueNativeScreenshot;

		// Best-effort enrichment captured after the image is complete.
		std::function<json()> sceneSnapshot;
		std::function<int()>  currentFrame;
	};

	class CaptureService : public std::enable_shared_from_this<CaptureService>
	{
	public:
		CaptureService(EventBus& a_events, CaptureConfiguration a_configuration, CaptureBackend a_backend);

		json Handle(const json& a_args, const ToolContext& a_context);
		json ListScreenshots(const json& a_args) const;

		// Register once, then call RefreshDescriptor from the host's single global extension-change
		// listener. Both paths retain this service's existing backend/state and only replace the
		// registry descriptor/handler entry.
		void Register(ToolRegistry& a_registry);
		void RefreshDescriptor(ToolRegistry& a_registry);

	private:
		ToolHandler StableHandler();

		struct State;
		std::shared_ptr<State> state_;
	};

	ToolDescriptor BuildCaptureDescriptor();
	json           BuildScreenshotInspectionExtensionDescriptor();

	// The returned shared service is the explicit runtime-owned lifetime. Registered handlers use
	// weak references and fail closed after shutdown rather than retaining backend/EventBus state.
	std::shared_ptr<CaptureService> RegisterCaptureTool(
		ToolRegistry&        a_registry,
		EventBus&            a_events,
		CaptureConfiguration a_configuration,
		CaptureBackend       a_backend);

	// Registers inspect kind=screenshots through the existing extension seam without modifying or
	// duplicating InspectTool's dispatcher.
	void RegisterScreenshotInspectionExtension(
		const std::shared_ptr<CaptureService>& a_service);
}

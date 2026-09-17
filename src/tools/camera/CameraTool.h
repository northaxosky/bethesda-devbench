#pragma once

#include "ToolRegistry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace dvb::tools
{
	enum class CameraPov
	{
		kFirst,
		kThird,
		kVanity,
		kOther,
	};

	struct CameraSnapshot
	{
		CameraPov                    pov = CameraPov::kOther;
		bool                         freeCam = false;
		bool                         freeCamOwned = false;
		std::optional<std::uint32_t> stateId;
		std::optional<float>         x;
		std::optional<float>         y;
		std::optional<float>         z;
		std::optional<float>         pitch;
		std::optional<float>         yaw;
	};

	struct CameraDriveRequest
	{
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;
		float pitch = 0.0F;
		float yaw = 0.0F;
	};

	enum class FreeCameraExitDecision
	{
		kAlreadyOff,
		kNotOwned,
		kExitOwned,
		kOwnershipLost,
	};

	class FreeCameraOwnership
	{
	public:
		bool                   Observe(bool a_freeCam);
		bool                   ClaimTransition(bool a_wasFreeCam, bool a_isFreeCam);
		FreeCameraExitDecision PrepareExit(bool a_freeCam);
		void                   CompleteExit(bool a_freeCam);

	private:
		std::mutex mutex_;
		bool       owned_ = false;
	};

	struct CameraBackend
	{
		std::function<CameraSnapshot()>                get;
		std::function<json(CameraPov)>                 setPov;
		std::function<json(bool)>                      setFreeCamera;
		std::function<json(const CameraDriveRequest&)> drive;
	};

	class CameraService final
	{
	public:
		CameraService(
			ToolRegistry& a_registry, bool a_allowControlActions, CameraBackend a_backend);

		void Register();
		void RefreshDescriptor();

	private:
		ToolRegistry*                  registry_;
		std::shared_ptr<CameraBackend> backend_;
		ToolHandler                    handler_;
	};

	std::string_view CameraPovName(CameraPov a_pov);

	ToolDescriptor                 BuildCameraDescriptor();
	std::shared_ptr<CameraService> RegisterCameraTool(
		ToolRegistry& a_registry, bool a_allowControlActions, CameraBackend a_backend);
}

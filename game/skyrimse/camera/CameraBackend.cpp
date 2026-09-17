#include "CameraBackend.h"

#include "MainThread.h"
#include "game/skyrimse/vr/VRFreeCamera.h"
#include "tools/camera/MutationDispatch.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace dvb::skyrimse
{
	namespace
	{
		bool IsFreeCamera(const RE::PlayerCamera& a_camera)
		{
			return a_camera.IsInFreeCameraMode();
		}

		tools::CameraPov CurrentPov(const RE::PlayerCamera& a_camera)
		{
			if (a_camera.IsInFirstPerson())
				return tools::CameraPov::kFirst;
			if (a_camera.IsInThirdPerson())
				return tools::CameraPov::kThird;
			if (a_camera.currentState &&
				a_camera.currentState->id == RE::CameraState::kAutoVanity)
				return tools::CameraPov::kVanity;
			return tools::CameraPov::kOther;
		}

		tools::CameraSnapshot SnapshotCamera(
			RE::PlayerCamera& a_camera, tools::FreeCameraOwnership& a_flatOwnership)
		{
			const bool            freeCamera = IsFreeCamera(a_camera);
			const bool            owned = REL::Module::IsVR() ?
			                                  vr::free_camera::IsOwned() :
			                                  a_flatOwnership.Observe(freeCamera);
			tools::CameraSnapshot out{
				.pov = CurrentPov(a_camera),
				.freeCam = freeCamera,
				.freeCamOwned = owned,
			};
			if (a_camera.currentState)
				out.stateId = static_cast<std::uint32_t>(a_camera.currentState->id);

			if (!REL::Module::IsVR() && freeCamera && a_camera.currentState)
			{
				const auto state =
					static_cast<const RE::FreeCameraState*>(a_camera.currentState.get());
				out.x = state->translation.x;
				out.y = state->translation.y;
				out.z = state->translation.z;
				out.pitch = state->rotation.x;
				out.yaw = state->rotation.y;
			}
			else if (a_camera.cameraRoot)
			{
				const auto& translation = a_camera.cameraRoot->world.translate;
				out.x = translation.x;
				out.y = translation.y;
				out.z = translation.z;
				RE::NiPoint3 euler{};
				if (a_camera.cameraRoot->world.rotate.ToEulerAnglesXYZ(euler))
				{
					out.pitch = euler.x;
					out.yaw = euler.z;
				}
			}
			return out;
		}

		json StateResult(
			std::string_view a_action, const tools::CameraSnapshot& a_snapshot)
		{
			json out{
				{ "queued", false },
				{ "completed", true },
				{ "action", a_action },
				{ "pov", tools::CameraPovName(a_snapshot.pov) },
				{ "freeCam", a_snapshot.freeCam },
				{ "freeCamOwned", a_snapshot.freeCamOwned },
				{ "freeCamBackend", REL::Module::IsVR() ? "vr-state" : "engine" },
				{ "stateId", a_snapshot.stateId ? json(*a_snapshot.stateId) : json(nullptr) },
			};
			if (a_snapshot.x)
				out["camX"] = *a_snapshot.x;
			if (a_snapshot.y)
				out["camY"] = *a_snapshot.y;
			if (a_snapshot.z)
				out["camZ"] = *a_snapshot.z;
			if (a_snapshot.pitch)
				out["camPitch"] = *a_snapshot.pitch;
			if (a_snapshot.yaw)
				out["camYaw"] = *a_snapshot.yaw;
			return out;
		}

		tools::CancelableMutationRunner::Submit MainThreadSubmitter()
		{
			return [](std::function<void()> a_task) {
				const auto tasks = SKSE::GetTaskInterface();
				if (!tasks)
					return false;
				try
				{
					tasks->AddTask(std::move(a_task));
					return true;
				}
				catch (const std::exception& a_exception)
				{
					logs::error(
						"devbench: could not enqueue Skyrim camera mutation: {}",
						a_exception.what());
					return false;
				}
			};
		}

		tools::CameraSnapshot ReadCamera(tools::FreeCameraOwnership& a_ownership)
		{
			const auto value = MainThread::RunAndWait([&a_ownership]() -> json {
				const auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera)
					throw ToolError(503, "PlayerCamera is unavailable");
				const auto snapshot = SnapshotCamera(*camera, a_ownership);
				json       out{
					{ "pov", tools::CameraPovName(snapshot.pov) },
					{ "freeCam", snapshot.freeCam },
					{ "freeCamOwned", snapshot.freeCamOwned },
					{ "stateId", snapshot.stateId ? json(*snapshot.stateId) : json(nullptr) },
				};
				if (snapshot.x)
					out["x"] = *snapshot.x;
				if (snapshot.y)
					out["y"] = *snapshot.y;
				if (snapshot.z)
					out["z"] = *snapshot.z;
				if (snapshot.pitch)
					out["pitch"] = *snapshot.pitch;
				if (snapshot.yaw)
					out["yaw"] = *snapshot.yaw;
				return out;
			});

			tools::CameraSnapshot snapshot;
			const auto            pov = value.at("pov").get<std::string>();
			snapshot.pov = pov == "first"  ? tools::CameraPov::kFirst :
			               pov == "third"  ? tools::CameraPov::kThird :
			               pov == "vanity" ? tools::CameraPov::kVanity :
			                                 tools::CameraPov::kOther;
			snapshot.freeCam = value.at("freeCam").get<bool>();
			snapshot.freeCamOwned = value.at("freeCamOwned").get<bool>();
			if (!value.at("stateId").is_null())
				snapshot.stateId = value.at("stateId").get<std::uint32_t>();
			for (const auto [name, target] : {
					 std::pair{ "x", std::addressof(snapshot.x) },
					 std::pair{ "y", std::addressof(snapshot.y) },
					 std::pair{ "z", std::addressof(snapshot.z) },
					 std::pair{ "pitch", std::addressof(snapshot.pitch) },
					 std::pair{ "yaw", std::addressof(snapshot.yaw) },
				 })
				if (value.contains(name))
					*target = value.at(name).get<float>();
			return snapshot;
		}

		void ApplyPov(RE::PlayerCamera& a_camera, tools::CameraPov a_pov)
		{
			switch (a_pov)
			{
				case tools::CameraPov::kFirst:
					(void)a_camera.ForceFirstPerson();
					return;
				case tools::CameraPov::kThird:
					(void)a_camera.ForceThirdPerson();
					return;
				case tools::CameraPov::kVanity:
					a_camera.PushCameraState(RE::CameraState::kAutoVanity);
					return;
				case tools::CameraPov::kOther:
					break;
			}
			throw ToolError(400, "unsupported camera POV");
		}
	}

	tools::CameraBackend MakeCameraBackend(std::string)
	{
		auto ownership = std::make_shared<tools::FreeCameraOwnership>();
		auto runner =
			std::make_shared<tools::CancelableMutationRunner>(MainThreadSubmitter());

		return tools::CameraBackend{
			.get = [ownership] { return ReadCamera(*ownership); },
			.setPov = [ownership, runner](tools::CameraPov a_pov) { return runner->Run("camera setPov", [a_pov, ownership]() -> json {
																		const auto camera = RE::PlayerCamera::GetSingleton();
																		if (!camera)
																			throw ToolError(503, "PlayerCamera is unavailable");
																		if (IsFreeCamera(*camera))
																			throw ToolError(
																				409, "setPov is unavailable while free-camera mode is active");
																		ApplyPov(*camera, a_pov);
																		auto result = StateResult("setPov", SnapshotCamera(*camera, *ownership));
																		result["requestedPov"] = tools::CameraPovName(a_pov);
																		result["applied"] = CurrentPov(*camera) == a_pov;
																		return result;
																	}); },
			.setFreeCamera = [ownership, runner](bool a_on) {
				const auto session = vr::free_camera::CurrentSession();
				return runner->Run("camera freecam", [a_on, session, ownership]() -> json {
					const auto camera = RE::PlayerCamera::GetSingleton();
					if (!camera)
						throw ToolError(503, "PlayerCamera is unavailable");
					const bool wasFree = IsFreeCamera(*camera);
					if (REL::Module::IsVR()) {
						vr::free_camera::SetEnabled(a_on, session);
						auto result = StateResult("freecam", SnapshotCamera(*camera, *ownership));
						result["on"] = a_on;
						result["changed"] = wasFree != result.value("freeCam", false);
						return result;
					}

					if (a_on) {
						if (wasFree) {
							if (!ownership->Observe(true))
								throw ToolError(409,
									"free-camera mode is already active but is not owned by DevBench");
							auto result = StateResult(
								"freecam", SnapshotCamera(*camera, *ownership));
							result["on"] = true;
							result["changed"] = false;
							return result;
						}
						camera->ToggleFreeCameraMode(false);
						const bool isFree = IsFreeCamera(*camera);
						ownership->ClaimTransition(wasFree, isFree);
						if (!isFree)
							throw ToolError(409, "Skyrim rejected the free-camera transition");
					} else {
						switch (ownership->PrepareExit(wasFree)) {
						case tools::FreeCameraExitDecision::kAlreadyOff:
						case tools::FreeCameraExitDecision::kOwnershipLost: {
							auto result = StateResult(
								"freecam", SnapshotCamera(*camera, *ownership));
							result["on"] = false;
							result["changed"] = false;
							return result;
						}
						case tools::FreeCameraExitDecision::kNotOwned:
							throw ToolError(409,
								"free-camera mode is active but is not owned by DevBench");
						case tools::FreeCameraExitDecision::kExitOwned:
							break;
						}
						camera->ToggleFreeCameraMode(false);
						const bool isFree = IsFreeCamera(*camera);
						ownership->CompleteExit(isFree);
						if (isFree)
							throw ToolError(
								503, "Skyrim did not exit the owned free-camera mode");
					}
					auto result = StateResult("freecam", SnapshotCamera(*camera, *ownership));
					result["on"] = a_on;
					result["changed"] = true;
					return result;
				}); },
			.drive = [ownership, runner](const tools::CameraDriveRequest& a_request) {
				const auto session = vr::free_camera::CurrentSession();
				return runner->Run("camera drive", [request = a_request, session, ownership]() -> json {
					const auto camera = RE::PlayerCamera::GetSingleton();
					if (!camera)
						throw ToolError(503, "PlayerCamera is unavailable");
					if (REL::Module::IsVR()) {
						vr::free_camera::Drive(request.x, request.y, request.z,
							request.pitch, request.yaw, session);
					} else {
						if (!IsFreeCamera(*camera)) {
							ownership->Observe(false);
							throw ToolError(
								409, "camera drive requires active free-camera mode");
						}
						if (!ownership->Observe(true))
							throw ToolError(409,
								"camera drive refuses a free-camera mode not owned by DevBench");
						const auto state =
							static_cast<RE::FreeCameraState*>(camera->currentState.get());
						state->translation = RE::NiPoint3{ request.x, request.y, request.z };
						state->rotation.x = request.pitch;
						state->rotation.y = request.yaw;
					}
					auto result = StateResult("drive", SnapshotCamera(*camera, *ownership));
					result["applied"] = true;
					return result;
				}); },
		};
	}

	void CameraPreLoad() noexcept
	{
		try
		{
			vr::free_camera::BeginLoad();
		}
		catch (...)
		{
		}
	}

	void CameraPostLoad() noexcept
	{
		try
		{
			vr::free_camera::EndLoad();
		}
		catch (...)
		{
		}
	}

	void ShutdownCamera() noexcept
	{
		CameraPreLoad();
	}
}

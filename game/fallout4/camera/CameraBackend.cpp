#include "CameraBackend.h"

#include "MainThread.h"
#include "tools/camera/MutationDispatch.h"

namespace dvb::fallout4
{
	namespace
	{
		constexpr std::string_view kSupportedRuntime = "1.11.240.0";
		constexpr std::uintptr_t   kStartAutoVanityRva = 0x102B4A0;

		bool IsFreeCamera(const RE::PlayerCamera& a_camera)
		{
			return a_camera.QCameraEquals(RE::CameraStates::kFree);
		}

		tools::CameraPov CurrentPov(const RE::PlayerCamera& a_camera)
		{
			if (a_camera.QCameraEquals(RE::CameraStates::kFirstPerson))
				return tools::CameraPov::kFirst;
			if (a_camera.QCameraEquals(RE::CameraStates::k3rdPerson))
				return tools::CameraPov::kThird;
			if (a_camera.QCameraEquals(RE::CameraStates::kAutoVanity))
				return tools::CameraPov::kVanity;
			return tools::CameraPov::kOther;
		}

		tools::CameraSnapshot SnapshotCamera(
			RE::PlayerCamera& a_camera, tools::FreeCameraOwnership& a_ownership)
		{
			const bool            freeCamera = IsFreeCamera(a_camera);
			tools::CameraSnapshot out{
				.pov = CurrentPov(a_camera),
				.freeCam = freeCamera,
				.freeCamOwned = a_ownership.Observe(freeCamera),
			};
			if (a_camera.currentState)
				out.stateId = static_cast<std::uint32_t>(
					a_camera.currentState->id.get());

			if (freeCamera && a_camera.currentState)
			{
				const auto free =
					static_cast<const RE::FreeCameraState*>(a_camera.currentState.get());
				out.x = free->translation.x;
				out.y = free->translation.y;
				out.z = free->translation.z;
				// Fallout 4 stores heading at rotation.x and pitch at rotation.y.
				out.yaw = free->rotation.x;
				out.pitch = free->rotation.y;
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
				{ "stateId",
					a_snapshot.stateId ? json(*a_snapshot.stateId) : json(nullptr) },
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

		void RequireSupportedRuntime(std::string_view a_runtimeVersion)
		{
			if (a_runtimeVersion != kSupportedRuntime)
				throw ToolError(503,
					std::format(
						"camera controls require Fallout 4 {}, current runtime is {}",
						kSupportedRuntime, a_runtimeVersion));
		}

		tools::CancelableMutationRunner::Submit MainThreadSubmitter()
		{
			return [](std::function<void()> a_task) {
				const auto tasks = F4SE::GetTaskInterface();
				if (!tasks)
					return false;
				try
				{
					tasks->AddTask(std::move(a_task));
					return true;
				}
				catch (const std::exception& a_exception)
				{
					REX::ERROR(
						"devbench: could not enqueue camera mutation: {}", a_exception.what());
					return false;
				}
			};
		}

		tools::CameraSnapshot ReadCamera(tools::FreeCameraOwnership& a_ownership)
		{
			const auto result = MainThread::RunAndWait([&a_ownership]() -> json {
				const auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera)
					throw ToolError(503, "PlayerCamera is unavailable");
				const auto snapshot = SnapshotCamera(*camera, a_ownership);
				json       out{
					{ "pov", tools::CameraPovName(snapshot.pov) },
					{ "freeCam", snapshot.freeCam },
					{ "freeCamOwned", snapshot.freeCamOwned },
					{ "stateId",
						snapshot.stateId ? json(*snapshot.stateId) : json(nullptr) },
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
			const auto            pov = result.at("pov").get<std::string>();
			snapshot.pov = pov == "first"  ? tools::CameraPov::kFirst :
			               pov == "third"  ? tools::CameraPov::kThird :
			               pov == "vanity" ? tools::CameraPov::kVanity :
			                                 tools::CameraPov::kOther;
			snapshot.freeCam = result.at("freeCam").get<bool>();
			snapshot.freeCamOwned = result.at("freeCamOwned").get<bool>();
			if (!result.at("stateId").is_null())
				snapshot.stateId = result.at("stateId").get<std::uint32_t>();
			for (const auto [name, target] : {
					 std::pair{ "x", std::addressof(snapshot.x) },
					 std::pair{ "y", std::addressof(snapshot.y) },
					 std::pair{ "z", std::addressof(snapshot.z) },
					 std::pair{ "pitch", std::addressof(snapshot.pitch) },
					 std::pair{ "yaw", std::addressof(snapshot.yaw) },
				 })
			{
				if (result.contains(name))
					*target = result.at(name).get<float>();
			}
			return snapshot;
		}

		void ApplyPov(RE::PlayerCamera& a_camera, tools::CameraPov a_pov)
		{
			using SetPov = void (*)(RE::PlayerCamera*);
			switch (a_pov)
			{
				case tools::CameraPov::kFirst:
				{
					// Normal Set1stPerson, not the force variant.
					static REL::Relocation<SetPov> setFirst{ REL::ID(2248339) };
					setFirst(std::addressof(a_camera));
					return;
				}
				case tools::CameraPov::kThird:
				{
					// Normal Set3rdPerson, not the force variant.
					static REL::Relocation<SetPov> setThird{ REL::ID(2248347) };
					setThird(std::addressof(a_camera));
					return;
				}
				case tools::CameraPov::kVanity:
				{
					// No Address Library ID exists for this entry point. The exact
					// 1.11.240 RVA was matched against the 1.11.221 StartAutoVanityMode
					// body; the executable gate above is therefore mandatory.
					static REL::Relocation<SetPov> startVanity{
						REL::Offset(kStartAutoVanityRva)
					};
					startVanity(std::addressof(a_camera));
					return;
				}
				case tools::CameraPov::kOther:
					break;
			}
			throw ToolError(400, "unsupported camera POV");
		}
	}

	tools::CameraBackend MakeCameraBackend(std::string a_runtimeVersion)
	{
		auto ownership = std::make_shared<tools::FreeCameraOwnership>();
		auto runner =
			std::make_shared<tools::CancelableMutationRunner>(MainThreadSubmitter());

		return tools::CameraBackend{
			.get = [runtime = a_runtimeVersion, ownership] {
				RequireSupportedRuntime(runtime);
				return ReadCamera(*ownership); },
			.setPov =
				[runtime = a_runtimeVersion, ownership, runner](tools::CameraPov a_pov) {
					RequireSupportedRuntime(runtime);
					return runner->Run("camera setPov",
						[a_pov, ownership]() -> json {
							const auto camera = RE::PlayerCamera::GetSingleton();
							if (!camera)
								throw ToolError(503, "PlayerCamera is unavailable");
							if (IsFreeCamera(*camera))
								throw ToolError(
									409, "setPov is unavailable while free-camera mode is active");
							ApplyPov(*camera, a_pov);
							auto result = StateResult(
								"setPov", SnapshotCamera(*camera, *ownership));
							result["requestedPov"] = tools::CameraPovName(a_pov);
							result["applied"] =
								CurrentPov(*camera) == a_pov;
							return result;
						});
				},
			.setFreeCamera =
				[runtime = a_runtimeVersion, ownership, runner](bool a_on) {
					RequireSupportedRuntime(runtime);
					return runner->Run("camera freecam",
						[a_on, ownership]() -> json {
							const auto camera = RE::PlayerCamera::GetSingleton();
							if (!camera)
								throw ToolError(503, "PlayerCamera is unavailable");

							const bool wasFree = IsFreeCamera(*camera);
							if (a_on)
							{
								if (wasFree)
								{
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
									throw ToolError(
										409, "Fallout 4 rejected the free-camera transition");
								auto result = StateResult(
									"freecam", SnapshotCamera(*camera, *ownership));
								result["on"] = true;
								result["changed"] = true;
								return result;
							}

							switch (ownership->PrepareExit(wasFree))
							{
								case tools::FreeCameraExitDecision::kAlreadyOff:
								{
									auto result = StateResult(
										"freecam", SnapshotCamera(*camera, *ownership));
									result["on"] = false;
									result["changed"] = false;
									return result;
								}
								case tools::FreeCameraExitDecision::kNotOwned:
									throw ToolError(409,
										"free-camera mode is active but is not owned by DevBench");
								case tools::FreeCameraExitDecision::kOwnershipLost:
								{
									auto result = StateResult(
										"freecam", SnapshotCamera(*camera, *ownership));
									result["on"] = false;
									result["changed"] = false;
									result["ownershipLost"] = true;
									return result;
								}
								case tools::FreeCameraExitDecision::kExitOwned:
									break;
							}

							camera->ToggleFreeCameraMode(false);
							const bool isFree = IsFreeCamera(*camera);
							ownership->CompleteExit(isFree);
							if (isFree)
								throw ToolError(
									503, "Fallout 4 did not exit the owned free-camera mode");
							auto result = StateResult(
								"freecam", SnapshotCamera(*camera, *ownership));
							result["on"] = false;
							result["changed"] = true;
							return result;
						});
				},
			.drive =
				[runtime = a_runtimeVersion, ownership, runner](
					const tools::CameraDriveRequest& a_request) {
					RequireSupportedRuntime(runtime);
					return runner->Run("camera drive",
						[request = a_request, ownership]() -> json {
							const auto camera = RE::PlayerCamera::GetSingleton();
							if (!camera)
								throw ToolError(503, "PlayerCamera is unavailable");
							if (!IsFreeCamera(*camera))
							{
								ownership->Observe(false);
								throw ToolError(
									409, "camera drive requires active free-camera mode");
							}
							if (!ownership->Observe(true))
								throw ToolError(409,
									"camera drive refuses a free-camera mode not owned by DevBench");

							const auto freeCamera =
								static_cast<RE::FreeCameraState*>(camera->currentState.get());
							freeCamera->translation =
								RE::NiPoint3{ request.x, request.y, request.z };
							// Native FreeCameraState stores heading at +0x34 (x) and
							// pitch at +0x38 (y), opposite the old upstream assignment.
							freeCamera->rotation.x = request.yaw;
							freeCamera->rotation.y = request.pitch;

							auto result = StateResult(
								"drive", SnapshotCamera(*camera, *ownership));
							result["applied"] =
								freeCamera->translation.x == request.x &&
								freeCamera->translation.y == request.y &&
								freeCamera->translation.z == request.z &&
								freeCamera->rotation.x == request.yaw &&
								freeCamera->rotation.y == request.pitch;
							return result;
						});
				},
		};
	}
}

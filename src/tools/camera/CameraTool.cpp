#include "CameraTool.h"

#include "tools/ToolPermissions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dvb::tools
{
	namespace
	{
		constexpr double kTwoPi = 6.283185307179586476925286766559;

		void RequireObject(const json& a_args)
		{
			if (!a_args.is_object())
				throw ToolError(400, "camera arguments must be an object");
		}

		void ValidateOnly(
			const json& a_args, std::initializer_list<std::string_view> a_allowed)
		{
			for (const auto& [key, value] : a_args.items())
			{
				(void)value;
				if (std::ranges::find(a_allowed, key) == a_allowed.end())
					throw ToolError(
						400, std::format("unexpected parameter '{}' for camera action", key));
			}
		}

		std::string ReadString(
			const json& a_args, std::string_view a_name, bool a_required, std::string a_default = {})
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
			{
				if (a_required)
					throw ToolError(
						400, std::format("missing required parameter '{}'", a_name));
				return a_default;
			}
			if (!it->is_string())
				throw ToolError(400, std::format("'{}' must be a string", a_name));
			return it->get<std::string>();
		}

		bool ReadBool(const json& a_args, std::string_view a_name, bool a_default)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return a_default;
			if (!it->is_boolean())
				throw ToolError(400, std::format("'{}' must be a boolean", a_name));
			return it->get<bool>();
		}

		float ReadFiniteFloat(const json& a_args, std::string_view a_name)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				throw ToolError(
					400, std::format("action 'drive' requires '{}'", a_name));
			if (!it->is_number())
				throw ToolError(400, std::format("'{}' must be a number", a_name));
			const auto value = it->get<double>();
			if (!std::isfinite(value) ||
				std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
				throw ToolError(400,
					std::format("'{}' must be finite and representable as a float", a_name));
			return static_cast<float>(value);
		}

		float NormalizeRadians(float a_value)
		{
			return static_cast<float>(
				std::remainder(static_cast<double>(a_value), kTwoPi));
		}

		CameraPov ParsePov(std::string_view a_value)
		{
			if (a_value == "first")
				return CameraPov::kFirst;
			if (a_value == "third")
				return CameraPov::kThird;
			if (a_value == "vanity")
				return CameraPov::kVanity;
			throw ToolError(400,
				std::format(
					"invalid camera pov '{}' (first|third|vanity)", a_value));
		}

		json SnapshotJson(const CameraSnapshot& a_snapshot)
		{
			json out{
				{ "pov", CameraPovName(a_snapshot.pov) },
				{ "freeCam", a_snapshot.freeCam },
				{ "freeCamOwned", a_snapshot.freeCamOwned },
				{ "freeCamBackend", "engine" },
				{ "stateId",
					a_snapshot.stateId ? json(*a_snapshot.stateId) : json(nullptr) },
			};
			for (const auto [name, value] : {
					 std::pair{ "camX", a_snapshot.x },
					 std::pair{ "camY", a_snapshot.y },
					 std::pair{ "camZ", a_snapshot.z },
					 std::pair{ "camPitch", a_snapshot.pitch },
					 std::pair{ "camYaw", a_snapshot.yaw },
				 })
			{
				if (value)
					out[name] = *value;
			}
			return out;
		}

		CameraSnapshot ReadSnapshot(const CameraBackend& a_backend)
		{
			if (!a_backend.get)
				throw ToolError(503, "camera state is unavailable");
			return a_backend.get();
		}

		json HandleCamera(
			const json& a_args, bool a_allowControlActions, const CameraBackend& a_backend)
		{
			RequireObject(a_args);
			const auto action = ReadString(a_args, "action", false, "get");

			if (action == "get")
			{
				ValidateOnly(a_args, { "action" });
				return SnapshotJson(ReadSnapshot(a_backend));
			}

			RequireToolPermission(
				a_allowControlActions, ToolPermission::kControlActions);

			if (action == "setPov")
			{
				ValidateOnly(a_args, { "action", "pov" });
				const auto pov = ParsePov(ReadString(a_args, "pov", true));
				if (ReadSnapshot(a_backend).freeCam)
					throw ToolError(409,
						"setPov is unavailable while free-camera mode is active");
				if (!a_backend.setPov)
					throw ToolError(503, "camera POV control is unavailable");
				return a_backend.setPov(pov);
			}

			if (action == "freecam")
			{
				ValidateOnly(a_args, { "action", "on" });
				if (!a_backend.setFreeCamera)
					throw ToolError(503, "free-camera control is unavailable");
				return a_backend.setFreeCamera(ReadBool(a_args, "on", true));
			}

			if (action == "drive")
			{
				ValidateOnly(
					a_args, { "action", "x", "y", "z", "pitch", "yaw" });
				const auto state = ReadSnapshot(a_backend);
				if (!state.freeCam)
					throw ToolError(
						409, "camera drive requires active free-camera mode");
				if (!state.freeCamOwned)
					throw ToolError(409,
						"camera drive refuses a free-camera mode not owned by DevBench");
				if (!a_backend.drive)
					throw ToolError(503, "free-camera driving is unavailable");
				CameraDriveRequest request{
					.x = ReadFiniteFloat(a_args, "x"),
					.y = ReadFiniteFloat(a_args, "y"),
					.z = ReadFiniteFloat(a_args, "z"),
					.pitch = NormalizeRadians(ReadFiniteFloat(a_args, "pitch")),
					.yaw = NormalizeRadians(ReadFiniteFloat(a_args, "yaw")),
				};
				return a_backend.drive(request);
			}

			throw ToolError(400,
				std::format(
					"unknown camera action '{}' (get|setPov|freecam|drive)", action));
		}
	}

	bool FreeCameraOwnership::Observe(bool a_freeCam)
	{
		const std::lock_guard lock{ mutex_ };
		if (owned_ && !a_freeCam)
			owned_ = false;
		return owned_;
	}

	bool FreeCameraOwnership::ClaimTransition(bool a_wasFreeCam, bool a_isFreeCam)
	{
		const std::lock_guard lock{ mutex_ };
		if (!a_wasFreeCam && a_isFreeCam)
			owned_ = true;
		else if (!a_isFreeCam)
			owned_ = false;
		return owned_;
	}

	FreeCameraExitDecision FreeCameraOwnership::PrepareExit(bool a_freeCam)
	{
		const std::lock_guard lock{ mutex_ };
		if (!owned_)
			return a_freeCam ? FreeCameraExitDecision::kNotOwned :
			                   FreeCameraExitDecision::kAlreadyOff;
		if (!a_freeCam)
		{
			owned_ = false;
			return FreeCameraExitDecision::kOwnershipLost;
		}
		return FreeCameraExitDecision::kExitOwned;
	}

	void FreeCameraOwnership::CompleteExit(bool a_freeCam)
	{
		const std::lock_guard lock{ mutex_ };
		if (!a_freeCam)
			owned_ = false;
	}

	std::string_view CameraPovName(CameraPov a_pov)
	{
		switch (a_pov)
		{
			case CameraPov::kFirst:
				return "first";
			case CameraPov::kThird:
				return "third";
			case CameraPov::kVanity:
				return "vanity";
			case CameraPov::kOther:
				return "other";
		}
		return "other";
	}

	ToolDescriptor BuildCameraDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "camera";
		descriptor.description =
			"Read or control the Fallout 4 player camera. get returns the live POV, state id, "
			"free-camera ownership, and available transform fields. setPov uses Fallout 4's "
			"normal first/third-person transitions and its native auto-vanity entry path. "
			"freecam enters/exits the native free-camera stack without freezing time, but only "
			"exits a mode entered by DevBench. drive requires DevBench-owned free camera and "
			"sets an absolute world-unit position plus pitch/yaw radians; angles are normalized "
			"before dispatch. Mutations require allowControlActions=true. A mutation that starts "
			"but misses its acknowledgement deadline has an uncertain outcome and must not be retried.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "get", "setPov", "freecam", "drive" }) }, { "default", "get" } } },
								{ "pov", json{ { "type", "string" }, { "enum", json::array({ "first", "third", "vanity" }) }, { "description", "setPov target" } } },
								{ "on", json{ { "type", "boolean" }, { "default", true }, { "description", "freecam target state" } } },
								{ "x", json{ { "type", "number" }, { "description", "drive world X in Fallout units" } } },
								{ "y", json{ { "type", "number" }, { "description", "drive world Y in Fallout units" } } },
								{ "z", json{ { "type", "number" }, { "description", "drive world Z in Fallout units" } } },
								{ "pitch", json{ { "type", "number" }, { "description", "drive pitch in radians" } } },
								{ "yaw", json{ { "type", "number" }, { "description", "drive yaw/heading in radians" } } },
							} },
			{ "additionalProperties", false },
		};
		return descriptor;
	}

	CameraService::CameraService(
		ToolRegistry& a_registry, bool a_allowControlActions, CameraBackend a_backend) :
		registry_(std::addressof(a_registry)),
		backend_(std::make_shared<CameraBackend>(std::move(a_backend))),
		handler_([a_allowControlActions, backend = backend_](
					 const json& a_args, const ToolContext&) {
			return HandleCamera(a_args, a_allowControlActions, *backend);
		})
	{}

	void CameraService::Register()
	{
		registry_->Register(BuildCameraDescriptor(), handler_);
	}

	void CameraService::RefreshDescriptor()
	{
		Register();
	}

	std::shared_ptr<CameraService> RegisterCameraTool(
		ToolRegistry& a_registry, bool a_allowControlActions, CameraBackend a_backend)
	{
		auto service = std::make_shared<CameraService>(
			a_registry, a_allowControlActions, std::move(a_backend));
		service->Register();
		return service;
	}
}

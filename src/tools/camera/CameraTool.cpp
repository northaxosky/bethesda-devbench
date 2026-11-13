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

		std::optional<double> ReadOptionalNumber(const json& a_args, std::string_view a_name)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return std::nullopt;
			if (!it->is_number() || !std::isfinite(it->get<double>()))
				throw ToolError(
					400, std::format("camera frame '{}' must be a finite number", a_name));
			return it->get<double>();
		}

		json HandleFrame(const json& a_args, const CameraBackend& a_backend)
		{
			ValidateOnly(a_args,
				{ "action", "formId", "side", "aroundDeg", "distance", "distanceScale",
					"eyeHeight", "pitchDeg" });
			const auto target = ReadString(a_args, "formId", true);
			if (target.empty())
				throw ToolError(400, "camera frame requires a non-empty 'formId'");
			const auto sideName = ReadString(a_args, "side", false, "front");
			const auto side = CameraFrame::ParseSide(sideName);
			if (!side)
				throw ToolError(400,
					"camera frame 'side' must be front | back | left | right | top | bottom");

			CameraFrame::Options options;
			options.side = *side;
			if (const auto v = ReadOptionalNumber(a_args, "aroundDeg"))
				options.aroundRad = *v * CameraFrame::kDegToRad;
			if (const auto v = ReadOptionalNumber(a_args, "distance"))
				options.distance = *v;
			if (const auto v = ReadOptionalNumber(a_args, "distanceScale"))
				options.distanceScale = *v;
			if (const auto v = ReadOptionalNumber(a_args, "eyeHeight"))
				options.eyeFraction = *v;
			if (const auto v = ReadOptionalNumber(a_args, "pitchDeg"))
				options.pitchRad = *v * CameraFrame::kDegToRad;
			if (const auto problem = CameraFrame::Validate(options))
				throw ToolError(400, std::format("camera frame {}", *problem));

			const auto state = ReadSnapshot(a_backend);
			if (!state.freeCam)
				throw ToolError(409, "camera frame requires active free-camera mode");
			if (!state.freeCamOwned)
				throw ToolError(409,
					"camera frame refuses a free-camera mode not owned by DevBench");
			if (!a_backend.resolveFrameTarget || !a_backend.drive)
				throw ToolError(503, "camera framing is unavailable");

			const auto resolved = a_backend.resolveFrameTarget(target);
			const auto pose = CameraFrame::Frame(resolved.place, options);
			auto       result = a_backend.drive(CameraDriveRequest{
					  .x = static_cast<float>(pose.x),
					  .y = static_cast<float>(pose.y),
					  .z = static_cast<float>(pose.z),
					  .pitch = static_cast<float>(pose.pitch),
					  .yaw = static_cast<float>(pose.yaw),
			});
			result["action"] = "frame";
			result["side"] = sideName;
			result["target"] = resolved.identity;
			result["pose"] = json{
				{ "x", pose.x },
				{ "y", pose.y },
				{ "z", pose.z },
				{ "pitch", pose.pitch },
				{ "yaw", pose.yaw },
			};
			return result;
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

			if (action == "frame")
				return HandleFrame(a_args, a_backend);

			throw ToolError(400,
				std::format(
					"unknown camera action '{}' (get|setPov|freecam|drive|frame)", action));
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
			"Read or control the native player camera. get returns the live POV, state id, "
			"free-camera ownership, and available transform fields. setPov uses the adapter's "
			"normal first/third-person transitions and its native auto-vanity entry path. "
			"freecam enters/exits the native free-camera stack without freezing time, but only "
			"exits a mode entered by DevBench. drive requires DevBench-owned free camera and "
			"sets an absolute world-unit position plus pitch/yaw radians; angles are normalized "
			"before dispatch. frame (formId of a placed reference; optional side front (default)|back|left|right|top|bottom, "
			"aroundDeg, distance, distanceScale, eyeHeight, pitchDeg) drives the already-owned free camera to look at "
			"that reference from one side and returns { side, target, pose: { x, y, z, pitch, yaw } }. front/back/left/right "
			"are relative to the reference's own heading (inspect refs rotation.z) with right on its clockwise side; top looks "
			"straight down with the image's up being its front, bottom straight up. The camera sits 1.4 x the reference's "
			"height away (distanceScale scales that, distance sets units) at eyeHeight (default 0.85) of its height; aroundDeg "
			"replaces the side's horizontal angle; pitchDeg tilts, positive looks down. A reference without bounds is framed as "
			"a 128-unit-tall target. Mutations require allowControlActions=true. A mutation that starts "
			"but misses its acknowledgement deadline has an uncertain outcome and must not be retried.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "get", "setPov", "freecam", "drive", "frame" }) }, { "default", "get" } } },
								{ "pov", json{ { "type", "string" }, { "enum", json::array({ "first", "third", "vanity" }) }, { "description", "setPov target" } } },
								{ "on", json{ { "type", "boolean" }, { "default", true }, { "description", "freecam target state" } } },
								{ "x", json{ { "type", "number" }, { "description", "drive world X in Fallout units" } } },
								{ "y", json{ { "type", "number" }, { "description", "drive world Y in Fallout units" } } },
								{ "z", json{ { "type", "number" }, { "description", "drive world Z in Fallout units" } } },
								{ "pitch", json{ { "type", "number" }, { "description", "drive pitch in radians" } } },
								{ "yaw", json{ { "type", "number" }, { "description", "drive yaw/heading in radians" } } },
								{ "formId", json{ { "type", "string" }, { "description", "frame: placed reference to look at (0x-prefixed/bare hexadecimal FormID or EditorID)" } } },
								{ "side", json{ { "type", "string" }, { "enum", json::array({ "front", "back", "left", "right", "top", "bottom" }) }, { "default", "front" }, { "description", "frame: side to look from, relative to the reference's heading" } } },
								{ "aroundDeg", json{ { "type", "number" }, { "description", "frame: degrees clockwise from the reference's front, replacing the side's horizontal angle" } } },
								{ "distance", json{ { "type", "number" }, { "exclusiveMinimum", 0 }, { "maximum", CameraFrame::kMaxDistance }, { "description", "frame: camera distance in world units (default 1.4 x the reference's height)" } } },
								{ "distanceScale", json{ { "type", "number" }, { "exclusiveMinimum", 0 }, { "maximum", CameraFrame::kMaxDistanceScale }, { "description", "frame: multiplier on the default distance (default 1)" } } },
								{ "eyeHeight", json{ { "type", "number" }, { "minimum", 0 }, { "maximum", CameraFrame::kMaxEyeFraction }, { "description", "frame: camera height as a fraction of the reference's height (default 0.85)" } } },
								{ "pitchDeg", json{ { "type", "number" }, { "minimum", -CameraFrame::kMaxPitchDeg }, { "maximum", CameraFrame::kMaxPitchDeg }, { "description", "frame: camera tilt in degrees, positive looks down (default 0; top and bottom use +-89)" } } },
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

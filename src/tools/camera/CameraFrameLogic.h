#pragma once

#include <numbers>
#include <optional>
#include <string>
#include <string_view>

namespace dvb::tools::CameraFrame
{
	enum class Side
	{
		kFront,
		kBack,
		kLeft,
		kRight,
		kTop,
		kBottom,
	};

	/// Case-sensitive; nullopt for an unknown side.
	std::optional<Side> ParseSide(std::string_view a_name);

	/// Heading is radians (inspect refs rotation.z); zero-height bounds mean unknown.
	struct Target
	{
		double x = 0;
		double y = 0;
		double z = 0;
		double heading = 0;
		double boundMinZ = 0;
		double boundMaxZ = 0;
	};

	struct Options
	{
		Side                  side = Side::kFront;
		std::optional<double> aroundRad;  ///< clockwise from the front
		std::optional<double> distance;   ///< default kDistanceFactor x height x scale
		double                distanceScale = 1.0;
		double                eyeFraction = 0.85;  ///< camera height as a fraction of the target's height
		std::optional<double> pitchRad;            ///< positive looks down
	};

	/// The pose `camera action=drive` takes.
	struct Pose
	{
		double x = 0;
		double y = 0;
		double z = 0;
		double pitch = 0;
		double yaw = 0;
	};

	inline constexpr double kDefaultHeight = 128.0;
	inline constexpr double kDistanceFactor = 1.4;
	inline constexpr double kDegToRad = std::numbers::pi / 180.0;
	inline constexpr double kMaxPitchDeg = 89.0;
	inline constexpr double kMaxPitch = kMaxPitchDeg * kDegToRad;
	inline constexpr double kMaxEyeFraction = 2.0;
	inline constexpr double kMaxDistance = 100000.0;
	inline constexpr double kMaxDistanceScale = 1000.0;

	/// The reason options cannot be framed, or nullopt if they can.
	std::optional<std::string> Validate(const Options& a_options);

	/// Yaw is clockwise from +Y, the same sense as a reference's heading.
	Pose Frame(const Target& a_target, const Options& a_options);

	/// Wrap an angle into (-pi, pi].
	double WrapPi(double a_angle);
}

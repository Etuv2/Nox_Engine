#pragma once
// Numeric helpers shared by the vehicle backend and its policies. Kept inline so both sides evaluate
// the same expressions in the same order (the parity baseline depends on it).
#include <algorithm>
#include <cmath>

namespace VehicleMath {
	constexpr float kPi = 3.14159265f;
	constexpr float kG = 9.81f;

	// Frame-rate independent exponential approach; 'rate' is already multiplied by dt.
	inline float Approach(float current, float target, float rate) {
		return current + (target - current) * (1.0f - std::exp(-rate));
	}

	inline float WrapAngle(float a) {
		while (a > kPi) a -= 2.0f * kPi;
		while (a < -kPi) a += 2.0f * kPi;
		return a;
	}

	// Simplified Pacejka lateral curve: peak near 8 deg slip, gentle fall-off when sliding.
	inline float TyreCurve(float slip) {
		const float B = 9.0f, C = 1.45f;
		return std::sin(C * std::atan(B * slip));
	}

	// Normalised engine torque curve.
	inline float TorqueCurve(float rpm, float idle, float limiter) {
		const float t = std::clamp((rpm - idle) / std::max(limiter - idle, 1.0f), 0.0f, 1.0f);
		if (t < 0.55f) return 0.72f + 0.28f * (t / 0.55f);
		return 1.0f - 0.18f * ((t - 0.55f) / 0.45f) * ((t - 0.55f) / 0.45f);
	}
}

#pragma once
// NFS-2015-style driving policy extracted from DriveMode (legacy racing library, NoxRacingLegacy).
// Everything here is game policy: nitrous, drift state machine, steering and yaw assists, drift
// velocity swing, direction and automatic gear selection. The numeric defaults in Handling are the
// NFS handling presets used when a scene does not override them; the native backend only receives
// the numeric values through NativeTuning. Transitional: deleted once project scripts own the policy.
#include <vector>
#include "json.hpp"
#include "vehicle/VehiclePolicy.h"
#include "vehicle/VehicleTypes.h"

class LegacyNfsPolicy final : public IVehiclePolicy {
public:
	struct Handling {
		float topSpeedKmh = 250.0f;
		float acceleration = 9.0f;      // m/s^2 drive capacity in first gear (before traction)
		float braking = 12.5f;          // m/s^2 at full brake
		float reverseMaxKmh = 40.0f;
		float mass = 1380.0f;           // kg
		float cgHeight = 0.48f;         // m
		float tireGrip = 1.2f;          // lateral friction coefficient
		float tireGripLong = 1.25f;     // longitudinal friction coefficient (traction / braking)
		float steerLockLow = 28.0f;     // deg at standstill
		float steerLockHigh = 4.5f;     // deg at top speed
		float steerRate = 6.0f;         // 1/s
		float driftGripScale = 0.58f;   // rear lateral grip while drifting
		float driftAssist = 0.65f;      // 0..1 holds the drift angle chosen by the steering
		float maxDriftAngle = 40.0f;    // deg
		float countersteerAssist = 0.5f;// 0..1 automatic countersteer on rear slip
		float yawDamping = 0.6f;        // stability when not steering
		float nitroThrust = 1.45f;      // drive force multiplier
		float nitroTopSpeed = 1.1f;     // top speed multiplier
		float nitroDrain = 0.24f;       // tank per second
		float nitroRecharge = 0.06f;    // tank per second
		float nitroRechargeDelay = 1.5f;// seconds after use
		float rpmIdle = 900.0f;
		float rpmRedline = 7800.0f;
		float rpmLimiter = 8200.0f;
		float shiftTime = 0.16f;
		// Gear ratios, first to top gear (2..8 gears). Only the relative spacing matters: the final
		// drive is derived so the top gear reaches the limiter just above topSpeedKmh.
		std::vector<float> gearRatios = { 3.68f, 2.42f, 1.81f, 1.42f, 1.12f, 0.82f };
		float reverseEngageDelay = 0.2f; // s of brake held at standstill before reverse engages (and throttle to leave it)
		float suspensionFreq = 1.6f;    // Hz (body on springs)
		float suspensionDamping = 0.55f;// damping ratio
		float bumpAmplitude = 0.012f;   // m procedural road roughness
		float roofHeight = 1.6f;        // m above the body base
		// Legacy editor fields (pre-rework handling model); unused by the dynamics.
		float grip = 0.0f, driftGrip = 0.0f, driftYawGain = 0.0f;
	};

	static void ReadHandling(const nlohmann::json& j, Handling& h);
	// Numeric subset consumed by the native backend (values copied verbatim).
	static VehicleTuning NativeTuning(const Handling& h);

	Handling& MutableHandling() { return m_handling; }
	const Handling& GetHandling() const { return m_handling; }
	int GearCount() const { return static_cast<int>(m_handling.gearRatios.size()); }
	void Refuel() { m_nitro = 1.0f; m_nitroCooldown = 0.0f; }
	bool ConsumeShift() { const bool s = m_shifted; m_shifted = false; return s; }

	void OnTeleport() override;
	void OnRespawn() override;
	void OnHold() override;
	void Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out) override;
	void YawAssist(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleYawAssist& io) override;
	void VelocityCorrection(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleVelocity& io) override;
	void PostStep(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleGearRequest& io) override;
	VehiclePolicyDebug Debug() const override;

private:
	float GearRatio(int gear) const;

	Handling m_handling;
	float m_nitro = 1.0f;
	float m_nitroCooldown = 0.0f;
	bool m_nitroActive = false;
	float m_drift = 0.0f;                // 0..1 drift state (grip blend)
	float m_driftSign = 1.0f;
	float m_prevBeta = 0.0f;
	float m_brakeTapTimer = 0.0f;
	float m_driftExitTimer = 0.0f;
	float m_steer = 0.0f;                // smoothed input [-1,1], right positive
	float m_shiftTimer = 0.0f;
	float m_shiftCooldown = 0.0f;        // minimum time between automatic shifts (no gear hunting)
	float m_directionHold = 0.0f;        // time the direction-change pedal has been held at standstill
	bool m_shifted = false;
	bool m_burnout = false;              // burnout decided in Control, applied in VelocityCorrection
};

#pragma once
// Policy interface for the vehicle backend. A policy owns every decision (boosts, drift, steering
// assists, gear selection and shift timing); the backend only evaluates tyres, integration,
// collision, suspension, wheels, rig posing and the mechanical gearbox. Policies are called once per
// fixed sub-step, in this order:
//
//   Control            -> VehicleActuation        (before force evaluation)
//   YawAssist          -> yawAcc, ax              (after native forces and accelerations)
//   VelocityCorrection -> vx, vy                  (after native integration and settle; burnout hold, drift swing)
//   PostStep           -> gear, gearbox shifting  (after position update; before the rpm math)
//
// A policy receives the same telemetry snapshot in every call and may keep its own state; it must
// not reach into the backend. OnTeleport resets the policy state that a teleport clears.
#include <map>
#include <string>
#include "VehicleTypes.h"

// Per-phase snapshot. Derived values are computed by the backend from the state at the start of the
// sub-step (speed, slip angle, ...) and refreshed where a later phase needs post-integration values.
struct VehicleStepTelemetry {
	float time = 0.0f;              // simulated seconds (after this sub-step's increment)
	float h = 0.0f;                 // sub-step length (s)
	glm::vec2 pos{ 0.0f }, vel{ 0.0f };
	float heading = 0.0f;
	float vx = 0.0f, vy = 0.0f, yawRate = 0.0f;
	float ax = 0.0f, ay = 0.0f;
	float steerAngle = 0.0f;        // applied front angle (valid after Control)
	float speed = 0.0f;             // |vel| at the start of the sub-step
	float speed01 = 0.0f;           // speed / top speed, clamped 0..1
	float beta = 0.0f;              // body slip angle (rad), + = velocity left of the nose
	float vxAbs = 0.0f;             // max(|vx|, 0.5)
	float wheelbase = 0.0f;         // front + rear axle distance
	float track = 0.0f;
	float topSpeed = 0.0f;          // m/s
	float rearSpin = 0.0f;
	float rpm = 0.0f;
	float wheelSpeed = 0.0f;        // gearbox speed: |vx| + rear spin term (valid in PostStep)
	int gear = 1;                   // current gear before the sub-step's selection
	bool limiter = false;           // rev limiter state from the previous sub-step
	bool airborne = false;
	bool surface = false;
	float groundPitch = 0.0f, groundRoll = 0.0f;
	int gearCount = 0;
	float rpmIdle = 0.0f;
	float finalDrive = 0.0f;        // mechanical: engine rpm per (rear wheel speed x ratio) factor
	float rearWheelRadius = 0.0f;   // m
};

// Output of Control. Initialised by the backend to the defaults below, then overwritten by the policy.
// The policy decides every pedal mapping and every special case (reverse pedal, burnout,
// brake split, handbrake lock, rest hold). The backend only applies the forces they request.
struct VehicleActuation {
	int gear = 1;                   // selected gear after direction selection (-1 = reverse)
	float steerAngle = 0.0f;        // front wheel angle (rad, left positive)
	float driveScale = 1.0f;        // multiplies the drive force (1 = none)
	bool driveShiftCut = false;     // torque interruption during a shift (drive force x 0.12)
	float dragScale = 1.0f;         // multiplies the aerodynamic/drag term (top speed factor)
	float rearGripScale = 1.0f;     // rear lateral grip multiplier (drift blend)
	// Pedals and brakes (mechanics: the backend clamps brake force to the friction limit).
	float driveThrottle = 0.0f;     // forward drive pedal 0..1, used in forward gears
	float reverseDrive = 0.0f;      // reverse drive pedal 0..1, used in reverse (backward force x 0.45)
	float brakeCommand = 0.0f;      // brake pedal 0..1 after direction mapping (force = pedal x brakeDecel x mass)
	float brakeFrontShare = 0.68f;  // fraction of the brake force on the front axle
	float brakeRearShare = 0.32f;   // fraction of the brake force on the rear axle
	bool brakeAtStandstill = false; // brake also below the rolling threshold (burnout hold)
	bool rearDriveCut = false;      // no rear drive force (burnout)
	float rearSpinFloor = 0.0f;     // minimum rear spin target 0..1 (burnout smoke)
	bool rearLock = false;          // handbrake: rear wheels locked; sliding force while moving above 0.5 m/s
	bool holdAtRest = false;        // pedals released: settle to rest below 0.3 m/s
};

struct VehicleYawAssist {           // in/out: absolute values after the policy's additions
	float yawAcc = 0.0f;
	float ax = 0.0f;
};

struct VehicleVelocity {            // in/out local velocity
	float vx = 0.0f;
	float vy = 0.0f;
};

struct VehicleGearRequest {         // in/out
	int gear = 1;                   // gear after the shift decision
	bool gearboxShifting = false;   // shift timer was running before this sub-step's decrement
};

// Policy state exposed to telemetry and the facade (read-only). The backend only reads shiftTimer;
// every other value is the policy's own: named channels (snake_case, written by the policy, 1/0 for flags).
struct VehiclePolicyDebug {
	float shiftTimer = 0.0f;
	std::map<std::string, float> channels;
	float Channel(const std::string& name, float fallback = 0.0f) const {
		const auto it = channels.find(name);
		return it == channels.end() ? fallback : it->second;
	}
};

class IVehiclePolicy {
public:
	virtual ~IVehiclePolicy() = default;
	virtual void OnTeleport() = 0;          // any teleport / recovery placement
	virtual void OnRespawn() {}             // ResetToSpawn (after OnTeleport)
	virtual void OnHold() {}                // frozen frame: no motion, engine revs
	virtual void Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out) = 0;
	virtual void YawAssist(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleYawAssist& io) = 0;
	virtual void VelocityCorrection(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleVelocity& io) = 0;
	virtual void PostStep(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleGearRequest& io) = 0;
	virtual VehiclePolicyDebug Debug() const = 0;
};

#include "LegacyNfsPolicy.h"
#include "vehicle/VehicleMath.h"

#include <algorithm>
#include <cmath>

using nlohmann::json;
using namespace VehicleMath;

// ---------------------------------------------------------------------------------------------
// Tuning (moved verbatim from DriveMode)
// ---------------------------------------------------------------------------------------------
void LegacyNfsPolicy::ReadHandling(const json& j, Handling& h)
{
	if (!j.is_object()) return;
	h.topSpeedKmh = j.value("top_speed_kmh", h.topSpeedKmh);
	h.acceleration = j.value("drive_accel", h.acceleration);
	h.braking = j.value("brake_decel", h.braking);
	h.reverseMaxKmh = j.value("reverse_max_kmh", h.reverseMaxKmh);
	h.mass = j.value("mass", h.mass);
	h.cgHeight = j.value("cg_height", h.cgHeight);
	h.tireGrip = j.value("tire_grip", h.tireGrip);
	h.tireGripLong = j.value("tire_grip_long", h.tireGripLong);
	h.steerLockLow = j.value("steer_lock_low", h.steerLockLow);
	h.steerLockHigh = j.value("steer_lock_high", h.steerLockHigh);
	h.steerRate = j.value("steer_rate_hz", h.steerRate);
	h.driftGripScale = j.value("drift_grip_scale", h.driftGripScale);
	h.driftAssist = j.value("drift_assist", h.driftAssist);
	h.maxDriftAngle = j.value("max_drift_angle", h.maxDriftAngle);
	h.countersteerAssist = j.value("countersteer_assist", h.countersteerAssist);
	h.yawDamping = j.value("yaw_damping", h.yawDamping);
	h.nitroThrust = j.value("nitro_thrust", h.nitroThrust);
	h.nitroTopSpeed = j.value("nitro_top_speed", h.nitroTopSpeed);
	h.nitroDrain = j.value("nitro_drain", h.nitroDrain);
	h.nitroRecharge = j.value("nitro_recharge", h.nitroRecharge);
	h.nitroRechargeDelay = j.value("nitro_recharge_delay", h.nitroRechargeDelay);
	h.rpmIdle = j.value("rpm_idle", h.rpmIdle);
	h.rpmRedline = j.value("rpm_redline", h.rpmRedline);
	h.rpmLimiter = j.value("rpm_limiter", h.rpmLimiter);
	h.shiftTime = j.value("shift_time", h.shiftTime);
	h.reverseEngageDelay = j.value("reverse_engage_delay", h.reverseEngageDelay);
	if (j.contains("gear_ratios") && j["gear_ratios"].is_array()) {
		std::vector<float> ratios;
		for (const json& r : j["gear_ratios"]) {
			if (r.is_number() && r.get<float>() > 0.05f) ratios.push_back(r.get<float>());
		}
		// Ratios must fall from first to top gear; anything else keeps the defaults.
		const bool descending = std::is_sorted(ratios.rbegin(), ratios.rend());
		if (ratios.size() >= 2 && ratios.size() <= 8 && descending) h.gearRatios = ratios;
	}
	h.suspensionFreq = j.value("suspension_freq", h.suspensionFreq);
	h.suspensionDamping = j.value("suspension_damping", h.suspensionDamping);
	h.bumpAmplitude = j.value("bump_amplitude", h.bumpAmplitude);
	h.roofHeight = j.value("roof_height", h.roofHeight);
}

VehicleTuning LegacyNfsPolicy::NativeTuning(const Handling& h)
{
	VehicleTuning t;
	t.topSpeedKmh = h.topSpeedKmh;
	t.driveAcceleration = h.acceleration;
	t.brakeDecel = h.braking;
	t.reverseMaxKmh = h.reverseMaxKmh;
	t.mass = h.mass;
	t.cgHeight = h.cgHeight;
	t.tireGrip = h.tireGrip;
	t.tireGripLong = h.tireGripLong;
	t.rpmIdle = h.rpmIdle;
	t.rpmLimiter = h.rpmLimiter;
	t.gearRatios = h.gearRatios;
	t.suspensionFreq = h.suspensionFreq;
	t.suspensionDamping = h.suspensionDamping;
	t.bumpAmplitude = h.bumpAmplitude;
	t.roofHeight = h.roofHeight;
	return t;
}

float LegacyNfsPolicy::GearRatio(int gear) const
{
	const std::vector<float>& r = m_handling.gearRatios;
	return r[static_cast<size_t>(std::clamp(gear, 1, static_cast<int>(r.size())) - 1)];
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------
void LegacyNfsPolicy::OnTeleport()
{
	m_steer = 0.0f;
	m_drift = 0.0f;
	m_shiftTimer = 0.0f;
	m_shiftCooldown = 0.0f;
	m_directionHold = 0.0f;
}

void LegacyNfsPolicy::OnRespawn()
{
	Refuel();
}

void LegacyNfsPolicy::OnHold()
{
	m_nitroActive = false;
}

VehiclePolicyDebug LegacyNfsPolicy::Debug() const
{
	VehiclePolicyDebug d;
	d.nitro = m_nitro;
	d.nitroCooldown = m_nitroCooldown;
	d.nitroActive = m_nitroActive;
	d.drift = m_drift;
	d.driftSign = m_driftSign;
	d.shiftTimer = m_shiftTimer;
	return d;
}

// ---------------------------------------------------------------------------------------------
// Control: nitrous, drift state machine, steering, direction selection
// (from DriveMode::Step, before the loads and force evaluation)
// ---------------------------------------------------------------------------------------------
void LegacyNfsPolicy::Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out)
{
	const Handling& hd = m_handling;
	const float h = t.h;
	const float speed = t.speed;
	const float speed01 = t.speed01;
	const float beta = t.beta;

	// --- Nitrous.
	m_nitroActive = in.nitrous && m_nitro > 0.0f && in.throttle > 0.05f && t.vx > 1.0f;
	if (m_nitroActive) {
		m_nitro = std::max(0.0f, m_nitro - hd.nitroDrain * h);
		m_nitroCooldown = hd.nitroRechargeDelay;
	}
	else {
		m_nitroCooldown = std::max(0.0f, m_nitroCooldown - h);
		if (m_nitroCooldown <= 0.0f) m_nitro = std::min(1.0f, m_nitro + hd.nitroRecharge * h);
	}

	// --- Drift state machine (NFS 2015: handbrake or brake-tap entry, power-over, hold, recover).
	// Brake tap: a press shorter than 0.4 s arms drift entry for 0.45 s after release.
	if (in.brake > 0.2f) m_brakeTapTimer = std::max(m_brakeTapTimer, 0.0f) + h;
	else if (m_brakeTapTimer > 0.0f) m_brakeTapTimer = m_brakeTapTimer < 0.4f ? -0.45f : 0.0f;
	else m_brakeTapTimer = std::min(0.0f, m_brakeTapTimer + h);
	const bool brakeTapped = m_brakeTapTimer < 0.0f && m_brakeTapTimer > -0.45f; // released recently after a short press
	// Drift direction is latched at entry from the steering (right steer = nose right of velocity =
	// positive body slip) so the assist never flips sides mid-slide.
	const float steerDir = in.steer >= 0.0f ? 1.0f : -1.0f;
	const bool powerOver = std::abs(beta) > 0.22f && in.throttle > 0.7f && beta * steerDir > 0.0f;
	const bool wantsDrift = speed > 11.0f && std::abs(in.steer) > 0.35f && (in.handbrake || (brakeTapped && in.throttle > 0.3f) || powerOver);
	if (wantsDrift) {
		if (m_drift <= 0.0f) m_driftSign = steerDir;
		m_drift = std::min(1.0f, m_drift + h * 6.0f);
	}
	const float driftDir = m_drift > 0.0f ? m_driftSign : (beta >= 0.0f ? 1.0f : -1.0f);
	if (m_drift > 0.0f) {
		const bool straightening = in.steer * -driftDir < -0.6f;    // countersteering hard out of it
		const bool small = std::abs(beta) < glm::radians(9.0f);
		const bool released = in.steer * driftDir < 0.15f && std::abs(beta) < glm::radians(18.0f);
		m_driftExitTimer = (small || straightening || released || speed < 7.0f || in.throttle < 0.05f) ? m_driftExitTimer + h : 0.0f;
		if (m_driftExitTimer > 0.2f && !in.handbrake) m_drift = std::max(0.0f, m_drift - h * 3.0f);
	}

	// --- Steering: speed-sensitive lock, smoothed; countersteer assist points the front wheels
	// toward the velocity so slides are catchable.
	m_steer = Approach(m_steer, in.steer, h * (std::abs(in.steer) > std::abs(m_steer) ? hd.steerRate : hd.steerRate * 1.6f));
	const float lock = glm::radians(glm::mix(hd.steerLockLow, hd.steerLockHigh, std::pow(speed01, 0.5f)));
	float delta = -m_steer * lock; // left positive
	const float assist = hd.countersteerAssist * (1.0f - 0.5f * m_drift);
	if (speed > 4.0f) delta += assist * std::clamp(beta, -0.6f, 0.6f) * 0.8f;
	delta = std::clamp(delta, -glm::radians(hd.steerLockLow + 10.0f), glm::radians(hd.steerLockLow + 10.0f));
	out.steerAngle = delta;

	// --- Direction (pedal-driven, NFS style): hold brake at a standstill to engage reverse; in reverse
	// the brake pedal drives backwards and the throttle brakes, held at a standstill it selects first again.
	// Only a deliberate hold at (near) zero speed changes direction, so a crash bounce or a brake held
	// through a stop never flips the car into reverse on its own.
	int gear = t.gear;
	const bool stopped = std::abs(t.vx) < 1.0f;
	const bool wantReverse = gear >= 1 && in.brake > 0.1f && in.throttle < 0.1f;
	const bool wantForward = gear == -1 && in.throttle > 0.1f && in.brake < 0.1f;
	if (stopped && (wantReverse || wantForward)) {
		m_directionHold += h;
		if (m_directionHold >= hd.reverseEngageDelay) {
			gear = wantReverse ? -1 : 1;
			m_directionHold = 0.0f;
			m_shiftCooldown = 0.3f;
			m_shifted = true;
		}
	}
	else {
		m_directionHold = 0.0f;
	}
	// Rolling forward under its own speed while in reverse (or after a teleport): back to a forward gear.
	if (gear == -1 && t.vx > 3.0f) gear = 1;
	if (gear == 0 || gear > GearCount()) gear = std::clamp(gear, 1, GearCount());
	// At a standstill a forward gear is always first (e.g. after stopping hard against a wall).
	if (gear > 1 && stopped && m_shiftTimer <= 0.0f) gear = 1;
	out.gear = gear;

	// --- Actuation for the native force evaluation.
	out.nitroDriveScale = m_nitroActive ? hd.nitroThrust : 1.0f;
	out.driveShiftCut = m_shiftTimer > 0.0f;
	out.dragScale = m_nitroActive ? 1.0f / (hd.nitroTopSpeed * hd.nitroTopSpeed) : 1.0f;
	out.rearGripScale = glm::mix(1.0f, hd.driftGripScale, m_drift);

	// --- Pedals, burnout and brakes (NFS policy; the backend applies the forces).
	// Burnout: both pedals held at (near) standstill. Front brakes hold the car, the rear spins up.
	m_burnout = in.throttle > 0.5f && in.brake > 0.5f && std::abs(t.vx) < 3.0f;
	out.driveThrottle = in.throttle;
	out.reverseDrive = in.brake > 0.1f ? in.brake : 0.0f;    // reverse: the brake pedal drives backwards
	out.brakeCommand = gear == -1 ? in.throttle : in.brake;  // brake pedal: S in forward gears, W in reverse
	out.brakeAtStandstill = m_burnout;
	out.brakeFrontShare = 0.68f;
	out.brakeRearShare = m_burnout ? 0.0f : 0.32f;
	out.rearDriveCut = m_burnout;
	out.rearSpinFloor = m_burnout ? in.throttle : 0.0f;
	out.rearLock = in.handbrake;                              // handbrake: rear wheels locked
	out.holdAtRest = in.throttle == 0.0f && in.brake == 0.0f; // pedals released: the car holds still
}

// ---------------------------------------------------------------------------------------------
// Yaw assist: drift hold, slip-rate damping, spin limit, grip stability, yaw damping
// (from DriveMode::Step, after the native force evaluation; same additions in the same order)
// ---------------------------------------------------------------------------------------------
void LegacyNfsPolicy::YawAssist(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleYawAssist& io)
{
	const Handling& hd = m_handling;
	const float h = t.h;
	const float speed = t.speed;
	const float beta = t.beta;
	const float delta = t.steerAngle;
	const float L = t.wheelbase;
	const float vx = t.vx;
	const float yawRate = t.yawRate;
	float ax = io.ax;
	float yawAcc = io.yawAcc;
	const float driftDir = m_drift > 0.0f ? m_driftSign : (beta >= 0.0f ? 1.0f : -1.0f);

	// Drift assist: hold the drift angle selected by the steering, keep speed through the drift.
	if (m_drift > 0.01f && speed > 6.0f) {
		const float toward = std::clamp(-in.steer * -driftDir, -1.0f, 1.0f); // + = steering into the drift
		// Steering into the drift holds/tightens it; neutral or countersteer straightens out.
		const float target = driftDir * glm::radians(hd.maxDriftAngle) * std::clamp(0.1f + 0.9f * toward, 0.0f, 1.0f);
		yawAcc += -hd.driftAssist * m_drift * 9.0f * (target - beta);
		// Steering released or countersteered: kill the rotation so the car straightens crisply.
		if (toward < 0.15f) yawAcc -= 3.5f * m_drift * (yawRate - vx * std::tan(delta) / L);
		ax += m_drift * hd.driftAssist * in.throttle * 2.2f * std::cos(beta);
	}
	// Slip-rate damping: no pendulum overshoot when the rear steps out (strongest in drifts).
	const float betaRate = (beta - m_prevBeta) / h;
	m_prevBeta = beta;
	if (speed > 5.0f) yawAcc += (0.6f + 2.4f * m_drift * hd.driftAssist) * std::clamp(betaRate, -6.0f, 6.0f);
	// Never spin out (drift or grip): strong correction toward the velocity past the limit.
	{
		const float limit = glm::radians(hd.maxDriftAngle + 12.0f);
		if (speed > 5.0f && std::abs(beta) > limit) yawAcc += 25.0f * (std::abs(beta) - limit) * (beta > 0.0f ? 1.0f : -1.0f) - 2.0f * yawRate;
	}
	// Grip-mode stability (arcade ESC): catch unintended oversteer outside drift mode.
	if (speed > 5.0f) yawAcc += 14.0f * (1.0f - m_drift) * std::clamp(beta, -0.5f, 0.5f) * std::clamp((std::abs(beta) - 0.03f) / 0.08f, 0.0f, 1.0f);
	// Yaw damping in grip driving when the steering is centred.
	if (m_drift < 0.2f && std::abs(in.steer) < 0.1f) yawAcc -= hd.yawDamping * 4.0f * (yawRate - vx * std::tan(delta) / L);

	io.yawAcc = yawAcc;
	io.ax = ax;
}

// ---------------------------------------------------------------------------------------------
// Velocity correction: arcade drift turn (NFS 2015). While drifting, the velocity swings toward the
// nose at a fixed rate (speed preserved).
// ---------------------------------------------------------------------------------------------
void LegacyNfsPolicy::VelocityCorrection(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleVelocity& io)
{
	const Handling& hd = m_handling;
	float vx = io.vx;
	float vy = io.vy;
	// Burnout hold: the car is braked toward rest while both pedals are held.
	if (m_burnout) vx = VehicleMath::Approach(vx, 0.0f, t.h * 8.0f);
	if (m_drift > 0.01f && vx > 6.0f) {
		const float sp = std::sqrt(vx * vx + vy * vy);
		const float b = std::atan2(vy, vx);
		const float steerInto = std::clamp(in.steer * m_driftSign, 0.0f, 1.0f);
		const float rate = hd.driftAssist * m_drift * (1.1f + 0.9f * in.throttle) * (0.25f + 0.75f * steerInto);
		const float nb = b - std::copysign(std::min(std::abs(b), rate * t.h), b);
		vx = sp * std::cos(nb);
		vy = sp * std::sin(nb);
	}
	io.vx = vx;
	io.vy = vy;
}

// ---------------------------------------------------------------------------------------------
// Gearbox decision (from DriveMode::UpdateGearbox): shift timers and automatic up/down shifts.
// The mechanical ratio/rpm math stays native and runs after this call.
// ---------------------------------------------------------------------------------------------
void LegacyNfsPolicy::PostStep(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleGearRequest& io)
{
	const Handling& hd = m_handling;
	const float h = t.h;
	const float wheelSpeed = t.wheelSpeed;
	const int gears = GearCount();
	int gear = io.gear;
	const bool shifting = m_shiftTimer > 0.0f;
	io.gearboxShifting = shifting;
	m_shiftTimer = std::max(0.0f, m_shiftTimer - h);
	m_shiftCooldown = std::max(0.0f, m_shiftCooldown - h);
	auto rpmFor = [&](int g, float v) {
		return std::abs(v) / t.rearWheelRadius * 60.0f / (2.0f * kPi) * GearRatio(g) * t.finalDrive;
	};
	auto engineRpm = [&](int g) { return hd.rpmIdle + rpmFor(g, wheelSpeed); };

	// Automatic shifting (forward gears only):
	//   up        at the redline under throttle (not while the rear tyres are spinning up)
	//   kickdown  floored throttle while the lower gear still has room below ~80% of the redline
	//   braking   step down as speed falls, keeping the engine in its band for the exit
	//   lugging   part throttle / coasting below ~2600 rpm
	// A short lockout after every shift prevents hunting between two gears.
	if (gear >= 1 && !shifting && m_shiftCooldown <= 0.0f) {
		const float rpmNow = engineRpm(gear);
		const float lowerRpm = gear > 1 ? engineRpm(gear - 1) : 1e9f;
		const bool up = gear < gears && in.throttle > 0.1f && t.vx > 3.0f && t.rearSpin < 0.5f &&
			(rpmNow >= hd.rpmRedline || t.limiter);
		const bool kickdown = in.throttle > 0.9f && lowerRpm < hd.rpmRedline * 0.8f;
		const bool braking = in.brake > 0.1f && t.vx > 0.3f && lowerRpm < hd.rpmRedline * 0.7f;
		const bool lugging = in.throttle < 0.9f && rpmNow < 2600.0f && lowerRpm < hd.rpmRedline * 0.75f;
		if (up) {
			++gear;
			m_shiftTimer = hd.shiftTime;
			m_shiftCooldown = 0.35f;
			m_shifted = true;
		}
		else if (gear > 1 && (kickdown || braking || lugging)) {
			--gear;
			m_shiftTimer = hd.shiftTime * 0.6f;
			m_shiftCooldown = 0.3f;
			m_shifted = true;
		}
	}
	io.gear = gear;
}

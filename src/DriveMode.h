#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "json.hpp"

class Camera;
class SceneGraph;
class SceneNode;
class RoadNetwork;

/*
* Drive mode (docs/nfs2015/game.md): arcade vehicle + chase camera for the racing prototype.
*
*   * Vehicle dynamics (fixed 240 Hz sub-steps): planar two-axle model with combined-slip tyres
*     (simplified Pacejka lateral curve + friction circle), longitudinal and lateral load transfer,
*     rear-wheel drive through a 6-speed box with traction-limited drive force (excess torque becomes
*     wheelspin: burnouts), brakes, handbrake, drag and rolling resistance. NFS-2015-style aids:
*     speed-sensitive steering, countersteer assist, drift mode (handbrake or brake-tap entry, held
*     drift angle with throttle, smooth grip recovery) and yaw damping.
*   * Suspension: 3-DOF sprung body (heave, pitch, roll) driven by load transfer and the road
*     surface under each wheel (procedural bumps, curbs); wheels stay on the ground.
*   * Rig: if the body model has glTF nodes Body / Wheel_FL / Wheel_FR / Wheel_RL / Wheel_RR (see
*     models/car_r33_rig.glb), the body is sprung and the wheels spin with their rolling speed, steer
*     with the front axle and follow the road height. Older models fall back to body roll only.
*   * Collision: RoadNetwork street band (building lines are walls) + layout obstacles; the car is
*     two circles, collisions are resolved per sub-step.
*   * Chase camera: matched to the NFS 2015 reference framing (height ~1.65 m, ~5 m behind the car
*     centre, ~3.5 deg down, 55 deg vertical FOV at rest), critically damped follow that blends the
*     car heading with the velocity direction (drift framing) and restrained speed effects.
*
* Tuning lives in the scene's "game" object ("handling", "camera", "spawn").
*/
class DriveMode {
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
		// drive is derived so the top gear reaches the limiter just above topSpeedKmh. The default
		// spacing is arcade-close so city straights reach 4th/5th and the top gear at top speed.
		std::vector<float> gearRatios = { 3.68f, 2.42f, 1.81f, 1.42f, 1.12f, 0.82f };
		float reverseEngageDelay = 0.2f; // s of brake held at standstill before reverse engages (and throttle to leave it)
		float suspensionFreq = 1.6f;    // Hz (body on springs)
		float suspensionDamping = 0.55f;// damping ratio
		float bumpAmplitude = 0.012f;   // m procedural road roughness
		// Legacy editor fields (pre-rework handling model); unused by the dynamics.
		float grip = 0.0f, driftGrip = 0.0f, driftYawGain = 0.0f;
	};
	struct CameraRig {
		float distance = 5.0f;          // m behind the car centre
		float height = 1.65f;           // m above the road
		float lookAhead = 10.0f;        // m ahead of the car centre
		float lookHeight = 0.7f;        // m (with height/distance this gives ~3.5 deg down)
		float fov = 55.0f;              // vertical degrees at rest
		float fovSpeedGain = 5.0f;      // restrained
		float fovNitro = 4.0f;
		float pullBack = 0.6f;          // extra distance at top speed
		float stiffness = 9.0f;
		float headingLag = 5.0f;
		float velocityFollow = 0.45f;   // 0 = follow heading, 1 = follow velocity (drift framing)
	};
	struct Input {
		float throttle = 0, brake = 0, steer = 0;
		bool handbrake = false, nitrous = false;
	};
	// Per-wheel state for effects (smoke/spray), audio and debugging. Order: FL, FR, RL, RR.
	struct WheelState {
		glm::vec3 contact{ 0.0f };      // world contact point
		glm::vec3 velocity{ 0.0f };     // world velocity of the contact patch (car motion)
		float slip = 0.0f;              // 0..1 combined slip (lateral + longitudinal)
		float wheelspin = 0.0f;         // 0..1 driven-wheel overspeed
		float load = 0.0f;              // N
		bool grounded = true;
		float spinAngle = 0.0f;
		float steerAngle = 0.0f;        // rad, left positive
		float suspension = 0.0f;        // m compression relative to rest
	};

	void Configure(const nlohmann::json& game, SceneGraph& sceneGraph, const RoadNetwork* roads,
		const std::string& layoutPath);
	bool IsAvailable() const { return m_available; }
	bool IsActive() const { return m_active; }
	void SetActive(bool active);
	void CycleCamera() { m_cameraMode = (m_cameraMode + 1) % 4; }
	int CameraMode() const { return m_cameraMode; }
	void ResetToSpawn();
	void Teleport(const glm::vec2& pos, float heading);
	void RecoverToRoad();
	bool ConsumeTeleport() { const bool t = m_teleported; m_teleported = false; return t; }
	bool ConsumeTeleportForEffects() { const bool t = m_teleportFx; m_teleportFx = false; return t; }

	// Simulates the car (frozen: no motion, e.g. countdown/menus) and poses the camera.
	void Update(float dt, const Input& in, bool frozen, Camera* camera, float aspect);

	// State for HUD / race / audio / effects.
	glm::vec2 Position() const { return m_pos; }
	glm::vec2 PreviousPosition() const { return m_prevPos; }
	float Heading() const { return m_heading; }
	float SpeedKmh() const { return glm::length(m_vel) * 3.6f; }
	float SpeedMs() const { return m_vx; }                 // signed forward speed
	float Speed01() const;
	float Rpm() const { return m_rpm; }
	int Gear() const { return m_gear; }                   // -1 = R, 1..GearCount()
	int GearCount() const { return static_cast<int>(m_handling.gearRatios.size()); }
	float Nitro() const { return m_nitro; }
	bool NitroActive() const { return m_nitroActive; }
	bool NitroRecharging() const { return !m_nitroActive && m_nitro < 1.0f && m_nitroCooldown <= 0.0f; }
	float Load() const { return m_load; }
	float Throttle() const { return m_throttle; }
	float Slip01() const { return m_slip; }
	bool Drifting() const { return m_drift > 0.5f; }
	float DriftAngleDeg() const;
	bool AtRedline() const { return m_rpm >= m_handling.rpmRedline; }
	bool LimiterHit() const { return m_limiter; }
	bool ConsumeShift() { const bool s = m_shifted; m_shifted = false; return s; }
	bool HasCollided() const { return m_impact > 0.0f; }
	const WheelState& Wheel(int i) const { return m_wheels[i]; }
	bool HasWheelRig() const { return m_rigWheels; }

	Handling& MutableHandling() { return m_handling; }
	CameraRig& MutableCamera() { return m_rig; }
	glm::vec2 SpawnPosition() const { return m_spawnPos; }
	float SpawnHeading() const { return m_spawnHeading; }
	void SetSpawn(const glm::vec2& p, float heading) { m_spawnPos = p; m_spawnHeading = heading; }
	nlohmann::json SaveTuning() const; // "handling" + "camera" objects

	static void ReadHandling(const nlohmann::json& j, Handling& h);
	static void ReadCamera(const nlohmann::json& j, CameraRig& c);

	// Road surface height (procedural bumps on asphalt, curbs/sidewalks).
	float GroundHeight(const glm::vec2& p) const;

private:
	struct Area { glm::vec2 min, max; };
	struct Obstacle { bool box = false; glm::vec2 center{ 0.0f }; glm::vec2 half{ 0.0f }; float yaw = 0.0f; float radius = 0.0f; };
	struct RigNode {
		std::weak_ptr<SceneNode> node;
		glm::vec3 restPos{ 0.0f };
		glm::quat restRot{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 restScale{ 1.0f };
	};

	void Step(float h, const Input& in);
	void SelectDirection(float h, const Input& in);       // forward / reverse from the pedals at standstill
	void UpdateGearbox(float h, const Input& in, float wheelRpm);
	float GearRatio(int gear) const;                      // 1-based, clamped
	void UpdateSuspension(float h);
	void Collide(glm::vec2& pos, glm::vec2& vel);
	glm::vec2 ResolveCircle(glm::vec2& c, float radius) const;
	void ApplyTransforms();
	void UpdateCamera(float dt, Camera& camera, float aspect);
	void LoadLayout(const std::string& path);

	bool m_available = false;
	bool m_active = false;
	std::weak_ptr<SceneNode> m_player;
	RigNode m_model;                     // scene body node (whole car model)
	RigNode m_bodyMesh;                  // glTF "Body" (sprung)
	std::array<RigNode, 4> m_wheelNodes;
	bool m_rigWheels = false;
	float m_wheelRadius[4] = { 0.33f, 0.33f, 0.345f, 0.345f };
	glm::vec3 m_wheelRest[4];            // hub positions in car space (x left, y up, z forward)
	float m_playerY = 0.0f;
	std::vector<Area> m_areas;
	std::vector<Obstacle> m_obstacles;
	const RoadNetwork* m_roads = nullptr;
	Handling m_handling;
	CameraRig m_rig;

	// Geometry (from the rig when present).
	float m_a = 1.36f;                   // CG to front axle
	float m_b = 1.36f;                   // CG to rear axle
	float m_track = 1.63f;

	// Vehicle state (metres, seconds, radians). Heading 0 = +Z, increases toward +X (the car's left).
	glm::vec2 m_pos{ 0.0f };
	glm::vec2 m_prevPos{ 0.0f };
	glm::vec2 m_spawnPos{ 0.0f };
	float m_spawnHeading = 0.0f;
	float m_heading = 0.0f;
	glm::vec2 m_vel{ 0.0f };             // world velocity
	float m_vx = 0.0f, m_vy = 0.0f;      // local: forward, left
	float m_yawRate = 0.0f;              // rad/s, positive = toward +X (left turn)
	float m_steer = 0.0f;                // smoothed input [-1,1], right positive
	float m_steerAngle = 0.0f;           // front wheel angle, left positive (rad)
	float m_ax = 0.0f, m_ay = 0.0f;      // local accelerations (filtered)
	float m_rearSpin = 0.0f;             // wheelspin 0..1
	float m_drift = 0.0f;                // 0..1 drift state (grip blend)
	float m_driftSign = 1.0f;
	float m_prevBeta = 0.0f;            // +1 right-hand drift, -1 left-hand drift
	float m_brakeTapTimer = 0.0f;
	float m_driftExitTimer = 0.0f;
	float m_accum = 0.0f;
	float m_nitro = 1.0f;
	float m_nitroCooldown = 0.0f;
	bool m_nitroActive = false;
	int m_gear = 1;
	float m_shiftTimer = 0.0f;
	float m_shiftCooldown = 0.0f;        // minimum time between automatic shifts (no gear hunting)
	float m_directionHold = 0.0f;        // time the direction-change pedal has been held at standstill
	bool m_shifted = false;
	float m_rpm = 900.0f;
	bool m_limiter = false;
	float m_load = 0.0f;
	float m_throttle = 0.0f;
	float m_slip = 0.0f;
	float m_impact = 0.0f;
	std::array<WheelState, 4> m_wheels;
	// Visual wheel state (render-rate smoothing of the physics wheel state).
	float m_visHub[4] = { 0.33f, 0.33f, 0.345f, 0.345f };
	float m_visSteer[4] = {};
	float m_visSpin[4] = {};
	float m_visSpinPrev[4] = {};
	float m_visDt = 1.0f / 60.0f;
	bool m_visInit = false;

	// Sprung body: heave (m), pitch (rad, nose up +), roll (rad, right side down +) with velocities.
	float m_heave = 0.0f, m_heaveV = 0.0f;
	float m_pitch = 0.0f, m_pitchV = 0.0f;
	float m_roll = 0.0f, m_rollV = 0.0f;

	// Camera state.
	int m_cameraMode = 0;        // 0 chase, 1 far chase, 2 bumper, 3 free
	glm::vec3 m_camPos{ 0.0f };
	glm::vec3 m_camVel{ 0.0f };
	glm::vec3 m_camCarPrev{ 0.0f };
	float m_camHeading = 0.0f;
	float m_camFov = 55.0f;
	bool m_camInitialized = false;
	float m_time = 0.0f;
	bool m_teleported = false;
	bool m_teleportFx = false;
};

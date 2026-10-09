#pragma once
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
*   * Vehicle: throttle/brake/reverse, speed-dependent steering, handbrake drift with lateral slip,
*     nitrous with depletion and delayed recharge, 6-speed gearbox with shift times, rev limiter and
*     an engine-load estimate (throttle x rpm, shown as BOOST: no turbo is simulated).
*   * Collision: the drivable band of the shared RoadNetwork (road + sidewalk; building lines are
*     walls) plus circle/box obstacles from the generated layout (poles, palms, parked cars). The
*     car is two circles; sub-stepped at speed so it cannot tunnel. Legacy scenes may still use
*     rectangular "drivable_areas".
*   * Chase camera: low rear view tuned to the reference framing, critically damped follow, heading
*     lag, speed/nitrous FOV, kept inside the street band and out of obstacles.
*
* All tuning lives in the scene's "game" object ("handling", "camera", "spawn") and is editable in
* the editor's Drive panel; Params() is the runtime copy.
*/
class DriveMode {
public:
	struct Handling {
		float topSpeedKmh = 250.0f;
		float acceleration = 10.5f;     // m/s^2 at low speed
		float braking = 17.0f;          // m/s^2
		float reverseMaxKmh = 40.0f;
		float steerLockLow = 34.0f;     // degrees at standstill
		float steerLockHigh = 5.5f;     // degrees at top speed
		float steerRate = 9.0f;         // 1/s
		float grip = 6.5f;              // lateral slip decay (1/s)
		float driftGrip = 1.2f;         // with the handbrake
		float driftYawGain = 1.9f;
		float nitroThrust = 1.7f;       // acceleration multiplier
		float nitroTopSpeed = 1.12f;    // top speed multiplier
		float nitroDrain = 0.24f;       // tank per second
		float nitroRecharge = 0.06f;    // tank per second
		float nitroRechargeDelay = 1.5f;// seconds after use
		float rpmIdle = 900.0f;
		float rpmRedline = 7800.0f;
		float rpmLimiter = 8200.0f;
		float shiftTime = 0.16f;
	};
	struct CameraRig {
		float distance = 5.6f;
		float height = 1.38f;
		float lookAhead = 9.0f;
		float lookHeight = 1.55f;
		float fov = 50.0f;              // vertical degrees at rest
		float fovSpeedGain = 10.0f;
		float fovNitro = 6.0f;
		float pullBack = 0.9f;          // extra distance at top speed
		float stiffness = 10.0f;
		float headingLag = 4.5f;
	};
	struct Input {
		float throttle = 0, brake = 0, steer = 0;
		bool handbrake = false, nitrous = false;
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
	// Put the car back on the nearest road, facing along it (R / reset).
	void RecoverToRoad();
	bool ConsumeTeleport() { const bool t = m_teleported; m_teleported = false; return t; }

	// Simulates the car (frozen: physics paused, e.g. countdown/menus) and poses the camera.
	void Update(float dt, const Input& in, bool frozen, Camera* camera, float aspect);

	// State for HUD / race / audio.
	glm::vec2 Position() const { return m_pos; }
	glm::vec2 PreviousPosition() const { return m_prevPos; }
	float Heading() const { return m_heading; }
	float SpeedKmh() const { return std::abs(m_speed) * 3.6f; }
	float SpeedMs() const { return m_speed; }
	float Speed01() const;
	float Rpm() const { return m_rpm; }
	int Gear() const { return m_gear; }          // -1 = R, 1..6
	float Nitro() const { return m_nitro; }
	bool NitroActive() const { return m_nitroActive; }
	bool NitroRecharging() const { return !m_nitroActive && m_nitro < 1.0f && m_nitroCooldown <= 0.0f; }
	float Load() const { return m_load; }
	float Throttle() const { return m_throttle; }
	float Slip01() const { return m_slip; }
	bool AtRedline() const { return m_rpm >= m_handling.rpmRedline; }
	bool LimiterHit() const { return m_limiter; }
	bool ConsumeShift() { const bool s = m_shifted; m_shifted = false; return s; }
	bool HasCollided() const { return m_impact > 0.0f; }

	Handling& MutableHandling() { return m_handling; }
	CameraRig& MutableCamera() { return m_rig; }
	glm::vec2 SpawnPosition() const { return m_spawnPos; }
	float SpawnHeading() const { return m_spawnHeading; }
	void SetSpawn(const glm::vec2& p, float heading) { m_spawnPos = p; m_spawnHeading = heading; }
	nlohmann::json SaveTuning() const; // "handling" + "camera" objects

	static void ReadHandling(const nlohmann::json& j, Handling& h);
	static void ReadCamera(const nlohmann::json& j, CameraRig& c);

private:
	struct Area { glm::vec2 min, max; };
	struct Obstacle { bool box = false; glm::vec2 center{ 0.0f }; glm::vec2 half{ 0.0f }; float yaw = 0.0f; float radius = 0.0f; };

	void Simulate(float dt, const Input& in);
	void Collide(glm::vec2& pos, glm::vec2& vel);
	// Push a circle out of walls/obstacles; returns the accumulated push normal (zero = free).
	glm::vec2 ResolveCircle(glm::vec2& c, float radius) const;
	void ApplyTransforms();
	void UpdateCamera(float dt, Camera& camera, float aspect);
	void LoadLayout(const std::string& path);

	bool m_available = false;
	bool m_active = false;
	std::weak_ptr<SceneNode> m_player;
	std::weak_ptr<SceneNode> m_body;
	glm::vec3 m_bodyBasePos{ 0.0f };
	glm::quat m_bodyBaseRot{ 1.0f, 0.0f, 0.0f, 0.0f };
	glm::vec3 m_bodyScale{ 1.0f };
	float m_playerY = 0.0f;
	std::vector<Area> m_areas;
	std::vector<Obstacle> m_obstacles;
	const RoadNetwork* m_roads = nullptr;
	Handling m_handling;
	CameraRig m_rig;

	// Vehicle state (metres, seconds, radians). Heading 0 = +Z, increases toward +X.
	glm::vec2 m_pos{ 0.0f };
	glm::vec2 m_prevPos{ 0.0f };
	glm::vec2 m_spawnPos{ 0.0f };
	float m_spawnHeading = 0.0f;
	float m_heading = 0.0f;
	float m_speed = 0.0f;        // forward m/s (negative = reverse)
	float m_lateral = 0.0f;      // sideways m/s (drift)
	float m_steer = 0.0f;
	float m_yawRate = 0.0f;
	float m_nitro = 1.0f;
	float m_nitroCooldown = 0.0f;
	bool m_nitroActive = false;
	int m_gear = 1;
	float m_shiftTimer = 0.0f;
	bool m_shifted = false;
	float m_rpm = 900.0f;
	bool m_limiter = false;
	float m_load = 0.0f;
	float m_throttle = 0.0f;
	float m_slip = 0.0f;
	float m_impact = 0.0f;
	float m_longAccel = 0.0f;
	float m_latAccel = 0.0f;
	float m_roll = 0.0f, m_pitch = 0.0f;

	// Camera state.
	int m_cameraMode = 0;        // 0 chase, 1 far chase, 2 bumper, 3 free
	glm::vec3 m_camPos{ 0.0f };
	glm::vec3 m_camVel{ 0.0f };
	float m_camHeading = 0.0f;
	float m_camFov = 50.0f;
	bool m_camInitialized = false;
	float m_time = 0.0f;
	bool m_teleported = false;
};

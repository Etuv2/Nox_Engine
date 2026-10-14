#pragma once
// Generic vehicle backend types (NoxEngineCore). No game names, asset paths or handling presets:
// the caller (a game module or the legacy racing adapter) supplies every numeric value.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

class SceneNode;
class RoadNetwork;
class CollisionWorld;

// Handle = slot + incarnation. A destroyed instance bumps its slot's incarnation, so stale handles
// never resolve to a newer instance that reuses the slot.
struct VehicleHandle {
	uint32_t id = 0;          // slot index + 1; 0 = invalid
	uint32_t incarnation = 0;
	bool Valid() const { return id != 0; }
	bool operator==(const VehicleHandle& o) const { return id == o.id && incarnation == o.incarnation; }
	bool operator!=(const VehicleHandle& o) const { return !(*this == o); }
};

// Driver input latched for a frame and held for its fixed sub-steps.
struct VehicleInput {
	float throttle = 0.0f, brake = 0.0f, steer = 0.0f; // 0..1, 0..1, -1..1 (right positive)
	bool handbrake = false;
	// Named auxiliary inputs (buttons or axes). The backend never reads them; the policy interprets
	// each name. A missing name reads as 0 (released).
	std::map<std::string, float> aux;
	float Aux(const std::string& name) const {
		const auto it = aux.find(name);
		return it == aux.end() ? 0.0f : it->second;
	}
};

// Numeric backend parameters per instance. Defaults are neutral placeholders, not a vehicle preset.
struct VehicleTuning {
	float topSpeedKmh = 200.0f;
	float driveAcceleration = 5.0f;     // m/s^2 drive capacity in first gear before traction limits
	float brakeDecel = 8.0f;            // m/s^2 at full brake pedal
	float reverseMaxKmh = 30.0f;
	float mass = 1000.0f;               // kg
	float cgHeight = 0.5f;              // m
	float tireGrip = 1.0f;              // lateral friction coefficient
	float tireGripLong = 1.0f;          // longitudinal friction coefficient
	float rpmIdle = 900.0f;
	float rpmLimiter = 7500.0f;
	std::vector<float> gearRatios = { 3.0f, 2.0f, 1.5f, 1.2f, 1.0f }; // first..top, strictly descending, 1..8 gears
	float suspensionFreq = 1.5f;        // Hz
	float suspensionDamping = 0.6f;     // damping ratio
	float bumpAmplitude = 0.0f;         // m procedural road roughness (0 = flat)
	float roofHeight = 1.6f;            // m above body base; wall contacts stop at this height
};

// Rig slots bound by the caller. Each is optional; the backend poses whatever is present.
struct VehicleRigNodes {
	std::weak_ptr<SceneNode> root;                    // placed at the ground point and heading
	std::weak_ptr<SceneNode> model;                   // body node (heave/roll in the non-wheel-rig path)
	std::weak_ptr<SceneNode> body;                    // sprung body mesh (wheel rig only)
	std::array<std::weak_ptr<SceneNode>, 4> wheels;   // hubs FL, FR, RL, RR (wheel rig only)
};

struct VehicleArea { glm::vec2 min{ 0.0f }, max{ 0.0f }; };                  // drivable rectangle (planar)
struct VehicleObstacle {                                                     // planar layout obstacle
	bool box = false;
	glm::vec2 center{ 0.0f }, half{ 0.0f };
	float yaw = 0.0f, radius = 0.0f;
};

// Everything a backend instance needs. Supported layout: backend "two_axle_planar", drive "rear",
// exactly four wheels on two axles. Anything else is rejected by VehicleService::Create.
struct VehicleDefinition {
	std::string name;                                 // diagnostics only
	std::string backend = "two_axle_planar";
	std::string drive = "rear";
	float frontAxleDistance = 1.36f;                  // m, CG to front axle (positive)
	float rearAxleDistance = 1.36f;                   // m, CG to rear axle (positive)
	float track = 1.6f;                               // m
	std::array<float, 4> wheelRadius = { 0.33f, 0.33f, 0.345f, 0.345f }; // FL FR RL RR
	std::array<glm::vec3, 4> wheelRest = { glm::vec3(0.8f, 0.33f, 1.36f), glm::vec3(-0.8f, 0.33f, 1.36f),
		glm::vec3(0.8f, 0.33f, -1.36f), glm::vec3(-0.8f, 0.33f, -1.36f) }; // hub, car space: x left, y up, z forward
	bool wheelRig = false;                            // wheel nodes present: sprung body, steer and spin posed
	float collisionRadius = 0.95f;                    // m, two collision circles
	float collisionOffset = 1.25f;                    // m, circle centres ahead and behind the CG
	float rootHeight = 0.0f;                          // planar mode: root height above the ground
	VehicleTuning tuning;
	VehicleRigNodes rig;
	glm::vec2 spawnPos{ 0.0f };
	float spawnHeading = 0.0f;                        // rad, 0 = +Z
	float spawnY = 1e4f;                              // surface mode: ground height hint (1e4 = highest surface)
	const RoadNetwork* roads = nullptr;               // optional, non-owning
	const CollisionWorld* collision = nullptr;        // optional surface mode, non-owning
	std::vector<VehicleArea> areas;
	std::vector<VehicleObstacle> obstacles;
};

struct VehicleWheelState {
	glm::vec3 contact{ 0.0f };      // world contact point
	glm::vec3 velocity{ 0.0f };     // world velocity of the contact patch
	float slip = 0.0f;              // 0..1 combined slip
	float wheelspin = 0.0f;         // 0..1 driven-wheel overspeed
	float load = 0.0f;              // N
	bool grounded = true;
	float spinAngle = 0.0f;
	float steerAngle = 0.0f;        // rad, left positive
	float suspension = 0.0f;        // m compression relative to rest
};

// Read-only snapshot of one instance (copied out; the service never hands out references).
struct VehicleState {
	glm::vec2 pos{ 0.0f }, prevPos{ 0.0f }, vel{ 0.0f };
	float heading = 0.0f;
	float vx = 0.0f, vy = 0.0f;     // local: forward, left
	float yawRate = 0.0f;
	float ax = 0.0f, ay = 0.0f;     // filtered local accelerations
	float steerAngle = 0.0f;        // front wheel angle, left positive (rad)
	float time = 0.0f;              // simulated seconds
	float y = 0.0f;                 // surface mode: ground height under the CG
	bool surface = false;           // collision surface present
	bool airborne = false;
	float groundPitch = 0.0f, groundRoll = 0.0f;
	float heave = 0.0f;             // body heave (m)
	float playerY = 0.0f;           // planar root height offset
	int gear = 1;                   // -1 = reverse, 1..gearCount
	float rpm = 900.0f;
	bool limiter = false;
	float rearSpin = 0.0f, slip = 0.0f, load = 0.0f, throttle = 0.0f, impact = 0.0f;
	std::array<VehicleWheelState, 4> wheels;
	bool wheelRig = false;
	glm::vec2 spawnPos{ 0.0f };
	float spawnHeading = 0.0f, spawnY = 1e4f;
	uint32_t teleportSerial = 0;    // increments on every teleport (camera re-initialisation)
};

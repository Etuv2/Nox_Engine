#include "DriveMode.h"
#include "assets/AssetIO.h"

#include "Camera.h"
#include "CollisionWorld.h"
#include "RoadNetwork.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>

using nlohmann::json;

namespace {
	constexpr float kPi = 3.14159265f;
	const char* kWheelNames[4] = { "Wheel_FL", "Wheel_FR", "Wheel_RL", "Wheel_RR" };

	std::shared_ptr<SceneNode> FindByName(const std::shared_ptr<SceneNode>& node, const std::string& name) {
		if (!node) return nullptr;
		if (node->GetName() == name) return node;
		for (const auto& child : node->children) {
			if (auto found = FindByName(child, name)) return found;
		}
		return nullptr;
	}

	float WrapAngle(float a) {
		while (a > kPi) a -= 2.0f * kPi;
		while (a < -kPi) a += 2.0f * kPi;
		return a;
	}

	// Frame-rate independent exponential approach; 'rate' is already multiplied by dt.
	float Approach(float current, float target, float rate) {
		return current + (target - current) * (1.0f - std::exp(-rate));
	}

	// NOX_VEHICLE_POLICY_TRACE=<file.jsonl>: opened once per process (see VehiclePolicyTrace).
	std::FILE* PolicyTraceFile() {
		static std::FILE* file = [] {
			const char* path = std::getenv("NOX_VEHICLE_POLICY_TRACE");
			return (path && *path) ? std::fopen(path, "w") : static_cast<std::FILE*>(nullptr);
		}();
		return file;
	}
}

// ---------------------------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------------------------
void DriveMode::ReadCamera(const json& j, CameraRig& c)
{
	if (!j.is_object()) return;
	// Camera keys were re-based to the NFS 2015 framing (v2); older v1 values are ignored.
	if (j.value("version", 1) < 2) return;
	c.distance = j.value("distance", c.distance);
	c.height = j.value("height", c.height);
	c.lookAhead = j.value("look_ahead", c.lookAhead);
	c.lookHeight = j.value("look_height", c.lookHeight);
	c.fov = j.value("fov", c.fov);
	c.fovSpeedGain = j.value("fov_speed_gain", c.fovSpeedGain);
	c.fovNitro = j.value("fov_nitro", c.fovNitro);
	c.pullBack = j.value("pull_back", c.pullBack);
	c.stiffness = j.value("stiffness", c.stiffness);
	c.headingLag = j.value("heading_lag", c.headingLag);
	c.velocityFollow = j.value("velocity_follow", c.velocityFollow);
}

json DriveMode::SaveTuning() const
{
	const Handling& h = m_policy.GetHandling();
	const CameraRig& c = m_rig;
	json out;
	out["handling"] = {
		{"top_speed_kmh", h.topSpeedKmh}, {"drive_accel", h.acceleration}, {"brake_decel", h.braking},
		{"reverse_max_kmh", h.reverseMaxKmh}, {"mass", h.mass}, {"cg_height", h.cgHeight},
		{"tire_grip", h.tireGrip}, {"tire_grip_long", h.tireGripLong},
		{"steer_lock_low", h.steerLockLow}, {"steer_lock_high", h.steerLockHigh}, {"steer_rate_hz", h.steerRate},
		{"drift_grip_scale", h.driftGripScale}, {"drift_assist", h.driftAssist}, {"max_drift_angle", h.maxDriftAngle},
		{"countersteer_assist", h.countersteerAssist}, {"yaw_damping", h.yawDamping},
		{"nitro_thrust", h.nitroThrust}, {"nitro_top_speed", h.nitroTopSpeed}, {"nitro_drain", h.nitroDrain},
		{"nitro_recharge", h.nitroRecharge}, {"nitro_recharge_delay", h.nitroRechargeDelay},
		{"rpm_idle", h.rpmIdle}, {"rpm_redline", h.rpmRedline}, {"rpm_limiter", h.rpmLimiter}, {"shift_time", h.shiftTime},
		{"gear_ratios", h.gearRatios}, {"reverse_engage_delay", h.reverseEngageDelay},
		{"suspension_freq", h.suspensionFreq}, {"suspension_damping", h.suspensionDamping}, {"bump_amplitude", h.bumpAmplitude}, {"roof_height", h.roofHeight} };
	out["camera"] = {
		{"version", 2}, {"distance", c.distance}, {"height", c.height}, {"look_ahead", c.lookAhead}, {"look_height", c.lookHeight},
		{"fov", c.fov}, {"fov_speed_gain", c.fovSpeedGain}, {"fov_nitro", c.fovNitro}, {"pull_back", c.pullBack},
		{"stiffness", c.stiffness}, {"heading_lag", c.headingLag}, {"velocity_follow", c.velocityFollow} };
	return out;
}

// Layout obstacles (planar mode): box or circle entries from the scene's layout file.
static std::vector<VehicleObstacle> ReadLayoutObstacles(const std::string& path)
{
	std::vector<VehicleObstacle> out;
	if (path.empty()) return out;
	const std::optional<std::string> layoutText = AssetIO::ReadText(path);
	if (!layoutText) return out;
	const json layout = json::parse(*layoutText, nullptr, false);
	if (!layout.is_object() || !layout.contains("obstacles") || !layout["obstacles"].is_array()) return out;
	for (const json& o : layout["obstacles"]) {
		VehicleObstacle ob;
		const std::string type = o.value("type", std::string("circle"));
		if (type == "box") {
			ob.box = true;
			const json c = o.value("center", json::array({ 0.0f, 0.0f }));
			const json h = o.value("half", json::array({ 0.5f, 0.5f }));
			ob.center = glm::vec2(c[0].get<float>(), c[1].get<float>());
			ob.half = glm::vec2(h[0].get<float>(), h[1].get<float>());
			ob.yaw = o.value("yaw", 0.0f);
		}
		else {
			const json c = o.value("pos", json::array({ 0.0f, 0.0f }));
			ob.center = glm::vec2(c[0].get<float>(), c[1].get<float>());
			ob.radius = o.value("radius", 0.2f);
		}
		out.push_back(ob);
	}
	return out;
}

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------
void DriveMode::Configure(const json& g, SceneGraph& sceneGraph, const RoadNetwork* roads, const std::string& layoutPath,
	const CollisionWorld* collision)
{
	if (m_handle.Valid()) m_service.Destroy(m_handle);
	m_handle = VehicleHandle();
	m_recorder.reset();      // refers to the policy: first
	m_luaPolicy.reset();
	m_luaRuntime.reset();
	m_luaSource.reset();
	m_available = false;
	m_active = false;
	m_collision = (collision && !collision->Empty()) ? collision : nullptr;
	m_policy = LegacyNfsPolicy();
	m_rig = CameraRig();
	if (!g.is_object()) return;

	auto root = sceneGraph.GetRoot();
	auto player = FindByName(root, g.value("player_node", std::string("PlayerCar")));
	if (!player) {
		std::cerr << "[DriveMode] player_node not found" << std::endl;
		return;
	}
	m_player = player;

	VehicleDefinition def;
	def.name = g.value("player_node", std::string("PlayerCar"));
	def.rootHeight = player->GetPosition().y;
	def.rig.root = player;
	if (auto body = FindByName(root, g.value("body_node", std::string()))) {
		def.rig.model = body;
		// Wheel rig (glTF nodes inside the body model).
		auto bodyMesh = FindByName(body, "Body");
		int found = 0;
		std::shared_ptr<SceneNode> wheels[4];
		for (int i = 0; i < 4; ++i) {
			wheels[i] = FindByName(body, kWheelNames[i]);
			def.rig.wheels[i] = wheels[i];
			found += wheels[i] ? 1 : 0;
		}
		if (bodyMesh && found == 4) {
			def.rig.body = bodyMesh;
			def.wheelRig = true;
			const glm::vec3 restPos = body->GetPosition();
			const glm::vec3 s = body->GetScale();
			for (int i = 0; i < 4; ++i) {
				def.wheelRest[i] = restPos + wheels[i]->GetPosition() * s;
			}
			const float frontZ = 0.5f * (def.wheelRest[0].z + def.wheelRest[1].z);
			const float rearZ = 0.5f * (def.wheelRest[2].z + def.wheelRest[3].z);
			def.frontAxleDistance = std::max(frontZ, 0.5f);
			def.rearAxleDistance = std::max(-rearZ, 0.5f);
			def.track = std::abs(def.wheelRest[0].x - def.wheelRest[1].x);
			for (int i = 0; i < 4; ++i) def.wheelRadius[i] = std::max(def.wheelRest[i].y, 0.2f);
			for (int i = 0; i < 4; ++i) {
				const glm::vec3 wp = wheels[i]->GetPosition();
				const glm::quat q = wheels[i]->GetOrientation();
				std::cout << "[DriveMode] " << kWheelNames[i] << " rest pos (" << wp.x << ", " << wp.y << ", "
					<< wp.z << ") rot (" << q.w << ", " << q.x << ", " << q.y << ", " << q.z << ") scale " << wheels[i]->GetScale().x << std::endl;
			}
		}
	}
	if (!def.wheelRig) {
		// Legacy single-mesh car: nominal geometry.
		def.frontAxleDistance = 1.36f;
		def.rearAxleDistance = 1.36f;
		def.track = 1.6f;
		const float x = def.track * 0.5f;
		def.wheelRest[0] = glm::vec3(x, 0.33f, def.frontAxleDistance);
		def.wheelRest[1] = glm::vec3(-x, 0.33f, def.frontAxleDistance);
		def.wheelRest[2] = glm::vec3(x, 0.33f, -def.rearAxleDistance);
		def.wheelRest[3] = glm::vec3(-x, 0.33f, -def.rearAxleDistance);
	}
	if (g.contains("drivable_areas") && g["drivable_areas"].is_array()) {
		for (const json& a : g["drivable_areas"]) {
			if (a.is_array() && a.size() >= 4) {
				def.areas.push_back({ glm::vec2(a[0].get<float>(), a[1].get<float>()), glm::vec2(a[2].get<float>(), a[3].get<float>()) });
			}
		}
	}
	LegacyNfsPolicy::ReadHandling(g.value("handling", json::object()), m_policy.MutableHandling());
	ReadCamera(g.value("camera", json::object()), m_rig);
	def.obstacles = ReadLayoutObstacles(layoutPath);
	def.tuning = LegacyNfsPolicy::NativeTuning(m_policy.GetHandling());
	def.roads = (roads && !roads->Empty()) ? roads : nullptr;
	def.collision = m_collision;

	const glm::vec3 p = player->GetPosition();
	def.spawnPos = glm::vec2(p.x, p.z);
	// Surface mode: the player node height is the ride offset above the ground; a spawn without a
	// height lands on the highest surface.
	def.spawnY = 1e4f;
	const glm::vec3 fwd = player->GetOrientation() * glm::vec3(0, 0, 1);
	def.spawnHeading = std::atan2(fwd.x, fwd.z);
	if (g.contains("spawn") && g["spawn"].is_object()) {
		const json& s = g["spawn"];
		if (s.contains("position") && s["position"].is_array() && s["position"].size() >= 3) {
			def.spawnPos = glm::vec2(s["position"][0].get<float>(), s["position"][2].get<float>());
			def.spawnY = s["position"][1].get<float>();
		}
		else if (s.contains("position") && s["position"].is_array() && s["position"].size() >= 2) {
			def.spawnPos = glm::vec2(s["position"][0].get<float>(), s["position"][1].get<float>());
		}
		def.spawnHeading = glm::radians(s.value("heading_deg", glm::degrees(def.spawnHeading)));
	}

	// Policy selection. Default: the native LegacyNfsPolicy. NOX_VEHICLE_POLICY=lua (transitional): the project
	// Lua controller is the only policy; LegacyNfsPolicy is not called. NOX_VEHICLE_POLICY_TRACE=<file>: every
	// policy call is recorded (VehiclePolicyTrace) and passes through the recorder unchanged.
	IVehiclePolicy* policy = &m_policy;
	static const bool useLua = [] { const char* v = std::getenv("NOX_VEHICLE_POLICY"); return v && std::string(v) == "lua"; }();
	if (useLua) {
		CreateLuaPolicy(SaveTuning().at("handling"));
		policy = m_luaPolicy.get();
	}
	if (PolicyTraceFile()) {
		m_recorder = std::make_unique<VehiclePolicyTrace::Recorder>(*policy, [](const std::string& line) {
			std::FILE* f = PolicyTraceFile();
			if (!f) return;
			std::fwrite(line.data(), 1, line.size(), f);
			std::fputc('\n', f);
		});
		policy = m_recorder.get();
	}

	std::string error;
	m_handle = m_service.Create(def, policy, &error);
	if (!m_handle.Valid()) {
		std::cerr << "[DriveMode] " << error << std::endl;
		return;
	}
	m_available = true;
	static const bool recording = [] { const char* path = std::getenv("NOX_VEHICLE_TELEMETRY"); return path && *path; }();
	if (recording) {
		m_service.SetSubstepObserver(m_handle, [this](const VehicleSubstepRecord& r) { RecordTelemetry(r); });
	}
	std::cout << "[DriveMode] Ready. " << (def.wheelRig ? "Wheel rig" : "Single-mesh body") << " a=" << def.frontAxleDistance << " b=" << def.rearAxleDistance
		<< " track=" << def.track << " areas=" << def.areas.size() << " obstacles=" << def.obstacles.size()
		<< (def.roads ? " (road network)" : "") << (m_collision ? " (collision surface)" : "") << std::endl;
}

// Transitional Lua controller (docs/lua_api.md "Vehicles"). The controller module and its tuning come from the
// vehicle data file; the handling profile is the facade's current handling (scene values merged over the
// defaults). A controller that cannot be loaded is still returned, faulted, so the car coasts and the reason
// is reported; no native policy runs in its place.
void DriveMode::CreateLuaPolicy(const json& handling)
{
	constexpr const char* kVehicleData = "projects/nox-racing/data/vehicles/r33.json";
	constexpr const char* kScriptRoot = "projects/nox-racing/scripts";
	m_luaSource = std::make_unique<NoxLua::AssetScriptSource>();
	NoxLua::RuntimeLimits limits;
	limits.memoryBytes = 4u * 1024u * 1024u;
	std::string module;
	json data = json::object();
	if (const std::optional<std::string> text = AssetIO::ReadText(kVehicleData)) {
		data = json::parse(*text, nullptr, false);
		if (data.is_object() && data.contains("controller") && data["controller"].is_object()) {
			module = data["controller"].value("module", std::string());
			limits.instructionBudget = data["controller"].value("instruction_budget", static_cast<uint64_t>(limits.instructionBudget));
		}
	}
	if (!data.is_object()) data = json::object();
	m_luaRuntime = std::make_unique<NoxLua::Runtime>(limits, m_luaSource.get());
	m_luaRuntime->SetScriptRoots({ kScriptRoot });
	// The scene's handling (facade values) is the controller's handling; the data file supplies the policy tuning.
	json params = json::object();
	params["handling"] = handling;
	const json controllerParams = (data.contains("controller") && data["controller"].is_object())
		? data["controller"].value("params", json::object()) : json::object();
	params["policy"] = controllerParams.value("policy", json::object());
	m_luaPolicy = std::make_unique<NoxLua::LuaVehiclePolicy>(*m_luaRuntime, module, params);
	m_luaFaultsReported = 0;
	if (m_luaPolicy->Ok()) {
		std::cout << "[DriveMode] NOX_VEHICLE_POLICY=lua: controller '" << module << "' loaded" << std::endl;
	}
	else {
		std::cerr << "[DriveMode] NOX_VEHICLE_POLICY=lua: controller '" << module << "' FAULTED: "
			<< m_luaPolicy->Error() << " (car coasts; no native policy runs)" << std::endl;
	}
}

void DriveMode::Refuel()
{
	if (m_luaPolicy) m_luaPolicy->OnRespawn();   // the controller's refuel rule (on_respawn)
	else m_policy.Refuel();
}

// Pushes the (possibly editor-modified) handling into the native backend. Structure errors keep the
// previous tuning, so a bad edit cannot stop the car.
void DriveMode::SyncTuning()
{
	if (!m_handle.Valid()) return;
	m_service.SetTuning(m_handle, LegacyNfsPolicy::NativeTuning(m_policy.GetHandling()));
}

float DriveMode::Speed01() const
{
	return std::clamp(glm::length(State().vel) / (m_policy.GetHandling().topSpeedKmh / 3.6f), 0.0f, 1.0f);
}

float DriveMode::DriftAngleDeg() const
{
	const VehicleState s = State();
	if (glm::length(s.vel) < 3.0f) return 0.0f;
	return glm::degrees(std::atan2(s.vy, std::abs(s.vx)));
}

void DriveMode::Teleport(const glm::vec3& pos, float heading)
{
	SyncTuning();
	m_service.Teleport(m_handle, pos, heading);
}

void DriveMode::Teleport(const glm::vec2& pos, float heading)
{
	SyncTuning();
	m_service.Teleport(m_handle, pos, heading);
}

void DriveMode::ResetToSpawn()
{
	SyncTuning();
	m_service.ResetToSpawn(m_handle);
}

void DriveMode::RecoverToRoad()
{
	SyncTuning();
	m_service.RecoverToRoad(m_handle);
}

bool DriveMode::ConsumeTeleport() { return m_service.TakeTeleported(m_handle); }
bool DriveMode::ConsumeTeleportForEffects() { return m_service.TakeTeleportFx(m_handle); }

void DriveMode::SetActive(bool active)
{
	if (!m_available) {
		m_active = false;
		return;
	}
	m_active = active;
	m_camInitialized = false;
	if (active) {
		if (auto player = m_player.lock()) {
			const glm::vec3 p = player->GetPosition();
			m_service.SetPlanarPosition(m_handle, glm::vec2(p.x, p.z));
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Camera (chase rig, drift framing, speed effects); the car state is read from the service snapshot
// ---------------------------------------------------------------------------------------------
void DriveMode::UpdateCamera(float dt, Camera& camera, float aspect)
{
	const VehicleState s = State();
	if (s.teleportSerial != m_camTeleportSerial) {
		// A teleport (or recovery) re-initialises the chase rig.
		m_camTeleportSerial = s.teleportSerial;
		m_camInitialized = false;
	}
	const CameraRig& rig = m_rig;
	const float speed = glm::length(s.vel);
	const float speed01 = Speed01();
	const float baseY = s.surface ? s.y : 0.0f;
	const float playerYOffset = s.surface ? 0.0f : s.playerY;
	const glm::vec3 carPos(s.pos.x, baseY + playerYOffset, s.pos.y);
	// Follow direction: the heading blended with the velocity direction, so drifts show the car's
	// flank like NFS 2015 while straight driving stays locked behind the car. Reversing keeps the
	// camera behind the nose (no flip).
	float followHeading = s.heading;
	if (speed > 4.0f && s.vx > 0.0f) {
		const float velHeading = std::atan2(s.vel.x, s.vel.y);
		followHeading = s.heading + WrapAngle(velHeading - s.heading) * rig.velocityFollow * std::clamp((speed - 4.0f) / 8.0f, 0.0f, 1.0f);
	}
	// NOX_CAM_ORBIT=<deg>: validation captures from the side / front (rotates the chase rig).
	static const float orbit = [] { const char* v = std::getenv("NOX_CAM_ORBIT"); return v ? glm::radians(static_cast<float>(std::atof(v))) : 0.0f; }();
	followHeading += orbit;
	if (!m_camInitialized) m_camHeading = followHeading;
	m_camHeading = WrapAngle(m_camHeading + WrapAngle(followHeading - m_camHeading) * (1.0f - std::exp(-dt * (m_cameraMode == 2 ? 30.0f : rig.headingLag))));
	glm::vec3 camFwd(std::sin(m_camHeading), 0.0f, std::cos(m_camHeading));
	// Surface mode: the rig follows the road grade (the camera stays above the road behind the car
	// on climbs and descents).
	if (s.surface) camFwd = glm::normalize(camFwd + glm::vec3(0.0f, std::tan(std::clamp(s.groundPitch, -0.3f, 0.3f)), 0.0f));

	float distance = rig.distance, height = rig.height, lookAhead = rig.lookAhead, lookHeight = rig.lookHeight;
	if (m_cameraMode == 1) { distance *= 1.5f; height *= 1.45f; lookAhead *= 1.1f; lookHeight *= 1.4f; }
	// Restrained speed effects: slight pull-back, a little more under hard acceleration.
	distance += speed01 * rig.pullBack + std::clamp(s.ax * 0.025f, -0.12f, 0.3f);
	glm::vec3 desired = carPos - camFwd * distance + glm::vec3(0.0f, height, 0.0f);
	glm::vec3 target = carPos + camFwd * lookAhead + glm::vec3(0.0f, lookHeight, 0.0f);
	const glm::vec3 fwd(std::sin(s.heading), 0.0f, std::cos(s.heading));
	if (m_cameraMode == 2) {
		desired = carPos + fwd * 1.9f + glm::vec3(0.0f, 0.75f + s.heave, 0.0f);
		target = desired + fwd * 10.0f;
	}

	if (!m_camInitialized || m_cameraMode == 2) {
		m_camPos = desired;
		m_camVel = glm::vec3(0.0f);
		m_camInitialized = true;
	}
	else {
		// Critically damped spring on the car-relative offset (no speed-dependent lag; the heading
		// smoothing above provides the cornering/drift response).
		const glm::vec3 desiredOff = desired - carPos;
		const glm::vec3 currentOff = m_camPos - m_camCarPrev;
		const float omega = rig.stiffness;
		const glm::vec3 x = currentOff - desiredOff;
		const float e = std::exp(-omega * dt);
		const glm::vec3 temp = (m_camVel + omega * x) * dt;
		m_camVel = (m_camVel - omega * temp) * e;
		glm::vec3 off = desiredOff + (x + temp) * e;
		off.y = desiredOff.y + (off.y - desiredOff.y) * 0.5f;
		m_camPos = carPos + off;
	}
	m_camCarPrev = carPos;
	// Keep the camera out of walls and obstacles: pull it toward the car until it is clear.
	if (m_cameraMode != 2 && m_collision) {
		// Ray from above the car to the camera against all geometry (walls, decks, tunnel ceilings).
		const glm::vec3 pivot = carPos + glm::vec3(0.0f, 1.2f, 0.0f);
		const glm::vec3 d = m_camPos - pivot;
		const float len = glm::length(d);
		CollisionWorld::Hit hit;
		if (len > 0.1f && m_collision->Raycast(pivot, d / len, len + 0.3f, CollisionWorld::Any, hit)) {
			m_camPos = pivot + d / len * std::max(0.3f, hit.t - 0.3f);
		}
	}
	else if (m_cameraMode != 2) {
		glm::vec2 c(m_camPos.x, m_camPos.z);
		const glm::vec2 car(s.pos.x, s.pos.y);
		for (int i = 0; i < 6; ++i) {
			glm::vec2 probe = c;
			const glm::vec2 push = m_service.ResolveCircle(m_handle, probe, 0.35f);
			if (glm::dot(push, push) < 1e-6f) break;
			c = glm::mix(c, car, 0.25f);
		}
		m_camPos.x = c.x;
		m_camPos.z = c.y;
	}
	glm::vec3 eye = m_camPos;
	if (speed01 > 0.6f) {
		const float a = (speed01 - 0.6f) * 0.012f;
		eye += glm::vec3(std::sin(s.time * 31.0f), std::sin(s.time * 47.0f + 1.3f), 0.0f) * a;
	}
	const float targetFov = rig.fov + speed01 * rig.fovSpeedGain + (NitroActive() ? rig.fovNitro : 0.0f);
	m_camFov = Approach(m_camFov, targetFov, dt * 2.5f);

	// NOX_CAM_POSE="ex,ey,ez,tx,ty,tz": fixed validation camera (close-ups of the city / interiors).
	static const std::string poseEnv = [] { const char* v = std::getenv("NOX_CAM_POSE"); return v ? std::string(v) : std::string(); }();
	if (!poseEnv.empty()) {
		float v[6] = {};
		if (std::sscanf(poseEnv.c_str(), "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
			eye = glm::vec3(v[0], v[1], v[2]);
			target = glm::vec3(v[3], v[4], v[5]);
		}
	}
	const glm::vec3 f = glm::normalize(target - eye);
	const float pitch = glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f)));
	const float yaw = glm::degrees(std::atan2(f.z, f.x));
	camera.SetPose(eye, glm::vec3(0, 1, 0), yaw, pitch);
	camera.SetPerspective(m_camFov, aspect, camera.GetCameraNearPlane(), camera.GetCameraFarPlane());
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
// NOX_VEHICLE_TELEMETRY=<file.jsonl>: one record per vehicle sub-step (parity baseline for the vehicle
// service). Values are written with %.9g, which round-trips float exactly, so runs compare bit-for-bit.
void DriveMode::RecordTelemetry(const VehicleSubstepRecord& r)
{
	static std::FILE* file = [] {
		const char* path = std::getenv("NOX_VEHICLE_TELEMETRY");
		return (path && *path) ? std::fopen(path, "w") : static_cast<std::FILE*>(nullptr);
	}();
	if (!file) return;
	const VehicleState& s = r.state;
	const VehiclePolicyDebug& p = r.policy;
	std::fprintf(file,
		"{\"t\":%.9g,\"h\":%.9g,\"thr\":%.9g,\"brk\":%.9g,\"steer\":%.9g,\"hb\":%d,\"nos\":%d,"
		"\"x\":%.9g,\"y\":%.9g,\"z\":%.9g,\"head\":%.9g,\"vx\":%.9g,\"vy\":%.9g,\"yawRate\":%.9g,"
		"\"gear\":%d,\"rpm\":%.9g,\"drift\":%.9g,\"driftSign\":%.9g,\"nitro\":%.9g,\"nitroCooldown\":%.9g,"
		"\"slip\":%.9g,\"rearSpin\":%.9g,\"shiftTimer\":%.9g,\"airborne\":%d}\n",
		s.time, r.h, r.in.throttle, r.in.brake, r.in.steer, r.in.handbrake ? 1 : 0, r.in.nitrous ? 1 : 0,
		s.pos.x, s.surface ? s.y : 0.0f, s.pos.y, s.heading, s.vx, s.vy, s.yawRate,
		s.gear, s.rpm, p.drift, p.driftSign, p.nitro, p.nitroCooldown,
		s.slip, s.rearSpin, p.shiftTimer, s.airborne ? 1 : 0);
}

void DriveMode::Update(float dt, const Input& in, bool frozen, Camera* camera, float aspect)
{
	if (!m_active) return;
	dt = std::clamp(dt, 0.0f, VehicleService::kMaxFrameDt);
	SyncTuning();
	m_service.Advance(m_handle, dt, in, frozen);
	// A faulted controller coasts the car; each new fault is printed once (its count is kept by the policy).
	if (m_luaPolicy && m_luaPolicy->Faults().size() > m_luaFaultsReported) {
		for (size_t i = m_luaFaultsReported; i < m_luaPolicy->Faults().size(); ++i) {
			const NoxLua::VehicleFault& f = m_luaPolicy->Faults()[i];
			std::cerr << "[DriveMode] vehicle controller fault (" << f.phase << "): " << f.message
				<< " - the car coasts on the safe actuation" << std::endl;
		}
		m_luaFaultsReported = m_luaPolicy->Faults().size();
	}
	if (camera && m_cameraMode != 3) UpdateCamera(dt, *camera, aspect);
}

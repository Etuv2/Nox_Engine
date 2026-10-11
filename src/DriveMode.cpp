#include "DriveMode.h"

#include "Camera.h"
#include "CollisionWorld.h"
#include "RoadNetwork.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include <SDL/SDL.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <iostream>

using nlohmann::json;

namespace {
	constexpr float kPi = 3.14159265f;
	constexpr float kG = 9.81f;
	constexpr float kStep = 1.0f / 240.0f;       // physics sub-step
	constexpr float kCarRadius = 0.95f;          // two collision circles along the body
	constexpr float kCarCircleOffset = 1.25f;
	const char* kWheelNames[4] = { "Wheel_FL", "Wheel_FR", "Wheel_RL", "Wheel_RR" };

	std::shared_ptr<SceneNode> FindByName(const std::shared_ptr<SceneNode>& node, const std::string& name) {
		if (!node) return nullptr;
		if (node->GetName() == name) return node;
		for (const auto& child : node->children) {
			if (auto found = FindByName(child, name)) return found;
		}
		return nullptr;
	}

	// Frame-rate independent exponential approach; 'rate' is already multiplied by dt.
	float Approach(float current, float target, float rate) {
		return current + (target - current) * (1.0f - std::exp(-rate));
	}

	float WrapAngle(float a) {
		while (a > kPi) a -= 2.0f * kPi;
		while (a < -kPi) a += 2.0f * kPi;
		return a;
	}

	// Simplified Pacejka lateral curve: peak near 8 deg slip, gentle fall-off when sliding.
	float TyreCurve(float slip) {
		const float B = 9.0f, C = 1.45f;
		return std::sin(C * std::atan(B * slip));
	}

	// Engine torque curve (normalised).
	float TorqueCurve(float rpm, float idle, float limiter) {
		const float t = std::clamp((rpm - idle) / std::max(limiter - idle, 1.0f), 0.0f, 1.0f);
		if (t < 0.55f) return 0.72f + 0.28f * (t / 0.55f);
		return 1.0f - 0.18f * ((t - 0.55f) / 0.45f) * ((t - 0.55f) / 0.45f);
	}
}

// ---------------------------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------------------------
void DriveMode::ReadHandling(const json& j, Handling& h)
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
	const Handling& h = m_handling;
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

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------
void DriveMode::Configure(const json& g, SceneGraph& sceneGraph, const RoadNetwork* roads, const std::string& layoutPath,
	const CollisionWorld* collision)
{
	m_available = false;
	m_active = false;
	m_areas.clear();
	m_obstacles.clear();
	m_roads = (roads && !roads->Empty()) ? roads : nullptr;
	m_collision = (collision && !collision->Empty()) ? collision : nullptr;
	m_y = 0.0f;
	m_vyUp = 0.0f;
	m_airborne = false;
	m_groundPitch = m_groundRoll = 0.0f;
	m_handling = Handling();
	m_rig = CameraRig();
	m_rigWheels = false;
	if (!g.is_object()) return;

	auto root = sceneGraph.GetRoot();
	auto player = FindByName(root, g.value("player_node", std::string("PlayerCar")));
	if (!player) {
		std::cerr << "[DriveMode] player_node not found" << std::endl;
		return;
	}
	m_player = player;
	m_playerY = player->GetPosition().y;
	auto capture = [](RigNode& r, const std::shared_ptr<SceneNode>& n) {
		r.node = n;
		if (n) {
			r.restPos = n->GetPosition();
			r.restRot = n->GetOrientation();
			r.restScale = n->GetScale();
		}
	};
	m_model = RigNode();
	m_bodyMesh = RigNode();
	if (auto body = FindByName(root, g.value("body_node", std::string()))) {
		capture(m_model, body);
		// Wheel rig (glTF nodes inside the body model).
		auto bodyMesh = FindByName(body, "Body");
		int found = 0;
		for (int i = 0; i < 4; ++i) {
			auto w = FindByName(body, kWheelNames[i]);
			capture(m_wheelNodes[i], w);
			found += w ? 1 : 0;
		}
		if (bodyMesh && found == 4) {
			capture(m_bodyMesh, bodyMesh);
			m_rigWheels = true;

			const glm::vec3 s = m_model.restScale;
			for (int i = 0; i < 4; ++i) {
				m_wheelRest[i] = m_model.restPos + m_wheelNodes[i].restPos * s;
			}
			const float frontZ = 0.5f * (m_wheelRest[0].z + m_wheelRest[1].z);
			const float rearZ = 0.5f * (m_wheelRest[2].z + m_wheelRest[3].z);
			m_a = std::max(frontZ, 0.5f);
			m_b = std::max(-rearZ, 0.5f);
			m_track = std::abs(m_wheelRest[0].x - m_wheelRest[1].x);
			for (int i = 0; i < 4; ++i) m_wheelRadius[i] = std::max(m_wheelRest[i].y, 0.2f);
			for (int i = 0; i < 4; ++i) {
				const glm::quat q = m_wheelNodes[i].restRot;
				std::cout << "[DriveMode] " << kWheelNames[i] << " rest pos (" << m_wheelNodes[i].restPos.x << ", " << m_wheelNodes[i].restPos.y << ", "
					<< m_wheelNodes[i].restPos.z << ") rot (" << q.w << ", " << q.x << ", " << q.y << ", " << q.z << ") scale " << m_wheelNodes[i].restScale.x << std::endl;
			}
		}
	}
	if (!m_rigWheels) {
		// Legacy single-mesh car: nominal geometry.
		m_a = 1.36f; m_b = 1.36f; m_track = 1.6f;
		const float x = m_track * 0.5f;
		m_wheelRest[0] = glm::vec3(x, 0.33f, m_a); m_wheelRest[1] = glm::vec3(-x, 0.33f, m_a);
		m_wheelRest[2] = glm::vec3(x, 0.33f, -m_b); m_wheelRest[3] = glm::vec3(-x, 0.33f, -m_b);
	}
	if (g.contains("drivable_areas") && g["drivable_areas"].is_array()) {
		for (const json& a : g["drivable_areas"]) {
			if (a.is_array() && a.size() >= 4) {
				m_areas.push_back({ glm::vec2(a[0].get<float>(), a[1].get<float>()), glm::vec2(a[2].get<float>(), a[3].get<float>()) });
			}
		}
	}
	ReadHandling(g.value("handling", json::object()), m_handling);
	ReadCamera(g.value("camera", json::object()), m_rig);
	LoadLayout(layoutPath);

	const glm::vec3 p = player->GetPosition();
	m_spawnPos = glm::vec2(p.x, p.z);
	// Surface mode: the player node height is the ride offset above the ground; a spawn without a
	// height lands on the highest surface.
	m_spawnY = 1e4f;
	const glm::vec3 fwd = player->GetOrientation() * glm::vec3(0, 0, 1);
	m_spawnHeading = std::atan2(fwd.x, fwd.z);
	if (g.contains("spawn") && g["spawn"].is_object()) {
		const json& s = g["spawn"];
		if (s.contains("position") && s["position"].is_array() && s["position"].size() >= 3) {
			m_spawnPos = glm::vec2(s["position"][0].get<float>(), s["position"][2].get<float>());
			m_spawnY = s["position"][1].get<float>();
		}
		else if (s.contains("position") && s["position"].is_array() && s["position"].size() >= 2) {
			m_spawnPos = glm::vec2(s["position"][0].get<float>(), s["position"][1].get<float>());
		}
		m_spawnHeading = glm::radians(s.value("heading_deg", glm::degrees(m_spawnHeading)));
	}
	ResetToSpawn();
	ApplyTransforms();
	m_available = true;
	std::cout << "[DriveMode] Ready. " << (m_rigWheels ? "Wheel rig" : "Single-mesh body") << " a=" << m_a << " b=" << m_b
		<< " track=" << m_track << " areas=" << m_areas.size() << " obstacles=" << m_obstacles.size()
		<< (m_roads ? " (road network)" : "") << (m_collision ? " (collision surface)" : "") << std::endl;
}

void DriveMode::LoadLayout(const std::string& path)
{
	if (path.empty()) return;
	std::ifstream in(path);
	if (!in) return;
	const json layout = json::parse(in, nullptr, false);
	if (!layout.is_object() || !layout.contains("obstacles") || !layout["obstacles"].is_array()) return;
	for (const json& o : layout["obstacles"]) {
		Obstacle ob;
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
		m_obstacles.push_back(ob);
	}
}

float DriveMode::Speed01() const
{
	return std::clamp(glm::length(m_vel) / (m_handling.topSpeedKmh / 3.6f), 0.0f, 1.0f);
}

float DriveMode::DriftAngleDeg() const
{
	if (glm::length(m_vel) < 3.0f) return 0.0f;
	return glm::degrees(std::atan2(m_vy, std::abs(m_vx)));
}

void DriveMode::Teleport(const glm::vec3& pos, float heading)
{
	if (m_collision) {
		float y = pos.y;
		const glm::vec2 p(pos.x, pos.z);
		if (!SurfaceHeight(p, pos.y + 2.5f, 40.0f, y) && !SurfaceHeight(p, 1e4f, 2e4f, y)) y = pos.y;
		m_y = y;
		m_vyUp = 0.0f;
		m_airborne = false;
		m_airTime = 0.0f;
	}
	Teleport(glm::vec2(pos.x, pos.z), heading);
}

void DriveMode::Teleport(const glm::vec2& pos, float heading)
{
	if (m_collision) {
		// Keep the current deck when one exists under the new position.
		float y;
		if (SurfaceHeight(pos, m_y + 2.5f, 40.0f, y) || SurfaceHeight(pos, 1e4f, 2e4f, y)) m_y = y;
		m_vyUp = 0.0f;
		m_airborne = false;
	}
	m_pos = m_prevPos = pos;
	m_heading = heading;
	m_vel = glm::vec2(0.0f);
	m_vx = m_vy = m_yawRate = 0.0f;
	m_steer = m_steerAngle = 0.0f;
	m_ax = m_ay = 0.0f;
	m_rearSpin = 0.0f;
	m_drift = 0.0f;
	m_heave = m_heaveV = m_pitch = m_pitchV = m_roll = m_rollV = 0.0f;
	m_gear = 1;
	m_rpm = m_handling.rpmIdle;
	m_shiftTimer = 0.0f;
	m_shiftCooldown = 0.0f;
	m_directionHold = 0.0f;
	m_camInitialized = false;
	m_teleported = true;
	m_teleportFx = true;
	SampleWheels();
	if (m_collision) UpdateVertical(0.0f);
	ApplyTransforms();
}

void DriveMode::ResetToSpawn()
{
	if (m_collision) Teleport(glm::vec3(m_spawnPos.x, m_spawnY, m_spawnPos.y), m_spawnHeading);
	else Teleport(m_spawnPos, m_spawnHeading);
	m_nitro = 1.0f;
	m_nitroCooldown = 0.0f;
}

void DriveMode::RecoverToRoad()
{
	if (!m_roads) {
		ResetToSpawn();
		return;
	}
	if (m_collision) {
		// Stay on the current level: nearest carriageway within a deck height whose lane has
		// collision ground under it (generated sections cover only part of the network). After a
		// fall the current height is meaningless, so fall back to the last safe grounded pose.
		int edge = -1;
		float t = 0.0f;
		if (!m_airborne) m_roads->ClosestOnRoads3D(Position3(), 4.0f, nullptr, &edge, &t);
		if (edge >= 0) {
			float heading = m_heading;
			const glm::vec3 lane = m_roads->LanePosition(edge, t, m_heading, -1, heading);
			float y;
			if (SurfaceHeight(glm::vec2(lane.x, lane.z), lane.y + 2.5f, 6.0f, y)) {
				Teleport(lane, heading);
				return;
			}
		}
		if (m_hasSafePose) {
			Teleport(m_safePos, m_safeHeading);
			return;
		}
		ResetToSpawn();
		return;
	}
	glm::vec2 closest;
	int edge = -1;
	m_roads->ClosestOnRoads(m_pos, &closest, &edge);
	if (edge < 0) {
		ResetToSpawn();
		return;
	}
	const RoadNetwork::Edge& e = m_roads->Edges()[edge];
	glm::vec2 dir = glm::normalize(m_roads->Nodes()[e.b].pos - m_roads->Nodes()[e.a].pos);
	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	if (glm::dot(dir, fwd) < 0.0f) dir = -dir;
	// Driving-side lane: facing dir, the car's right is (-dir.y, dir.x).
	const glm::vec2 right(-dir.y, dir.x);
	Teleport(closest + right * (e.halfWidth * 0.3f * (m_roads->LeftHandTraffic() ? -1.0f : 1.0f)), std::atan2(dir.x, dir.y));
}

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
			m_pos = m_prevPos = glm::vec2(p.x, p.z);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Road surface and collision
// ---------------------------------------------------------------------------------------------
float DriveMode::GroundHeight(const glm::vec2& p) const
{
	if (m_roads) {
		if (!m_roads->IsOnRoad(p, 0.0f)) {
			return 0.15f; // curb + sidewalk (the building band beyond is a wall)
		}
	}
	// Low-frequency road roughness (patched asphalt, gentle dips) plus faint high-frequency noise.
	const float a = m_handling.bumpAmplitude;
	const float low = std::sin(p.x * 0.37f + 1.3f) * std::sin(p.y * 0.29f + 0.4f);
	const float mid = 0.5f * std::sin(p.x * 1.9f + p.y * 0.7f) * std::sin(p.y * 1.3f - p.x * 0.4f);
	return a * (low + mid) + a;
}

bool DriveMode::SurfaceHeight(const glm::vec2& p, float fromY, float maxDrop, float& outY, glm::vec3* outNormal) const
{
	if (!m_collision) return false;
	CollisionWorld::Hit hit;
	if (!m_collision->GroundBelow(glm::vec3(p.x, fromY, p.y), maxDrop, hit)) return false;
	outY = hit.position.y;
	if (outNormal) *outNormal = hit.normal;
	return true;
}

void DriveMode::SampleWheels()
{
	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	const glm::vec2 left(std::cos(m_heading), -std::sin(m_heading));
	for (int i = 0; i < 4; ++i) {
		const glm::vec2 wp = m_pos + left * m_wheelRest[i].x + fwd * m_wheelRest[i].z;
		WheelState& w = m_wheels[i];
		if (!m_collision) {
			w.contact = glm::vec3(wp.x, GroundHeight(wp), wp.y);
			m_wheelHit[i] = true;
			continue;
		}
		// From just above the current deck (decks above are >= ~5 m higher), down past a crest drop.
		float y;
		m_wheelHit[i] = SurfaceHeight(wp, m_y + 1.5f, 1.5f + (m_airborne ? 60.0f : 4.0f), y);
		if (!m_wheelHit[i]) y = m_y;
		// Fine roughness on top of the authored surface (same as the planar mode bumps).
		const float a = m_handling.bumpAmplitude;
		const float bump = a * (std::sin(wp.x * 0.37f + 1.3f) * std::sin(wp.y * 0.29f + 0.4f) +
			0.5f * std::sin(wp.x * 1.9f + wp.y * 0.7f) * std::sin(wp.y * 1.3f - wp.x * 0.4f));
		w.contact = glm::vec3(wp.x, y + bump, wp.y);
	}
	if (!m_collision) {
		for (int i = 0; i < 4; ++i) m_wheelRel[i] = m_wheels[i].contact.y;
	}
}

// Surface mode: fit the ground plane under the wheels, follow it while grounded (with a little
// extra stickiness over crests), otherwise fly ballistically and land. Updates the ground pitch and
// bank used for the body orientation and slope gravity, and the per-wheel residual for the springs.
void DriveMode::UpdateVertical(float h)
{
	int hits = 0;
	for (int i = 0; i < 4; ++i) hits += m_wheelHit[i] ? 1 : 0;
	const float L = std::max(m_a + m_b, 0.5f);
	const float T = std::max(m_track, 0.5f);
	float groundY = m_y;
	float pitch = m_groundPitch, roll = m_groundRoll;
	if (hits >= 3) {
		float g[4];
		for (int i = 0; i < 4; ++i) g[i] = m_wheelHit[i] ? m_wheels[i].contact.y : 0.0f;
		// A missing wheel takes the plane through the other three.
		if (!m_wheelHit[0]) g[0] = g[1] + g[2] - g[3];
		if (!m_wheelHit[1]) g[1] = g[0] + g[3] - g[2];
		if (!m_wheelHit[2]) g[2] = g[0] + g[3] - g[1];
		if (!m_wheelHit[3]) g[3] = g[1] + g[2] - g[0];
		const float front = 0.5f * (g[0] + g[1]), rear = 0.5f * (g[2] + g[3]);
		groundY = (front * m_b + rear * m_a) / L; // height at the CG
		pitch = std::atan2(front - rear, L);
		roll = std::atan2(0.5f * (g[0] + g[2]) - 0.5f * (g[1] + g[3]), T);
	}
	if (h <= 0.0f) {
		// Placement: snap to the ground.
		if (hits >= 3) { m_y = groundY; m_groundPitch = pitch; m_groundRoll = roll; }
	}
	else if (hits >= 3 && (!m_airborne || m_y <= groundY + 0.02f)) {
		// Grounded. Ground that falls away faster than gravity (x 2.5 stickiness) launches the car.
		const float follow = (groundY - m_y) / h;
		const float maxDropSpeed = m_vyUp - 2.5f * kG * h;
		if (!m_airborne && follow < maxDropSpeed && groundY < m_y - 0.05f) {
			m_airborne = true;
			m_airTime = 0.0f;
		}
		else {
			if (m_airborne) m_impact = std::max(m_impact, std::max(0.0f, -m_vyUp) * 0.5f); // landing
			m_airborne = false;
			m_vyUp = std::clamp(follow, -40.0f, 40.0f);
			m_y = groundY;
			// Orientation follows the road quickly but not instantly (no jitter at seams).
			const float k = 1.0f - std::exp(-h * 30.0f);
			m_groundPitch += (pitch - m_groundPitch) * k;
			m_groundRoll += (roll - m_groundRoll) * k;
		}
	}
	else {
		m_airborne = true;
	}
	if (m_airborne && h > 0.0f) {
		m_airTime += h;
		// Fell out of the world (off the end of a section, through a gap): back onto the road.
		if (m_airTime > 3.0f) {
			std::cout << "[DriveMode] No ground for 3 s at (" << m_pos.x << ", " << m_y << ", " << m_pos.y << "): recovering" << std::endl;
			m_airTime = 0.0f;
			m_recoverRequested = true;
		}
		m_vyUp -= kG * h;
		m_y += m_vyUp * h;
		// Level out slowly in the air.
		m_groundPitch *= 1.0f - std::min(1.0f, h * 0.8f);
		m_groundRoll *= 1.0f - std::min(1.0f, h * 0.8f);
		if (hits >= 3 && m_y <= groundY) {
			m_impact = std::max(m_impact, -m_vyUp * 0.5f);
			m_y = groundY;
			m_vyUp = 0.0f;
			m_airborne = false;
		}
	}
	// Last safe pose for recovery: grounded on all wheels, moving, level-ish, for a while.
	if (h > 0.0f) {
		const bool safe = !m_airborne && hits == 4 && std::abs(m_groundRoll) < 0.1f;
		m_safeTimer = safe ? m_safeTimer + h : 0.0f;
		if (m_safeTimer > 0.5f) {
			m_safeTimer = 0.0f;
			m_safePos = glm::vec3(m_pos.x, m_y, m_pos.y);
			m_safeHeading = m_heading;
			m_hasSafePose = true;
		}
	}
	// Residual road height under each wheel relative to the car's ground plane (springs).
	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	for (int i = 0; i < 4; ++i) {
		const float plane = m_y + std::tan(m_groundPitch) * m_wheelRest[i].z + std::tan(m_groundRoll) * m_wheelRest[i].x;
		m_wheelRel[i] = m_wheelHit[i] && !m_airborne ? std::clamp(m_wheels[i].contact.y - plane, -0.15f, 0.15f) : -0.08f;
		m_wheels[i].contact.y = m_wheelHit[i] ? m_wheels[i].contact.y : m_y;
	}
	(void)fwd;
}

glm::vec2 DriveMode::ResolveCircle(glm::vec2& c, float radius) const
{
	glm::vec2 total(0.0f);
	// Surface mode: the collision mesh is authoritative (planar street bands would merge stacked decks).
	if (m_roads && !m_collision) {
		glm::vec2 n;
		const float clearance = m_roads->Clearance(c, &n);
		if (clearance < radius) {
			const glm::vec2 push = -n * (radius - clearance);
			c += push;
			total += push;
		}
	}
	else if (!m_areas.empty()) {
		bool inside = false;
		for (const Area& a : m_areas) {
			if (c.x >= a.min.x && c.x <= a.max.x && c.y >= a.min.y && c.y <= a.max.y) { inside = true; break; }
		}
		if (!inside) {
			float best = 1e30f;
			glm::vec2 bestP = c;
			for (const Area& a : m_areas) {
				const glm::vec2 q = glm::clamp(c, a.min, a.max);
				const float d = glm::length(q - c);
				if (d < best) { best = d; bestP = q; }
			}
			total += bestP - c;
			c = bestP;
		}
	}
	if (m_collision) {
		// Wall triangles at body height (above curbs, below a tunnel ceiling).
		// Contacts only up to the roof line (~1.6 m): overhead structure (a ramp parapet passing over
		// the road) must not catch the wider-than-tall sphere.
		glm::vec3 centre(c.x, m_y + radius + 0.25f, c.y);
		const glm::vec3 push3 = m_collision->ResolveSphere(centre, radius, CollisionWorld::Wall, nullptr, 2, m_y + m_handling.roofHeight);
		const glm::vec2 push(push3.x, push3.z);
		c += push;
		total += push;
	}
	for (const Obstacle& o : m_obstacles) {
		if (!o.box) {
			const glm::vec2 d = c - o.center;
			const float dist = glm::length(d);
			const float minDist = o.radius + radius;
			if (dist < minDist) {
				const glm::vec2 n = dist > 1e-4f ? d / dist : glm::vec2(1, 0);
				const glm::vec2 push = n * (minDist - dist);
				c += push;
				total += push;
			}
		}
		else {
			const float cs = std::cos(o.yaw), sn = std::sin(o.yaw);
			const glm::vec2 d = c - o.center;
			const glm::vec2 local(d.x * cs + d.y * sn, -d.x * sn + d.y * cs);
			const glm::vec2 q = glm::clamp(local, -o.half, o.half);
			const glm::vec2 delta = local - q;
			float dist = glm::length(delta);
			if (dist < radius) {
				glm::vec2 nLocal;
				if (dist > 1e-4f) nLocal = delta / dist;
				else {
					const glm::vec2 pen = o.half - glm::abs(local);
					nLocal = pen.x < pen.y ? glm::vec2(local.x >= 0 ? 1.0f : -1.0f, 0.0f) : glm::vec2(0.0f, local.y >= 0 ? 1.0f : -1.0f);
					dist = -std::min(pen.x, pen.y);
				}
				const glm::vec2 nWorld(nLocal.x * cs - nLocal.y * sn, nLocal.x * sn + nLocal.y * cs);
				const glm::vec2 push = nWorld * (radius - dist);
				c += push;
				total += push;
			}
		}
	}
	return total;
}

void DriveMode::Collide(glm::vec2& pos, glm::vec2& vel)
{
	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	for (int iter = 0; iter < 3; ++iter) {
		glm::vec2 front = pos + fwd * kCarCircleOffset;
		glm::vec2 rear = pos - fwd * kCarCircleOffset;
		const glm::vec2 pf = ResolveCircle(front, kCarRadius);
		const glm::vec2 pr = ResolveCircle(rear, kCarRadius);
		if (glm::dot(pf, pf) < 1e-8f && glm::dot(pr, pr) < 1e-8f) break;
		const glm::vec2 push = glm::dot(pf, pf) > glm::dot(pr, pr) ? pf : pr;
		pos += (pf + pr) * 0.5f + push * 0.5f;
		const glm::vec2 n = glm::normalize(push);
		const float into = glm::dot(vel, n);
		if (into < 0.0f) {
			// Remove the velocity into the wall (small bounce) and scrape along it; the impact
			// also yaws the car away from the wall end that hit (front vs rear circle).
			vel -= n * into * 1.2f;
			vel *= 0.985f;
			m_impact = std::max(m_impact, -into);
			const float lever = glm::dot(pf, pf) > glm::dot(pr, pr) ? 1.0f : -1.0f;
			const glm::vec2 left(std::cos(m_heading), -std::sin(m_heading));
			m_yawRate += lever * glm::dot(n, left) * std::min(-into, 15.0f) * 0.12f;
			m_yawRate *= 0.8f;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Dynamics
// ---------------------------------------------------------------------------------------------
float DriveMode::GearRatio(int gear) const
{
	const std::vector<float>& r = m_handling.gearRatios;
	return r[static_cast<size_t>(std::clamp(gear, 1, static_cast<int>(r.size())) - 1)];
}

// Pedal-driven direction (NFS style): hold brake at a standstill to engage reverse; in reverse the
// brake pedal drives backwards and the throttle brakes, held at a standstill it selects first again.
// Only a deliberate hold at (near) zero speed changes direction, so a crash bounce or a brake held
// through a stop never flips the car into reverse on its own.
void DriveMode::SelectDirection(float h, const Input& in)
{
	const bool stopped = std::abs(m_vx) < 1.0f;
	const bool wantReverse = m_gear >= 1 && in.brake > 0.1f && in.throttle < 0.1f;
	const bool wantForward = m_gear == -1 && in.throttle > 0.1f && in.brake < 0.1f;
	if (stopped && (wantReverse || wantForward)) {
		m_directionHold += h;
		if (m_directionHold >= m_handling.reverseEngageDelay) {
			m_gear = wantReverse ? -1 : 1;
			m_directionHold = 0.0f;
			m_shiftCooldown = 0.3f;
			m_shifted = true;
		}
	}
	else {
		m_directionHold = 0.0f;
	}
	// Rolling forward under its own speed while in reverse (or after a teleport): back to a forward gear.
	if (m_gear == -1 && m_vx > 3.0f) m_gear = 1;
	if (m_gear == 0 || m_gear > GearCount()) m_gear = std::clamp(m_gear, 1, GearCount());
	// At a standstill a forward gear is always first (e.g. after stopping hard against a wall).
	if (m_gear > 1 && stopped && m_shiftTimer <= 0.0f) m_gear = 1;
}

void DriveMode::UpdateGearbox(float h, const Input& in, float wheelSpeed)
{
	const Handling& hd = m_handling;
	const float topSpeed = hd.topSpeedKmh / 3.6f;
	const int gears = GearCount();
	// Final drive: the top gear reaches the limiter slightly above top speed.
	const float finalDrive = hd.rpmLimiter * 2.0f * kPi / 60.0f * m_wheelRadius[2] / (topSpeed * 1.04f * GearRatio(gears));
	auto rpmFor = [&](int gear, float v) {
		return std::abs(v) / m_wheelRadius[2] * 60.0f / (2.0f * kPi) * GearRatio(gear) * finalDrive;
	};
	auto engineRpm = [&](int gear) { return hd.rpmIdle + rpmFor(gear, wheelSpeed); };
	const bool shifting = m_shiftTimer > 0.0f;
	m_shiftTimer = std::max(0.0f, m_shiftTimer - h);
	m_shiftCooldown = std::max(0.0f, m_shiftCooldown - h);

	// Automatic shifting (forward gears only):
	//   up        at the redline under throttle (not while the rear tyres are spinning up)
	//   kickdown  floored throttle while the lower gear still has room below ~80% of the redline
	//   braking   step down as speed falls, keeping the engine in its band for the exit
	//   lugging   part throttle / coasting below ~2600 rpm
	// A short lockout after every shift prevents hunting between two gears.
	if (m_gear >= 1 && !shifting && m_shiftCooldown <= 0.0f) {
		const float rpmNow = engineRpm(m_gear);
		const float lowerRpm = m_gear > 1 ? engineRpm(m_gear - 1) : 1e9f;
		const bool up = m_gear < gears && in.throttle > 0.1f && m_vx > 3.0f && m_rearSpin < 0.5f &&
			(rpmNow >= hd.rpmRedline || m_limiter);
		const bool kickdown = in.throttle > 0.9f && lowerRpm < hd.rpmRedline * 0.8f;
		const bool braking = in.brake > 0.1f && m_vx > 0.3f && lowerRpm < hd.rpmRedline * 0.7f;
		const bool lugging = in.throttle < 0.9f && rpmNow < 2600.0f && lowerRpm < hd.rpmRedline * 0.75f;
		if (up) {
			++m_gear;
			m_shiftTimer = hd.shiftTime;
			m_shiftCooldown = 0.35f;
			m_shifted = true;
		}
		else if (m_gear > 1 && (kickdown || braking || lugging)) {
			--m_gear;
			m_shiftTimer = hd.shiftTime * 0.6f;
			m_shiftCooldown = 0.3f;
			m_shifted = true;
		}
	}
	float target = m_gear == -1 ? hd.rpmIdle + std::abs(m_vx) / (hd.reverseMaxKmh / 3.6f) * 4500.0f
		: hd.rpmIdle + rpmFor(m_gear, wheelSpeed);
	// Clutch slip at launch.
	target = std::max(target, hd.rpmIdle + in.throttle * 3000.0f * std::clamp(1.0f - std::abs(m_vx) / 6.0f, 0.0f, 1.0f));
	if (shifting) target *= 0.8f;
	m_limiter = target >= hd.rpmLimiter - 1.0f && in.throttle > 0.1f;
	if (m_limiter) target = hd.rpmLimiter - (std::fmod(m_time * 22.0f, 1.0f) < 0.5f ? 0.0f : 380.0f);
	m_rpm = Approach(m_rpm, std::min(target, hd.rpmLimiter), h * (shifting ? 14.0f : 10.0f));
}

void DriveMode::Step(float h, const Input& in)
{
	const Handling& hd = m_handling;
	const float m = hd.mass;
	const float L = m_a + m_b;
	const float Iz = m * m_a * m_b * 1.1f;
	const float topSpeed = hd.topSpeedKmh / 3.6f;
	const float speed = glm::length(m_vel);
	const float speed01 = std::clamp(speed / topSpeed, 0.0f, 1.0f);
	m_time += h;
	m_throttle = in.throttle;

	// --- Local velocity (forward, left).
	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	const glm::vec2 left(std::cos(m_heading), -std::sin(m_heading));
	m_vx = glm::dot(m_vel, fwd);
	m_vy = glm::dot(m_vel, left);
	const float vxAbs = std::max(std::abs(m_vx), 0.5f);
	const float beta = speed > 2.0f ? std::atan2(m_vy, vxAbs) : 0.0f; // body slip, + = velocity left of the nose

	// --- Nitrous.
	m_nitroActive = in.nitrous && m_nitro > 0.0f && in.throttle > 0.05f && m_vx > 1.0f;
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
	m_steerAngle = delta;

	// --- Loads with longitudinal and lateral transfer.
	const float Fz = m * kG;
	float FzF = Fz * m_b / L - m * m_ax * hd.cgHeight / L;
	float FzR = Fz * m_a / L + m * m_ax * hd.cgHeight / L;
	FzF = std::max(FzF, Fz * 0.15f);
	FzR = std::max(FzR, Fz * 0.15f);
	const float latTransfer = m * m_ay * hd.cgHeight / std::max(m_track, 0.5f);

	// --- Longitudinal forces. Direction first, so a reverse/forward selection acts this step.
	SelectDirection(h, in);
	const float ratio1 = GearRatio(1);
	const float finalDriveForce = hd.acceleration * m; // first-gear drive capacity
	const float curve = TorqueCurve(m_rpm, hd.rpmIdle, hd.rpmLimiter);
	float driveForce = 0.0f;
	const bool shifting = m_shiftTimer > 0.0f;
	const bool burnout = in.throttle > 0.5f && in.brake > 0.5f && std::abs(m_vx) < 3.0f;
	if (m_gear >= 1 && in.throttle > 0.0f) {
		driveForce = in.throttle * finalDriveForce * curve * GearRatio(m_gear) / ratio1;
		if (m_nitroActive) driveForce *= hd.nitroThrust;
		if (shifting) driveForce *= 0.12f;
		if (m_limiter) driveForce *= 0.25f;
	}
	else if (m_gear == -1 && in.brake > 0.1f && m_vx > -hd.reverseMaxKmh / 3.6f) {
		driveForce = -in.brake * finalDriveForce * 0.45f;
	}
	const float tractionR = hd.tireGripLong * FzR;
	float FxR = driveForce;
	float spinTarget = 0.0f;
	if (std::abs(driveForce) > tractionR) {
		spinTarget = std::clamp((std::abs(driveForce) - tractionR) / tractionR * 1.6f, 0.0f, 1.0f);
		FxR = std::copysign(tractionR * 0.9f, driveForce);
	}
	// Brake pedal: S in forward gears, W in reverse. Brakes always oppose the motion.
	const float brakePedal = m_gear == -1 ? in.throttle : in.brake;
	float brakeF = 0.0f, brakeR = 0.0f;
	if (brakePedal > 0.0f && (std::abs(m_vx) > 0.3f || burnout)) {
		const float total = std::min(brakePedal * hd.braking * m, hd.tireGripLong * (FzF + FzR));
		brakeF = total * 0.68f;
		brakeR = burnout ? 0.0f : total * 0.32f;
	}
	if (burnout) {
		// Front brakes hold the car, rear wheels spin up: smoke show.
		spinTarget = std::max(spinTarget, in.throttle);
		FxR = 0.0f;
	}
	if (in.handbrake && std::abs(m_vx) > 0.5f) {
		FxR = -std::copysign(hd.tireGripLong * FzR * 0.75f, m_vx); // rear locked, sliding
		spinTarget = 0.0f;
	}
	m_rearSpin = Approach(m_rearSpin, spinTarget, h * (spinTarget > m_rearSpin ? 10.0f : 4.0f));
	const float sgnVx = m_vx >= 0.0f ? 1.0f : -1.0f;
	const float FxF = -sgnVx * brakeF;
	FxR += -sgnVx * brakeR;
	const float drag = (topSpeed > 1.0f ? (finalDriveForce * 0.92f * GearRatio(GearCount()) / ratio1 - 220.0f) / (topSpeed * topSpeed) : 0.4f)
		* (m_nitroActive ? 1.0f / (hd.nitroTopSpeed * hd.nitroTopSpeed) : 1.0f);
	const float resist = drag * m_vx * std::abs(m_vx) + (std::abs(m_vx) > 0.1f ? sgnVx * 220.0f : 0.0f);

	// --- Lateral tyre forces (combined slip via friction ellipse).
	const float driftScale = glm::mix(1.0f, hd.driftGripScale, m_drift);
	const float alphaF = std::atan2(m_vy + m_a * m_yawRate, vxAbs) - delta * sgnVx;
	const float alphaR = std::atan2(m_vy - m_b * m_yawRate, vxAbs);
	auto ellipse = [](float fx, float fxMax) { const float u = fxMax > 1.0f ? fx / fxMax : 0.0f; return std::sqrt(std::max(0.08f, 1.0f - u * u)); };
	const float FyF = -hd.tireGrip * FzF * TyreCurve(alphaF) * ellipse(FxF, hd.tireGripLong * FzF);
	const float rearLong = in.handbrake ? 0.72f : std::abs(FxR) / std::max(tractionR, 1.0f);
	const float FyR = -hd.tireGrip * 1.2f * FzR * driftScale * TyreCurve(alphaR) * std::sqrt(std::max(0.12f, 1.0f - rearLong * rearLong)) * (1.0f - 0.35f * m_rearSpin);

	// --- Integrate (local frame).
	const float cd = std::cos(delta), sd = std::sin(delta);
	float ax = (FxR + FxF * cd - FyF * sd - resist) / m + m_vy * m_yawRate;
	float ay = (FyF * cd + FxF * sd + FyR) / m - m_vx * m_yawRate;
	float yawAcc = (m_a * (FyF * cd + FxF * sd) - m_b * FyR) / Iz;

	// Surface mode: gravity along the road (grade) and across it (bank). No traction in the air.
	if (m_collision) {
		if (!m_airborne) {
			ax -= kG * std::sin(m_groundPitch);
			ay -= kG * std::sin(m_groundRoll);
		}
		else {
			ax = -resist / m + m_vy * m_yawRate;
			ay = -m_vx * m_yawRate;
			yawAcc = 0.0f;
		}
	}
	// Drift assist: hold the drift angle selected by the steering, keep speed through the drift.
	if (m_drift > 0.01f && speed > 6.0f) {
		const float toward = std::clamp(-in.steer * -driftDir, -1.0f, 1.0f); // + = steering into the drift
		// Steering into the drift holds/tightens it; neutral or countersteer straightens out.
		const float target = driftDir * glm::radians(hd.maxDriftAngle) * std::clamp(0.1f + 0.9f * toward, 0.0f, 1.0f);
		yawAcc += -hd.driftAssist * m_drift * 9.0f * (target - beta);
		// Steering released or countersteered: kill the rotation so the car straightens crisply.
		if (toward < 0.15f) yawAcc -= 3.5f * m_drift * (m_yawRate - m_vx * std::tan(delta) / L);
		ax += m_drift * hd.driftAssist * in.throttle * 2.2f * std::cos(beta);
	}
	// Slip-rate damping: no pendulum overshoot when the rear steps out (strongest in drifts).
	const float betaRate = (beta - m_prevBeta) / h;
	m_prevBeta = beta;
	if (speed > 5.0f) yawAcc += (0.6f + 2.4f * m_drift * hd.driftAssist) * std::clamp(betaRate, -6.0f, 6.0f);
	// Never spin out (drift or grip): strong correction toward the velocity past the limit.
	{
		const float limit = glm::radians(hd.maxDriftAngle + 12.0f);
		if (speed > 5.0f && std::abs(beta) > limit) yawAcc += 25.0f * (std::abs(beta) - limit) * (beta > 0.0f ? 1.0f : -1.0f) - 2.0f * m_yawRate;
	}
	// Grip-mode stability (arcade ESC): catch unintended oversteer outside drift mode.
	if (speed > 5.0f) yawAcc += 14.0f * (1.0f - m_drift) * std::clamp(beta, -0.5f, 0.5f) * std::clamp((std::abs(beta) - 0.03f) / 0.08f, 0.0f, 1.0f);
	// Yaw damping in grip driving when the steering is centred.
	if (m_drift < 0.2f && std::abs(in.steer) < 0.1f) yawAcc -= hd.yawDamping * 4.0f * (m_yawRate - m_vx * std::tan(delta) / L);

	m_vx += ax * h;
	m_vy += ay * h;
	m_yawRate += yawAcc * h;
	// Settle at rest: no pedals, or the brake pedal held (the car holds still instead of creeping).
	if (speed < 0.3f && ((in.throttle == 0.0f && in.brake == 0.0f) || (brakePedal > 0.1f && std::abs(driveForce) < 1.0f))) { m_vx = 0.0f; m_vy = 0.0f; }
	if (burnout) m_vx = Approach(m_vx, 0.0f, h * 8.0f);

	// Arcade drift turn (NFS 2015): while drifting, the velocity swings toward the nose at a fixed
	// rate (speed preserved), so the car carries its slide around 90-degree corners; the yaw assist
	// above keeps the drift angle, which keeps the nose rotating.
	if (m_drift > 0.01f && m_vx > 6.0f) {
		const float sp = std::sqrt(m_vx * m_vx + m_vy * m_vy);
		const float b = std::atan2(m_vy, m_vx);
		const float steerInto = std::clamp(in.steer * m_driftSign, 0.0f, 1.0f);
		const float rate = hd.driftAssist * m_drift * (1.1f + 0.9f * in.throttle) * (0.25f + 0.75f * steerInto);
		const float nb = b - std::copysign(std::min(std::abs(b), rate * h), b);
		m_vx = sp * std::cos(nb);
		m_vy = sp * std::sin(nb);
	}
	// Low speed: blend to the kinematic bicycle model (slip angles are meaningless near zero).
	const float k = std::clamp((std::abs(m_vx) - 1.0f) / 3.0f, 0.0f, 1.0f);
	const float rKin = m_vx * std::tan(delta) / L;
	m_yawRate = glm::mix(rKin, m_yawRate, k);
	m_vy = glm::mix(0.0f, m_vy, k);

	m_heading = WrapAngle(m_heading + m_yawRate * h);
	const glm::vec2 fwd2(std::sin(m_heading), std::cos(m_heading));
	const glm::vec2 left2(std::cos(m_heading), -std::sin(m_heading));
	m_vel = fwd2 * m_vx + left2 * m_vy;
	glm::vec2 next = m_pos + m_vel * h;
	Collide(next, m_vel);
	m_pos = next;
	m_vx = glm::dot(m_vel, fwd2);
	m_vy = glm::dot(m_vel, left2);

	// Filtered accelerations for load transfer and the sprung body.
	m_ax = Approach(m_ax, ax - m_vy * m_yawRate, h * 12.0f);
	m_ay = Approach(m_ay, ay + m_vx * m_yawRate, h * 12.0f);

	// --- Wheel state for effects, audio and the rig.
	const float wheelSpeedR = m_vx + (in.handbrake ? -m_vx : 0.0f) + m_rearSpin * 22.0f * (m_gear == -1 ? -1.0f : 1.0f);
	UpdateGearbox(h, in, std::abs(m_vx) + m_rearSpin * 22.0f);
	const float slipF = std::clamp(std::abs(alphaF) / 0.35f, 0.0f, 1.0f);
	const float slipR = std::clamp(std::abs(alphaR) / 0.3f, 0.0f, 1.0f);
	for (int i = 0; i < 4; ++i) {
		WheelState& w = m_wheels[i];
		const glm::vec3& r = m_wheelRest[i];
		const glm::vec2 offset = left2 * r.x + fwd2 * r.z;
		const glm::vec2 wp = m_pos + offset;
		w.contact = glm::vec3(wp.x, w.contact.y, wp.y);
		const glm::vec2 rotVel = m_yawRate * (left2 * r.z - fwd2 * r.x); // d(offset)/dt for yaw toward +X
		w.velocity = glm::vec3(m_vel.x + rotVel.x, 0.0f, m_vel.y + rotVel.y);
		const bool rear = i >= 2;
		const float side = (r.x > 0.0f ? 1.0f : -1.0f); // left wheels unload in right-hand... lateral transfer
		w.load = (rear ? FzR : FzF) * 0.5f - side * latTransfer * 0.5f;
		w.wheelspin = rear ? m_rearSpin : 0.0f;
		const float lockSlide = rear && in.handbrake && std::abs(m_vx) > 3.0f ? 0.8f : 0.0f;
		w.slip = std::clamp((rear ? slipR : slipF) * std::clamp(speed / 8.0f, 0.0f, 1.0f) + w.wheelspin + lockSlide, 0.0f, 1.0f);
		w.steerAngle = rear ? 0.0f : delta;
		const float rollSpeed = rear ? wheelSpeedR : m_vx;
		w.spinAngle = std::fmod(w.spinAngle + rollSpeed / m_wheelRadius[i] * h, 2.0f * kPi);
	}
	m_slip = std::clamp(std::max({ m_wheels[2].slip, m_wheels[3].slip, slipF * 0.6f }), 0.0f, 1.0f);
	SampleWheels();
	if (m_collision) {
		UpdateVertical(h);
		for (int i = 0; i < 4; ++i) {
			m_wheels[i].grounded = !m_airborne && m_wheelHit[i];
			if (m_airborne) m_wheels[i].slip = 0.0f;
		}
	}

	// Engine load (shown as BOOST): throttle x rpm with spool lag; no turbo is simulated.
	const float rpm01 = std::clamp((m_rpm - hd.rpmIdle) / (hd.rpmLimiter - hd.rpmIdle), 0.0f, 1.0f);
	m_load = Approach(m_load, in.throttle * (0.25f + 0.75f * rpm01) * (shifting ? 0.3f : 1.0f), h * (in.throttle > m_load ? 3.0f : 6.0f));
}

void DriveMode::UpdateSuspension(float h)
{
	const Handling& hd = m_handling;
	// Road under each wheel drives heave/pitch/roll targets together with the load transfer.
	float g[4];
	for (int i = 0; i < 4; ++i) g[i] = m_wheelRel[i];
	const float L = m_a + m_b;
	const float front = 0.5f * (g[0] + g[1]), rear = 0.5f * (g[2] + g[3]);
	const float leftH = 0.5f * (g[0] + g[2]), rightH = 0.5f * (g[1] + g[3]);
	const float heaveT = 0.25f * (g[0] + g[1] + g[2] + g[3]);
	const float pitchT = std::atan2(front - rear, L) + std::clamp(m_ax * 0.0032f, -0.05f, 0.04f);       // squat / dive
	const float rollT = std::atan2(leftH - rightH, std::max(m_track, 0.5f)) + std::clamp(m_ay * 0.0042f, -0.07f, 0.07f); // body leans out
	const float w = 2.0f * kPi * hd.suspensionFreq;
	const float z = hd.suspensionDamping;
	auto spring = [&](float& x, float& v, float target, float wScale) {
		const float ww = w * wScale;
		v += (ww * ww * (target - x) - 2.0f * z * ww * v) * h;
		x += v * h;
	};
	spring(m_heave, m_heaveV, heaveT, 1.15f);
	spring(m_pitch, m_pitchV, pitchT, 1.0f);
	spring(m_roll, m_rollV, rollT, 1.1f);
	for (int i = 0; i < 4; ++i) {
		const glm::vec3& r = m_wheelRest[i];
		const float bodyHub = r.y + m_heave + std::sin(m_pitch) * r.z + std::sin(m_roll) * r.x;
		m_wheels[i].suspension = bodyHub - (g[i] + m_wheelRadius[i]);
		m_wheels[i].grounded = m_wheels[i].suspension < 0.12f && m_wheelHit[i] && !m_airborne;
	}
}

void DriveMode::ApplyTransforms()
{
	if (auto player = m_player.lock()) {
		const glm::vec3 pos(m_pos.x, BaseY() + PlayerYOffset(), m_pos.y);
		glm::quat q = glm::angleAxis(m_heading, glm::vec3(0, 1, 0));
		// Surface mode: the whole car follows the road grade and bank (same axes as the body below).
		if (m_collision) q = q * glm::angleAxis(m_groundRoll, glm::vec3(0, 0, 1)) * glm::angleAxis(-m_groundPitch, glm::vec3(1, 0, 0));
		player->SetLocalTRS(pos, q, player->GetScale());
	}
	// Body: pitch about X (nose up = negative rotation, +Z forward), roll about Z (right side down).
	const glm::quat bodyQ = glm::angleAxis(m_roll, glm::vec3(0, 0, 1)) * glm::angleAxis(-m_pitch, glm::vec3(1, 0, 0));
	const glm::vec3 pivot(0.0f, 0.45f, 0.0f);
	if (m_rigWheels) {
		if (auto body = m_bodyMesh.node.lock()) {
			const glm::vec3 rest = m_bodyMesh.restPos;
			const glm::vec3 p = pivot + bodyQ * (rest - pivot) + glm::vec3(0.0f, m_heave, 0.0f);
			body->SetLocalTRS(p, bodyQ * m_bodyMesh.restRot, m_bodyMesh.restScale);
		}
		const glm::vec3 s = m_model.restScale;
		for (int i = 0; i < 4; ++i) {
			auto wn = m_wheelNodes[i].node.lock();
			if (!wn) continue;
			const WheelState& w = m_wheels[i];
			// Hub on the road (clamped to suspension travel relative to the sprung body), low-passed so
			// road noise does not make the wheels jitter.
			const float bodyHub = m_wheelRest[i].y + m_heave + std::sin(m_pitch) * m_wheelRest[i].z + std::sin(m_roll) * m_wheelRest[i].x;
			const float hubTarget = std::clamp(m_wheelRel[i] + m_wheelRadius[i], bodyHub - 0.09f, bodyHub + 0.09f);
			(void)w;
			if (m_teleported || !m_visInit) m_visHub[i] = hubTarget;
			m_visHub[i] += (hubTarget - m_visHub[i]) * (1.0f - std::exp(-m_visDt * 25.0f));
			const float hubY = m_visHub[i];
			// Visual steer follows the physics angle smoothly (assist terms jitter with body slip).
			m_visSteer[i] += (w.steerAngle - m_visSteer[i]) * (1.0f - std::exp(-m_visDt * 14.0f));
			// Spin: advance by the true rotation but cap the per-frame step (no strobing / wagon-wheel
			// aliasing at speed; motion blur conveys the rest).
			float dSpin = w.spinAngle - m_visSpinPrev[i];
			while (dSpin > 3.14159265f) dSpin -= 6.2831853f;
			while (dSpin < -3.14159265f) dSpin += 6.2831853f;
			m_visSpinPrev[i] = w.spinAngle;
			m_visSpin[i] = std::fmod(m_visSpin[i] + std::clamp(dSpin, -0.42f, 0.42f), 6.2831853f);
			glm::vec3 local = m_wheelNodes[i].restPos;
			local.y += (hubY - m_wheelRest[i].y) / std::max(s.y, 1e-4f);
			static const float testSpin = [] { const char* v = std::getenv("NOX_TEST_WHEELSPIN"); return v ? static_cast<float>(std::atof(v)) : -1000.0f; }();
			const float spin = testSpin > -999.0f ? testSpin : m_visSpin[i];
			const glm::quat q = glm::angleAxis(m_visSteer[i], glm::vec3(0, 1, 0)) * glm::angleAxis(spin, glm::vec3(1, 0, 0));
			wn->SetLocalTRS(local, q * m_wheelNodes[i].restRot, m_wheelNodes[i].restScale);
		}
		m_visInit = true;
	}
	else if (auto body = m_model.node.lock()) {
		body->SetLocalTRS(m_model.restPos + glm::vec3(0.0f, m_heave, 0.0f), bodyQ * m_model.restRot, m_model.restScale);
	}
}

// ---------------------------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------------------------
void DriveMode::UpdateCamera(float dt, Camera& camera, float aspect)
{
	const CameraRig& rig = m_rig;
	const float speed = glm::length(m_vel);
	const float speed01 = Speed01();
	const glm::vec3 carPos(m_pos.x, BaseY() + PlayerYOffset(), m_pos.y);
	// Follow direction: the heading blended with the velocity direction, so drifts show the car's
	// flank like NFS 2015 while straight driving stays locked behind the car. Reversing keeps the
	// camera behind the nose (no flip).
	float followHeading = m_heading;
	if (speed > 4.0f && m_vx > 0.0f) {
		const float velHeading = std::atan2(m_vel.x, m_vel.y);
		followHeading = m_heading + WrapAngle(velHeading - m_heading) * rig.velocityFollow * std::clamp((speed - 4.0f) / 8.0f, 0.0f, 1.0f);
	}
	// NOX_CAM_ORBIT=<deg>: validation captures from the side / front (rotates the chase rig).
	static const float orbit = [] { const char* v = std::getenv("NOX_CAM_ORBIT"); return v ? glm::radians(static_cast<float>(std::atof(v))) : 0.0f; }();
	followHeading += orbit;
	if (!m_camInitialized) m_camHeading = followHeading;
	m_camHeading = WrapAngle(m_camHeading + WrapAngle(followHeading - m_camHeading) * (1.0f - std::exp(-dt * (m_cameraMode == 2 ? 30.0f : rig.headingLag))));
	glm::vec3 camFwd(std::sin(m_camHeading), 0.0f, std::cos(m_camHeading));
	// Surface mode: the rig follows the road grade (the camera stays above the road behind the car
	// on climbs and descents).
	if (m_collision) camFwd = glm::normalize(camFwd + glm::vec3(0.0f, std::tan(std::clamp(m_groundPitch, -0.3f, 0.3f)), 0.0f));

	float distance = rig.distance, height = rig.height, lookAhead = rig.lookAhead, lookHeight = rig.lookHeight;
	if (m_cameraMode == 1) { distance *= 1.5f; height *= 1.45f; lookAhead *= 1.1f; lookHeight *= 1.4f; }
	// Restrained speed effects: slight pull-back, a little more under hard acceleration.
	distance += speed01 * rig.pullBack + std::clamp(m_ax * 0.025f, -0.12f, 0.3f);
	glm::vec3 desired = carPos - camFwd * distance + glm::vec3(0.0f, height, 0.0f);
	glm::vec3 target = carPos + camFwd * lookAhead + glm::vec3(0.0f, lookHeight, 0.0f);
	const glm::vec3 fwd(std::sin(m_heading), 0.0f, std::cos(m_heading));
	if (m_cameraMode == 2) {
		desired = carPos + fwd * 1.9f + glm::vec3(0.0f, 0.75f + m_heave, 0.0f);
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
		const glm::vec2 car(m_pos.x, m_pos.y);
		for (int i = 0; i < 6; ++i) {
			glm::vec2 probe = c;
			const glm::vec2 push = ResolveCircle(probe, 0.35f);
			if (glm::dot(push, push) < 1e-6f) break;
			c = glm::mix(c, car, 0.25f);
		}
		m_camPos.x = c.x;
		m_camPos.z = c.y;
	}
	glm::vec3 eye = m_camPos;
	if (speed01 > 0.6f) {
		const float a = (speed01 - 0.6f) * 0.012f;
		eye += glm::vec3(std::sin(m_time * 31.0f), std::sin(m_time * 47.0f + 1.3f), 0.0f) * a;
	}
	const float targetFov = rig.fov + speed01 * rig.fovSpeedGain + (m_nitroActive ? rig.fovNitro : 0.0f);
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
void DriveMode::Update(float dt, const Input& in, bool frozen, Camera* camera, float aspect)
{
	if (!m_active) return;
	dt = std::clamp(dt, 0.0f, 0.05f);
	m_impact = std::max(0.0f, m_impact - dt * 40.0f);
	m_prevPos = m_pos;
	if (frozen) {
		// Engine still revs (countdown launch control) but the car does not move.
		m_time += dt;
		m_throttle = in.throttle;
		m_rpm = Approach(m_rpm, m_handling.rpmIdle + in.throttle * 4200.0f, dt * 8.0f);
		m_limiter = false;
		m_load = Approach(m_load, in.throttle * 0.6f, dt * 4.0f);
		m_vel = glm::vec2(0.0f);
		m_vx = m_vy = m_yawRate = 0.0f;
		m_nitroActive = false;
		m_rearSpin = 0.0f;
		SampleWheels();
		if (m_collision) UpdateVertical(0.0f);
		for (int i = 0; i < 4; ++i) {
			m_wheels[i].velocity = glm::vec3(0.0f);
			m_wheels[i].slip = m_wheels[i].wheelspin = 0.0f;
		}
		UpdateSuspension(dt);
	}
	else {
		m_accum = std::min(m_accum + dt, 0.1f);
		while (m_accum >= kStep) {
			Step(kStep, in);
			UpdateSuspension(kStep);
			m_accum -= kStep;
		}
		if (m_recoverRequested) {
			m_recoverRequested = false;
			RecoverToRoad();
		}
	}
	m_visDt = dt;
	ApplyTransforms();
	if (camera && m_cameraMode != 3) UpdateCamera(dt, *camera, aspect);
}

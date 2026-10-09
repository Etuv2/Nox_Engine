#include "DriveMode.h"

#include "Camera.h"
#include "RoadNetwork.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include <SDL/SDL.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

using nlohmann::json;

namespace {
	constexpr float kPi = 3.14159265f;

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

	// Upper speed of each gear as a fraction of top speed (6-speed box).
	const float kGearTop[6] = { 0.22f, 0.38f, 0.53f, 0.68f, 0.84f, 1.0f };
	constexpr float kCarRadius = 0.95f;   // two circles along the body
	constexpr float kCarCircleOffset = 1.25f;
}

void DriveMode::ReadHandling(const json& j, Handling& h)
{
	if (!j.is_object()) return;
	h.topSpeedKmh = j.value("top_speed_kmh", h.topSpeedKmh);
	h.acceleration = j.value("acceleration", h.acceleration);
	h.braking = j.value("braking", h.braking);
	h.reverseMaxKmh = j.value("reverse_max_kmh", h.reverseMaxKmh);
	h.steerLockLow = j.value("steer_lock_low", h.steerLockLow);
	h.steerLockHigh = j.value("steer_lock_high", h.steerLockHigh);
	h.steerRate = j.value("steer_rate", h.steerRate);
	h.grip = j.value("grip", h.grip);
	h.driftGrip = j.value("drift_grip", h.driftGrip);
	h.driftYawGain = j.value("drift_yaw_gain", h.driftYawGain);
	h.nitroThrust = j.value("nitro_thrust", h.nitroThrust);
	h.nitroTopSpeed = j.value("nitro_top_speed", h.nitroTopSpeed);
	h.nitroDrain = j.value("nitro_drain", h.nitroDrain);
	h.nitroRecharge = j.value("nitro_recharge", h.nitroRecharge);
	h.nitroRechargeDelay = j.value("nitro_recharge_delay", h.nitroRechargeDelay);
	h.rpmIdle = j.value("rpm_idle", h.rpmIdle);
	h.rpmRedline = j.value("rpm_redline", h.rpmRedline);
	h.rpmLimiter = j.value("rpm_limiter", h.rpmLimiter);
	h.shiftTime = j.value("shift_time", h.shiftTime);
}

void DriveMode::ReadCamera(const json& j, CameraRig& c)
{
	if (!j.is_object()) return;
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
}

json DriveMode::SaveTuning() const
{
	const Handling& h = m_handling;
	const CameraRig& c = m_rig;
	json out;
	out["handling"] = {
		{"top_speed_kmh", h.topSpeedKmh}, {"acceleration", h.acceleration}, {"braking", h.braking},
		{"reverse_max_kmh", h.reverseMaxKmh}, {"steer_lock_low", h.steerLockLow}, {"steer_lock_high", h.steerLockHigh},
		{"steer_rate", h.steerRate}, {"grip", h.grip}, {"drift_grip", h.driftGrip}, {"drift_yaw_gain", h.driftYawGain},
		{"nitro_thrust", h.nitroThrust}, {"nitro_top_speed", h.nitroTopSpeed}, {"nitro_drain", h.nitroDrain},
		{"nitro_recharge", h.nitroRecharge}, {"nitro_recharge_delay", h.nitroRechargeDelay},
		{"rpm_idle", h.rpmIdle}, {"rpm_redline", h.rpmRedline}, {"rpm_limiter", h.rpmLimiter}, {"shift_time", h.shiftTime} };
	out["camera"] = {
		{"distance", c.distance}, {"height", c.height}, {"look_ahead", c.lookAhead}, {"look_height", c.lookHeight},
		{"fov", c.fov}, {"fov_speed_gain", c.fovSpeedGain}, {"fov_nitro", c.fovNitro}, {"pull_back", c.pullBack},
		{"stiffness", c.stiffness}, {"heading_lag", c.headingLag} };
	return out;
}

void DriveMode::Configure(const json& g, SceneGraph& sceneGraph, const RoadNetwork* roads, const std::string& layoutPath)
{
	m_available = false;
	m_active = false;
	m_areas.clear();
	m_obstacles.clear();
	m_roads = (roads && !roads->Empty()) ? roads : nullptr;
	m_handling = Handling();
	m_rig = CameraRig();
	if (!g.is_object()) return;

	auto root = sceneGraph.GetRoot();
	auto player = FindByName(root, g.value("player_node", std::string("PlayerCar")));
	if (!player) {
		std::cerr << "[DriveMode] player_node not found" << std::endl;
		return;
	}
	m_player = player;
	m_playerY = player->GetPosition().y;
	m_body.reset();
	if (auto body = FindByName(root, g.value("body_node", std::string()))) {
		m_body = body;
		m_bodyBasePos = body->GetPosition();
		m_bodyBaseRot = body->GetOrientation();
		m_bodyScale = body->GetScale();
	}
	if (g.contains("drivable_areas") && g["drivable_areas"].is_array()) {
		for (const json& a : g["drivable_areas"]) {
			if (a.is_array() && a.size() >= 4) {
				m_areas.push_back({ glm::vec2(a[0].get<float>(), a[1].get<float>()), glm::vec2(a[2].get<float>(), a[3].get<float>()) });
			}
		}
	}
	// Legacy key, now part of "handling".
	m_handling.topSpeedKmh = g.value("top_speed_kmh", m_handling.topSpeedKmh);
	ReadHandling(g.value("handling", json::object()), m_handling);
	ReadCamera(g.value("camera", json::object()), m_rig);
	LoadLayout(layoutPath);

	const glm::vec3 p = player->GetPosition();
	m_spawnPos = glm::vec2(p.x, p.z);
	const glm::vec3 fwd = player->GetOrientation() * glm::vec3(0, 0, 1);
	m_spawnHeading = std::atan2(fwd.x, fwd.z);
	if (g.contains("spawn") && g["spawn"].is_object()) {
		const json& s = g["spawn"];
		if (s.contains("position") && s["position"].is_array() && s["position"].size() >= 2) {
			m_spawnPos = glm::vec2(s["position"][0].get<float>(), s["position"][1].get<float>());
		}
		m_spawnHeading = glm::radians(s.value("heading_deg", glm::degrees(m_spawnHeading)));
	}
	ResetToSpawn();
	ApplyTransforms();
	m_available = true;
	std::cout << "[DriveMode] Ready. Areas: " << m_areas.size() << " obstacles: " << m_obstacles.size()
		<< (m_roads ? " (road network)" : "") << std::endl;
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
	return std::clamp(std::abs(m_speed) / (m_handling.topSpeedKmh / 3.6f), 0.0f, 1.0f);
}

void DriveMode::Teleport(const glm::vec2& pos, float heading)
{
	m_pos = m_prevPos = pos;
	m_heading = heading;
	m_speed = m_lateral = m_steer = m_yawRate = 0.0f;
	m_longAccel = m_latAccel = m_roll = m_pitch = 0.0f;
	m_gear = 1;
	m_rpm = m_handling.rpmIdle;
	m_shiftTimer = 0.0f;
	m_camInitialized = false;
	m_teleported = true;
	ApplyTransforms();
}

void DriveMode::ResetToSpawn()
{
	Teleport(m_spawnPos, m_spawnHeading);
	m_nitro = 1.0f;
	m_nitroCooldown = 0.0f;
}

void DriveMode::RecoverToRoad()
{
	if (!m_roads) {
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
	// Right-hand lane: facing dir, the car's right is (dir.y, -dir.x).
	const glm::vec2 right(dir.y, -dir.x);
	Teleport(closest + right * (e.halfWidth * 0.5f), std::atan2(dir.x, dir.y));
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

glm::vec2 DriveMode::ResolveCircle(glm::vec2& c, float radius) const
{
	glm::vec2 total(0.0f);
	// Street band: building lines and block interiors are walls.
	if (m_roads) {
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
			// Snap to the closest point of the closest area.
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
			glm::vec2 delta = local - q;
			float dist = glm::length(delta);
			if (dist < radius) {
				glm::vec2 nLocal;
				if (dist > 1e-4f) nLocal = delta / dist;
				else {
					// Centre inside the box: push out along the shallowest axis.
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
			// Remove the velocity into the surface (slight bounce), scrape along it, and turn the car
			// toward the wall direction so it slides instead of sticking.
			vel -= n * into * 1.15f;
			vel *= 0.985f;
			m_impact = std::max(m_impact, -into);
			const glm::vec2 tangent(-n.y, n.x);
			const float side = glm::dot(fwd, tangent) >= 0.0f ? 1.0f : -1.0f;
			const float wallHeading = std::atan2(tangent.x * side, tangent.y * side);
			m_heading += WrapAngle(wallHeading - m_heading) * std::min(1.0f, std::abs(into) * 0.02f + 0.08f);
			m_yawRate *= 0.6f;
		}
	}
}

void DriveMode::Simulate(float dt, const Input& in)
{
	const Handling& h = m_handling;
	const float topSpeed0 = h.topSpeedKmh / 3.6f;
	const float speed01 = std::clamp(std::abs(m_speed) / topSpeed0, 0.0f, 1.0f);
	m_steer = Approach(m_steer, in.steer, dt * (std::abs(in.steer) > std::abs(m_steer) ? h.steerRate : h.steerRate * 1.4f));
	m_throttle = in.throttle;

	// Nitrous: depletes while held with throttle, recharges after a delay.
	m_nitroActive = in.nitrous && m_nitro > 0.0f && in.throttle > 0.05f && m_speed > 1.0f;
	if (m_nitroActive) {
		m_nitro = std::max(0.0f, m_nitro - h.nitroDrain * dt);
		m_nitroCooldown = h.nitroRechargeDelay;
	}
	else {
		m_nitroCooldown = std::max(0.0f, m_nitroCooldown - dt);
		if (m_nitroCooldown <= 0.0f) m_nitro = std::min(1.0f, m_nitro + h.nitroRecharge * dt);
	}

	// Longitudinal.
	const float topSpeed = topSpeed0 * (m_nitroActive ? h.nitroTopSpeed : 1.0f);
	const bool shifting = m_shiftTimer > 0.0f;
	m_shiftTimer = std::max(0.0f, m_shiftTimer - dt);
	float accel = 0.0f;
	if (in.throttle > 0.0f && m_speed >= -0.5f) {
		const float s = std::clamp(m_speed / topSpeed, 0.0f, 1.0f);
		float drive = in.throttle * h.acceleration * (1.0f - s * s) * (m_nitroActive ? h.nitroThrust : 1.0f);
		if (shifting) drive *= 0.15f;
		if (m_limiter) drive *= 0.3f;
		accel += drive;
	}
	if (in.brake > 0.0f) {
		if (m_speed > 0.5f) accel -= in.brake * h.braking;
		else if (m_speed > -h.reverseMaxKmh / 3.6f) accel -= in.brake * 6.0f; // reverse
	}
	if (in.handbrake) accel -= std::copysign(std::min(std::abs(m_speed), 1.0f) * 7.0f, m_speed);
	accel -= m_speed * (0.0085f * std::abs(m_speed) + 0.06f); // aero + rolling drag
	const float prevSpeed = m_speed;
	m_speed += accel * dt;
	if (in.throttle == 0.0f && in.brake == 0.0f && std::abs(m_speed) < 0.25f) m_speed = 0.0f;
	if (prevSpeed > 0.0f && m_speed < 0.0f && in.brake > 0.0f && prevSpeed > 0.6f) m_speed = 0.0f; // stop before reversing

	// Lateral: bicycle-model yaw from steering with reduced lock at speed; the handbrake breaks rear
	// grip so the car rotates and slides.
	const float maxLock = glm::radians(glm::mix(h.steerLockLow, h.steerLockHigh, std::sqrt(speed01)));
	const float wheelbase = 2.6f;
	// Heading increases toward +X, which is the car's left (right = -X when facing +Z).
	float targetYaw = -m_speed / wheelbase * std::tan(m_steer * maxLock);
	if (in.handbrake && std::abs(m_speed) > 8.0f) targetYaw *= h.driftYawGain;
	m_yawRate = Approach(m_yawRate, targetYaw, dt * (in.handbrake ? 5.0f : 8.0f));
	m_heading = WrapAngle(m_heading + m_yawRate * dt);
	const float grip = in.handbrake ? h.driftGrip : h.grip;
	m_lateral += (-m_yawRate * m_speed * (in.handbrake ? 0.35f : 0.12f)) * dt;
	m_lateral *= std::exp(-grip * dt);

	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	const glm::vec2 right(fwd.y, -fwd.x);
	glm::vec2 velocity = fwd * m_speed + right * m_lateral;
	glm::vec2 next = m_pos + velocity * dt;
	Collide(next, velocity);
	m_speed = glm::dot(velocity, fwd);
	m_lateral = glm::dot(velocity, right);
	m_pos = next;

	m_longAccel = Approach(m_longAccel, (m_speed - prevSpeed) / std::max(dt, 1e-4f), dt * 6.0f);
	m_latAccel = Approach(m_latAccel, m_yawRate * m_speed, dt * 6.0f);
	m_slip = std::clamp(std::abs(m_lateral) / 6.0f + (in.handbrake && std::abs(m_speed) > 5.0f ? 0.5f : 0.0f)
		+ (in.throttle > 0.9f && m_gear == 1 && std::abs(m_speed) < 6.0f ? 0.35f : 0.0f), 0.0f, 1.0f);

	// Gearbox: 6 speeds, upshift at the redline, downshift when the lower gear has room.
	const float s = std::abs(m_speed) / topSpeed0;
	auto rpmFor = [&](int gear) {
		const float t = s / kGearTop[std::clamp(gear, 1, 6) - 1];
		return h.rpmIdle + t * (h.rpmLimiter - h.rpmIdle);
	};
	if (m_speed < -0.5f) {
		m_gear = -1;
	}
	else {
		if (m_gear < 1) m_gear = 1;
		if (!shifting) {
			if (m_gear < 6 && rpmFor(m_gear) > h.rpmRedline + 150.0f && in.throttle > 0.1f) {
				++m_gear;
				m_shiftTimer = h.shiftTime;
				m_shifted = true;
			}
			else if (m_gear > 1 && rpmFor(m_gear - 1) < h.rpmRedline * 0.82f) {
				--m_gear;
				m_shiftTimer = h.shiftTime * 0.6f;
				m_shifted = true;
			}
		}
	}
	float rpmTarget;
	if (m_gear == -1) rpmTarget = h.rpmIdle + std::abs(m_speed) / (h.reverseMaxKmh / 3.6f) * 4500.0f;
	else rpmTarget = rpmFor(m_gear);
	// Clutch slip at launch / free revving when stationary.
	rpmTarget = std::max(rpmTarget, h.rpmIdle + in.throttle * 3200.0f * std::clamp(1.0f - s / (kGearTop[0] * 0.6f), 0.0f, 1.0f));
	if (shifting) rpmTarget *= 0.82f;
	m_limiter = rpmTarget >= h.rpmLimiter - 1.0f && in.throttle > 0.1f;
	if (m_limiter) rpmTarget = h.rpmLimiter - (std::fmod(m_time * 22.0f, 1.0f) < 0.5f ? 0.0f : 380.0f);
	m_rpm = Approach(m_rpm, std::min(rpmTarget, h.rpmLimiter), dt * (shifting ? 14.0f : 9.0f));
	// Engine load (shown as BOOST): throttle x rpm with spool lag; no turbo is simulated.
	const float rpm01 = std::clamp((m_rpm - h.rpmIdle) / (h.rpmLimiter - h.rpmIdle), 0.0f, 1.0f);
	m_load = Approach(m_load, in.throttle * (0.25f + 0.75f * rpm01) * (shifting ? 0.3f : 1.0f), dt * (in.throttle > m_load ? 3.0f : 6.0f));

	m_roll = Approach(m_roll, std::clamp(-m_latAccel * 0.0045f, -0.06f, 0.06f), dt * 5.0f);
	m_pitch = Approach(m_pitch, std::clamp(-m_longAccel * 0.0035f, -0.04f, 0.04f), dt * 5.0f);
}

void DriveMode::ApplyTransforms()
{
	if (auto player = m_player.lock()) {
		const glm::vec3 pos(m_pos.x, m_playerY, m_pos.y);
		player->SetLocalTRS(pos, glm::angleAxis(m_heading, glm::vec3(0, 1, 0)), player->GetScale());
	}
	if (auto body = m_body.lock()) {
		// Body roll/pitch about the car's local axes (suspension feel).
		const glm::quat q = glm::angleAxis(m_roll, glm::vec3(0, 0, 1)) * glm::angleAxis(m_pitch, glm::vec3(1, 0, 0));
		body->SetLocalTRS(m_bodyBasePos, q * m_bodyBaseRot, m_bodyScale);
	}
}

void DriveMode::UpdateCamera(float dt, Camera& camera, float aspect)
{
	const CameraRig& rig = m_rig;
	const float speed01 = Speed01();
	const glm::vec3 carPos(m_pos.x, m_playerY, m_pos.y);
	if (!m_camInitialized) m_camHeading = m_heading;
	// Heading lag: the camera trails the car's yaw, so drifts show the car's flank. Reversing keeps
	// the camera behind the car's nose direction (no flip).
	m_camHeading = WrapAngle(m_camHeading + WrapAngle(m_heading - m_camHeading) * (1.0f - std::exp(-dt * (m_cameraMode == 2 ? 30.0f : rig.headingLag))));
	const glm::vec3 camFwd(std::sin(m_camHeading), 0.0f, std::cos(m_camHeading));

	float distance = rig.distance, height = rig.height, lookAhead = rig.lookAhead, lookHeight = rig.lookHeight;
	if (m_cameraMode == 1) { distance *= 1.6f; height *= 1.6f; lookAhead *= 1.2f; lookHeight *= 1.3f; }
	distance += speed01 * rig.pullBack + std::clamp(m_longAccel * 0.03f, -0.15f, 0.45f);
	glm::vec3 desired = carPos - camFwd * distance + glm::vec3(0.0f, height, 0.0f);
	glm::vec3 target = carPos + camFwd * lookAhead + glm::vec3(0.0f, lookHeight, 0.0f);
	const glm::vec3 fwd(std::sin(m_heading), 0.0f, std::cos(m_heading));
	if (m_cameraMode == 2) {
		desired = carPos + fwd * 1.9f + glm::vec3(0.0f, 0.75f, 0.0f);
		target = desired + fwd * 10.0f;
	}

	if (!m_camInitialized || m_cameraMode == 2) {
		m_camPos = desired;
		m_camVel = glm::vec3(0.0f);
		m_camInitialized = true;
	}
	else {
		// Critically damped spring.
		const float omega = rig.stiffness;
		const glm::vec3 x = m_camPos - desired;
		const float e = std::exp(-omega * dt);
		const glm::vec3 temp = (m_camVel + omega * x) * dt;
		m_camVel = (m_camVel - omega * temp) * e;
		m_camPos = desired + (x + temp) * e;
	}
	// Keep the camera out of walls and obstacles: pull it toward the car until it is clear.
	if (m_cameraMode != 2) {
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
	if (speed01 > 0.55f) {
		const float a = (speed01 - 0.55f) * 0.025f;
		eye += glm::vec3(std::sin(m_time * 37.0f), std::sin(m_time * 53.0f + 1.3f), 0.0f) * a;
	}
	const float targetFov = rig.fov + speed01 * rig.fovSpeedGain + (m_nitroActive ? rig.fovNitro : 0.0f);
	m_camFov = Approach(m_camFov, targetFov, dt * 3.0f);

	const glm::vec3 f = glm::normalize(target - eye);
	const float pitch = glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f)));
	const float yaw = glm::degrees(std::atan2(f.z, f.x));
	camera.SetPose(eye, glm::vec3(0, 1, 0), yaw, pitch);
	camera.SetPerspective(m_camFov, aspect, camera.GetCameraNearPlane(), camera.GetCameraFarPlane());
}

void DriveMode::Update(float dt, const Input& in, bool frozen, Camera* camera, float aspect)
{
	if (!m_active) return;
	dt = std::clamp(dt, 0.0f, 0.05f);
	m_time += dt;
	m_impact = std::max(0.0f, m_impact - dt * 40.0f);
	m_prevPos = m_pos;
	if (frozen) {
		// Engine still revs (countdown launch control), but the car does not move.
		Input held = in;
		m_throttle = in.throttle;
		const float target = m_handling.rpmIdle + held.throttle * 4200.0f;
		m_rpm = Approach(m_rpm, target, dt * 8.0f);
		m_limiter = false;
		m_load = Approach(m_load, held.throttle * 0.6f, dt * 4.0f);
		m_speed = m_lateral = m_yawRate = 0.0f;
		m_nitroActive = false;
	}
	else {
		// Sub-step at speed so thin obstacles and walls cannot be skipped.
		const float travel = std::abs(m_speed) * dt;
		const int steps = std::clamp(static_cast<int>(std::ceil(travel / 0.6f)), 1, 8);
		for (int i = 0; i < steps; ++i) Simulate(dt / steps, in);
	}
	ApplyTransforms();
	if (camera && m_cameraMode != 3) UpdateCamera(dt, *camera, aspect);
}

#include "VehicleService.h"
#include "VehicleMath.h"

#include "CollisionWorld.h"
#include "RoadNetwork.h"
#include "SceneNode.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace VehicleMath;

namespace {
	std::string ValidateDefinition(const VehicleDefinition& d, IVehiclePolicy* policy) {
		if (!policy) return "no vehicle policy bound";
		if (d.backend != "two_axle_planar") return "unsupported backend '" + d.backend + "' (supported: two_axle_planar)";
		if (d.drive != "rear") return "unsupported drive '" + d.drive + "' (supported: rear)";
		auto positive = [](float v, float maxV) { return std::isfinite(v) && v > 0.0f && v <= maxV; };
		if (!positive(d.frontAxleDistance, 20.0f)) return "frontAxleDistance must be in (0, 20] m";
		if (!positive(d.rearAxleDistance, 20.0f)) return "rearAxleDistance must be in (0, 20] m";
		if (!positive(d.track, 10.0f)) return "track must be in (0, 10] m";
		for (int i = 0; i < 4; ++i) {
			if (!positive(d.wheelRadius[i], 2.0f)) return "wheelRadius[" + std::to_string(i) + "] must be in (0, 2] m";
			const glm::vec3& r = d.wheelRest[i];
			if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)) return "wheelRest[" + std::to_string(i) + "] is not finite";
		}
		if (!positive(d.collisionRadius, 5.0f)) return "collisionRadius must be in (0, 5] m";
		if (!std::isfinite(d.collisionOffset) || d.collisionOffset < 0.0f || d.collisionOffset > 5.0f) return "collisionOffset must be in [0, 5] m";
		const VehicleTuning& t = d.tuning;
		if (t.gearRatios.empty() || t.gearRatios.size() > 8) return "gearRatios must hold 1..8 ratios";
		for (size_t i = 0; i < t.gearRatios.size(); ++i) {
			if (!std::isfinite(t.gearRatios[i]) || t.gearRatios[i] <= 0.05f) return "gear ratios must be positive";
			if (i > 0 && t.gearRatios[i] > t.gearRatios[i - 1]) return "gear ratios must fall from first to top gear";
		}
		if (!positive(t.mass, 1e6f)) return "mass must be positive";
		if (!positive(t.topSpeedKmh, 1000.0f)) return "topSpeedKmh must be positive";
		return std::string();
	}
}

// ---------------------------------------------------------------------------------------------
// One backend instance. Member names follow the original DriveMode fields so the numeric code can be
// compared line by line with the legacy implementation (parity baseline in build/baseline/vehicle).
// ---------------------------------------------------------------------------------------------
class VehicleInstance {
public:
	VehicleInstance(const VehicleDefinition& def, IVehiclePolicy* policy);

	void Advance(float dt, const VehicleInput& in, bool frozen);
	void Teleport(const glm::vec3& pos, float heading);
	void Teleport(const glm::vec2& pos, float heading);
	void ResetToSpawn();
	void RecoverToRoad();
	void SetSpawn(const glm::vec2& p, float heading) { m_spawnPos = p; m_spawnHeading = heading; }
	void SetPlanarPosition(const glm::vec2& p) { m_pos = m_prevPos = p; }
	void SetTuning(const VehicleTuning& t) { m_tuning = t; }
	void SetPolicy(IVehiclePolicy* policy) { m_policy = policy; }
	void SetObserver(VehicleService::SubstepObserver o) { m_observer = std::move(o); }
	VehicleState Snapshot() const;
	float GroundHeight(const glm::vec2& p) const;
	bool SurfaceHeight(const glm::vec2& p, float fromY, float maxDrop, float& outY, glm::vec3* outNormal) const;
	glm::vec2 ResolveCircle(glm::vec2& c, float radius) const;
	bool TakeTeleported() { const bool t = m_teleported; m_teleported = false; return t; }
	bool TakeTeleportFx() { const bool t = m_teleportFx; m_teleportFx = false; return t; }

private:
	struct RigNode {
		std::weak_ptr<SceneNode> node;
		glm::vec3 restPos{ 0.0f };
		glm::quat restRot{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 restScale{ 1.0f };
	};
	static RigNode Capture(const std::weak_ptr<SceneNode>& w);

	void Step(float h, const VehicleInput& in);
	void EngineRpmGearbox(float h, const VehicleInput& in, float wheelSpeed, bool shifting);
	void Hold(float dt, float throttle);
	void UpdateSuspension(float h);
	void SampleWheels();
	void UpdateVertical(float h);
	void Collide(glm::vec2& pos, glm::vec2& vel);
	float GearRatio(int gear) const;
	int GearCount() const { return static_cast<int>(m_tuning.gearRatios.size()); }
	float BaseY() const { return m_collision ? m_y : 0.0f; }
	float PlayerYOffset() const { return m_collision ? 0.0f : m_playerY; }
	void ApplyTransforms();
	VehicleStepTelemetry Telemetry(float h, float speed, float speed01, float beta, float vxAbs) const;

	IVehiclePolicy* m_policy = nullptr;
	VehicleTuning m_tuning;
	VehicleService::SubstepObserver m_observer;

	// Rig slots (captured at creation).
	RigNode m_player;
	RigNode m_model;
	RigNode m_bodyMesh;
	std::array<RigNode, 4> m_wheelNodes;
	bool m_rigWheels = false;
	float m_wheelRadius[4] = { 0.33f, 0.33f, 0.345f, 0.345f };
	glm::vec3 m_wheelRest[4] = { glm::vec3(0.8f, 0.33f, 1.36f), glm::vec3(-0.8f, 0.33f, 1.36f),
		glm::vec3(0.8f, 0.33f, -1.36f), glm::vec3(-0.8f, 0.33f, -1.36f) };
	float m_playerY = 0.0f;
	float m_carRadius = 0.95f;
	float m_carOffset = 1.25f;
	std::vector<VehicleArea> m_areas;
	std::vector<VehicleObstacle> m_obstacles;
	const RoadNetwork* m_roads = nullptr;
	const CollisionWorld* m_collision = nullptr;
	float m_a = 1.36f;                   // CG to front axle
	float m_b = 1.36f;                   // CG to rear axle
	float m_track = 1.6f;
	float m_spawnY = 1e4f;
	glm::vec2 m_spawnPos{ 0.0f };
	float m_spawnHeading = 0.0f;

	// Surface mode vertical state.
	float m_y = 0.0f;
	float m_vyUp = 0.0f;
	bool m_airborne = false;
	float m_airTime = 0.0f;
	bool m_recoverRequested = false;
	bool m_hasSafePose = false;
	glm::vec3 m_safePos{ 0.0f };
	float m_safeHeading = 0.0f;
	float m_safeTimer = 0.0f;
	float m_groundPitch = 0.0f;          // rad, nose up +
	float m_groundRoll = 0.0f;           // rad, right side down +
	bool m_wheelHit[4] = { true, true, true, true };
	float m_wheelRel[4] = {};

	// Vehicle state (metres, seconds, radians). Heading 0 = +Z, increases toward +X (the car's left).
	glm::vec2 m_pos{ 0.0f };
	glm::vec2 m_prevPos{ 0.0f };
	float m_heading = 0.0f;
	glm::vec2 m_vel{ 0.0f };
	float m_vx = 0.0f, m_vy = 0.0f;
	float m_yawRate = 0.0f;
	float m_steerAngle = 0.0f;
	float m_ax = 0.0f, m_ay = 0.0f;
	float m_rearSpin = 0.0f;
	float m_accum = 0.0f;
	int m_gear = 1;
	float m_rpm = 900.0f;
	bool m_limiter = false;
	float m_load = 0.0f;
	float m_throttle = 0.0f;
	float m_slip = 0.0f;
	float m_impact = 0.0f;
	std::array<VehicleWheelState, 4> m_wheels;
	float m_time = 0.0f;
	uint32_t m_teleportSerial = 0;
	bool m_teleported = false;
	bool m_teleportFx = false;
	// Visual wheel state (render-rate smoothing).
	float m_visHub[4] = { 0.33f, 0.33f, 0.345f, 0.345f };
	float m_visSteer[4] = {};
	float m_visSpin[4] = {};
	float m_visSpinPrev[4] = {};
	float m_visDt = 1.0f / 60.0f;
	bool m_visInit = false;
	// Sprung body.
	float m_heave = 0.0f, m_heaveV = 0.0f;
	float m_pitch = 0.0f, m_pitchV = 0.0f;
	float m_roll = 0.0f, m_rollV = 0.0f;
};

VehicleInstance::RigNode VehicleInstance::Capture(const std::weak_ptr<SceneNode>& w)
{
	RigNode r;
	r.node = w;
	if (auto n = w.lock()) {
		r.restPos = n->GetPosition();
		r.restRot = n->GetOrientation();
		r.restScale = n->GetScale();
	}
	return r;
}

VehicleInstance::VehicleInstance(const VehicleDefinition& def, IVehiclePolicy* policy)
	: m_policy(policy), m_tuning(def.tuning), m_rigWheels(def.wheelRig), m_playerY(def.rootHeight),
	m_carRadius(def.collisionRadius), m_carOffset(def.collisionOffset), m_areas(def.areas), m_obstacles(def.obstacles),
	m_roads(def.roads), m_collision(def.collision), m_a(def.frontAxleDistance), m_b(def.rearAxleDistance), m_track(def.track),
	m_spawnY(def.spawnY), m_spawnPos(def.spawnPos), m_spawnHeading(def.spawnHeading)
{
	m_player = Capture(def.rig.root);
	m_model = Capture(def.rig.model);
	m_bodyMesh = Capture(def.rig.body);
	for (int i = 0; i < 4; ++i) {
		m_wheelNodes[i] = Capture(def.rig.wheels[i]);
		m_wheelRadius[i] = def.wheelRadius[i];
		m_wheelRest[i] = def.wheelRest[i];
	}
}

// ---------------------------------------------------------------------------------------------
// Road surface and collision
// ---------------------------------------------------------------------------------------------
float VehicleInstance::GroundHeight(const glm::vec2& p) const
{
	if (m_roads) {
		if (!m_roads->IsOnRoad(p, 0.0f)) {
			return 0.15f; // curb + sidewalk (the building band beyond is a wall)
		}
	}
	// Low-frequency road roughness (patched asphalt, gentle dips) plus faint high-frequency noise.
	const float a = m_tuning.bumpAmplitude;
	const float low = std::sin(p.x * 0.37f + 1.3f) * std::sin(p.y * 0.29f + 0.4f);
	const float mid = 0.5f * std::sin(p.x * 1.9f + p.y * 0.7f) * std::sin(p.y * 1.3f - p.x * 0.4f);
	return a * (low + mid) + a;
}

bool VehicleInstance::SurfaceHeight(const glm::vec2& p, float fromY, float maxDrop, float& outY, glm::vec3* outNormal) const
{
	if (!m_collision) return false;
	CollisionWorld::Hit hit;
	if (!m_collision->GroundBelow(glm::vec3(p.x, fromY, p.y), maxDrop, hit)) return false;
	outY = hit.position.y;
	if (outNormal) *outNormal = hit.normal;
	return true;
}

void VehicleInstance::SampleWheels()
{
	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	const glm::vec2 left(std::cos(m_heading), -std::sin(m_heading));
	for (int i = 0; i < 4; ++i) {
		const glm::vec2 wp = m_pos + left * m_wheelRest[i].x + fwd * m_wheelRest[i].z;
		VehicleWheelState& w = m_wheels[i];
		if (!m_collision) {
			w.contact = glm::vec3(wp.x, GroundHeight(wp), wp.y);
			m_wheelHit[i] = true;
			continue;
		}
		// From just above the current deck (decks above are >= ~5 m higher), down past a crest drop.
		float y;
		m_wheelHit[i] = SurfaceHeight(wp, m_y + 1.5f, 1.5f + (m_airborne ? 60.0f : 4.0f), y, nullptr);
		if (!m_wheelHit[i]) y = m_y;
		// Fine roughness on top of the authored surface (same as the planar mode bumps).
		const float a = m_tuning.bumpAmplitude;
		const float bump = a * (std::sin(wp.x * 0.37f + 1.3f) * std::sin(wp.y * 0.29f + 0.4f) +
			0.5f * std::sin(wp.x * 1.9f + wp.y * 0.7f) * std::sin(wp.y * 1.3f - wp.x * 0.4f));
		w.contact = glm::vec3(wp.x, y + bump, wp.y);
	}
	if (!m_collision) {
		for (int i = 0; i < 4; ++i) m_wheelRel[i] = m_wheels[i].contact.y;
	}
}

// Surface mode: fit the ground plane under the wheels, follow it while grounded (with a little
// extra stickiness over crests), otherwise fly ballistically and land.
void VehicleInstance::UpdateVertical(float h)
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
			std::cout << "[Vehicle] No ground for 3 s at (" << m_pos.x << ", " << m_y << ", " << m_pos.y << "): recovering" << std::endl;
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
	for (int i = 0; i < 4; ++i) {
		const float plane = m_y + std::tan(m_groundPitch) * m_wheelRest[i].z + std::tan(m_groundRoll) * m_wheelRest[i].x;
		m_wheelRel[i] = m_wheelHit[i] && !m_airborne ? std::clamp(m_wheels[i].contact.y - plane, -0.15f, 0.15f) : -0.08f;
		m_wheels[i].contact.y = m_wheelHit[i] ? m_wheels[i].contact.y : m_y;
	}
}

glm::vec2 VehicleInstance::ResolveCircle(glm::vec2& c, float radius) const
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
		for (const VehicleArea& a : m_areas) {
			if (c.x >= a.min.x && c.x <= a.max.x && c.y >= a.min.y && c.y <= a.max.y) { inside = true; break; }
		}
		if (!inside) {
			float best = 1e30f;
			glm::vec2 bestP = c;
			for (const VehicleArea& a : m_areas) {
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
		// Contacts only up to the roof line (~1.6 m): overhead structure must not catch the sphere.
		glm::vec3 centre(c.x, m_y + radius + 0.25f, c.y);
		const glm::vec3 push3 = m_collision->ResolveSphere(centre, radius, CollisionWorld::Wall, nullptr, 2, m_y + m_tuning.roofHeight);
		const glm::vec2 push(push3.x, push3.z);
		c += push;
		total += push;
	}
	for (const VehicleObstacle& o : m_obstacles) {
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

void VehicleInstance::Collide(glm::vec2& pos, glm::vec2& vel)
{
	const glm::vec2 fwd(std::sin(m_heading), std::cos(m_heading));
	for (int iter = 0; iter < 3; ++iter) {
		glm::vec2 front = pos + fwd * m_carOffset;
		glm::vec2 rear = pos - fwd * m_carOffset;
		const glm::vec2 pf = ResolveCircle(front, m_carRadius);
		const glm::vec2 pr = ResolveCircle(rear, m_carRadius);
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
// Placement
// ---------------------------------------------------------------------------------------------
void VehicleInstance::Teleport(const glm::vec3& pos, float heading)
{
	if (m_collision) {
		float y = pos.y;
		const glm::vec2 p(pos.x, pos.z);
		if (!SurfaceHeight(p, pos.y + 2.5f, 40.0f, y, nullptr) && !SurfaceHeight(p, 1e4f, 2e4f, y, nullptr)) y = pos.y;
		m_y = y;
		m_vyUp = 0.0f;
		m_airborne = false;
		m_airTime = 0.0f;
	}
	Teleport(glm::vec2(pos.x, pos.z), heading);
}

void VehicleInstance::Teleport(const glm::vec2& pos, float heading)
{
	if (m_collision) {
		// Keep the current deck when one exists under the new position.
		float y;
		if (SurfaceHeight(pos, m_y + 2.5f, 40.0f, y, nullptr) || SurfaceHeight(pos, 1e4f, 2e4f, y, nullptr)) m_y = y;
		m_vyUp = 0.0f;
		m_airborne = false;
	}
	m_pos = m_prevPos = pos;
	m_heading = heading;
	m_vel = glm::vec2(0.0f);
	m_vx = m_vy = m_yawRate = 0.0f;
	m_steerAngle = 0.0f;
	m_ax = m_ay = 0.0f;
	m_rearSpin = 0.0f;
	m_heave = m_heaveV = m_pitch = m_pitchV = m_roll = m_rollV = 0.0f;
	m_gear = 1;
	m_rpm = m_tuning.rpmIdle;
	m_teleported = true;
	m_teleportFx = true;
	++m_teleportSerial;
	if (m_policy) m_policy->OnTeleport();
	SampleWheels();
	if (m_collision) UpdateVertical(0.0f);
	ApplyTransforms();
}

void VehicleInstance::ResetToSpawn()
{
	if (m_collision) Teleport(glm::vec3(m_spawnPos.x, m_spawnY, m_spawnPos.y), m_spawnHeading);
	else Teleport(m_spawnPos, m_spawnHeading);
	if (m_policy) m_policy->OnRespawn();
}

void VehicleInstance::RecoverToRoad()
{
	if (!m_roads) {
		ResetToSpawn();
		return;
	}
	if (m_collision) {
		// Stay on the current level: nearest carriageway within a deck height whose lane has
		// collision ground under it. After a fall the current height is meaningless, so fall back to
		// the last safe grounded pose.
		int edge = -1;
		float t = 0.0f;
		if (!m_airborne) m_roads->ClosestOnRoads3D(glm::vec3(m_pos.x, BaseY(), m_pos.y), 4.0f, nullptr, &edge, &t);
		if (edge >= 0) {
			float heading = m_heading;
			const glm::vec3 lane = m_roads->LanePosition(edge, t, m_heading, -1, heading);
			float y;
			if (SurfaceHeight(glm::vec2(lane.x, lane.z), lane.y + 2.5f, 6.0f, y, nullptr)) {
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

// ---------------------------------------------------------------------------------------------
// Native mechanics
// ---------------------------------------------------------------------------------------------
float VehicleInstance::GearRatio(int gear) const
{
	const std::vector<float>& r = m_tuning.gearRatios;
	return r[static_cast<size_t>(std::clamp(gear, 1, static_cast<int>(r.size())) - 1)];
}

VehicleStepTelemetry VehicleInstance::Telemetry(float h, float speed, float speed01, float beta, float vxAbs) const
{
	const float topSpeed = m_tuning.topSpeedKmh / 3.6f;
	VehicleStepTelemetry t;
	t.time = m_time;
	t.h = h;
	t.pos = m_pos;
	t.vel = m_vel;
	t.heading = m_heading;
	t.vx = m_vx;
	t.vy = m_vy;
	t.yawRate = m_yawRate;
	t.ax = m_ax;
	t.ay = m_ay;
	t.steerAngle = m_steerAngle;
	t.speed = speed;
	t.speed01 = speed01;
	t.beta = beta;
	t.vxAbs = vxAbs;
	t.wheelbase = m_a + m_b;
	t.track = m_track;
	t.topSpeed = topSpeed;
	t.rearSpin = m_rearSpin;
	t.rpm = m_rpm;
	t.gear = m_gear;
	t.limiter = m_limiter;
	t.airborne = m_airborne;
	t.surface = m_collision != nullptr;
	t.groundPitch = m_groundPitch;
	t.groundRoll = m_groundRoll;
	t.gearCount = GearCount();
	t.rpmIdle = m_tuning.rpmIdle;
	// Final drive: the top gear reaches the limiter slightly above top speed.
	t.finalDrive = m_tuning.rpmLimiter * 2.0f * kPi / 60.0f * m_wheelRadius[2] / (topSpeed * 1.04f * GearRatio(GearCount()));
	t.rearWheelRadius = m_wheelRadius[2];
	return t;
}

void VehicleInstance::Step(float h, const VehicleInput& in)
{
	const VehicleTuning& hd = m_tuning;
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

	// --- Control (policy): nitrous, drift, steering, direction and gear selection.
	VehicleStepTelemetry t = Telemetry(h, speed, speed01, beta, vxAbs);
	VehicleActuation act;
	act.gear = m_gear;
	m_policy->Control(in, t, act);
	m_gear = act.gear;
	m_steerAngle = act.steerAngle;
	const float delta = act.steerAngle; // left positive
	const bool shifting = act.driveShiftCut;

	// --- Loads with longitudinal and lateral transfer.
	const float Fz = m * kG;
	float FzF = Fz * m_b / L - m * m_ax * hd.cgHeight / L;
	float FzR = Fz * m_a / L + m * m_ax * hd.cgHeight / L;
	FzF = std::max(FzF, Fz * 0.15f);
	FzR = std::max(FzR, Fz * 0.15f);
	const float latTransfer = m * m_ay * hd.cgHeight / std::max(m_track, 0.5f);

	// --- Longitudinal forces.
	const float ratio1 = GearRatio(1);
	const float finalDriveForce = hd.driveAcceleration * m; // first-gear drive capacity
	const float curve = TorqueCurve(m_rpm, hd.rpmIdle, hd.rpmLimiter);
	float driveForce = 0.0f;
	if (m_gear >= 1 && act.driveThrottle > 0.0f) {
		driveForce = act.driveThrottle * finalDriveForce * curve * GearRatio(m_gear) / ratio1;
		driveForce *= act.nitroDriveScale;
		if (shifting) driveForce *= 0.12f;
		if (m_limiter) driveForce *= 0.25f;
	}
	else if (m_gear == -1 && act.reverseDrive > 0.0f && m_vx > -hd.reverseMaxKmh / 3.6f) {
		driveForce = -act.reverseDrive * finalDriveForce * 0.45f;
	}
	const float tractionR = hd.tireGripLong * FzR;
	float FxR = driveForce;
	float spinTarget = 0.0f;
	if (std::abs(driveForce) > tractionR) {
		spinTarget = std::clamp((std::abs(driveForce) - tractionR) / tractionR * 1.6f, 0.0f, 1.0f);
		FxR = std::copysign(tractionR * 0.9f, driveForce);
	}
	// Brake force (the pedal mapping and the front/rear split come from the policy). Brakes always oppose the motion.
	float brakeF = 0.0f, brakeR = 0.0f;
	if (act.brakeCommand > 0.0f && (std::abs(m_vx) > 0.3f || act.brakeAtStandstill)) {
		const float total = std::min(act.brakeCommand * hd.brakeDecel * m, hd.tireGripLong * (FzF + FzR));
		brakeF = total * act.brakeFrontShare;
		brakeR = total * act.brakeRearShare;
	}
	if (act.rearDriveCut) {
		// Burnout: front brakes hold the car, rear wheels spin up (smoke show).
		spinTarget = std::max(spinTarget, act.rearSpinFloor);
		FxR = 0.0f;
	}
	if (act.rearLock && std::abs(m_vx) > 0.5f) {
		FxR = -std::copysign(hd.tireGripLong * FzR * 0.75f, m_vx); // rear locked, sliding
		spinTarget = 0.0f;
	}
	m_rearSpin = Approach(m_rearSpin, spinTarget, h * (spinTarget > m_rearSpin ? 10.0f : 4.0f));
	const float sgnVx = m_vx >= 0.0f ? 1.0f : -1.0f;
	const float FxF = -sgnVx * brakeF;
	FxR += -sgnVx * brakeR;
	const float drag = (topSpeed > 1.0f ? (finalDriveForce * 0.92f * GearRatio(GearCount()) / ratio1 - 220.0f) / (topSpeed * topSpeed) : 0.4f)
		* act.dragScale;
	const float resist = drag * m_vx * std::abs(m_vx) + (std::abs(m_vx) > 0.1f ? sgnVx * 220.0f : 0.0f);

	// --- Lateral tyre forces (combined slip via friction ellipse).
	const float driftScale = act.rearGripScale;
	const float alphaF = std::atan2(m_vy + m_a * m_yawRate, vxAbs) - delta * sgnVx;
	const float alphaR = std::atan2(m_vy - m_b * m_yawRate, vxAbs);
	auto ellipse = [](float fx, float fxMax) { const float u = fxMax > 1.0f ? fx / fxMax : 0.0f; return std::sqrt(std::max(0.08f, 1.0f - u * u)); };
	const float FyF = -hd.tireGrip * FzF * TyreCurve(alphaF) * ellipse(FxF, hd.tireGripLong * FzF);
	const float rearLong = act.rearLock ? 0.72f : std::abs(FxR) / std::max(tractionR, 1.0f);
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
	// --- Yaw assist (policy): drift hold, slip-rate damping, spin limit, grip stability, yaw damping.
	t = Telemetry(h, speed, speed01, beta, vxAbs);
	t.steerAngle = delta;
	t.ax = ax;
	t.ay = ay;
	VehicleYawAssist ya;
	ya.yawAcc = yawAcc;
	ya.ax = ax;
	m_policy->YawAssist(in, t, ya);
	yawAcc = ya.yawAcc;
	ax = ya.ax;

	m_vx += ax * h;
	m_vy += ay * h;
	m_yawRate += yawAcc * h;
	// Settle at rest: no pedals, or the brake pedal held (the car holds still instead of creeping).
	if (speed < 0.3f && (act.holdAtRest || (act.brakeCommand > 0.1f && std::abs(driveForce) < 1.0f))) { m_vx = 0.0f; m_vy = 0.0f; }

	// --- Velocity correction (policy): arcade drift swing on the integrated velocity.
	t.vx = m_vx;
	t.vy = m_vy;
	VehicleVelocity vc;
	vc.vx = m_vx;
	vc.vy = m_vy;
	m_policy->VelocityCorrection(in, t, vc);
	m_vx = vc.vx;
	m_vy = vc.vy;

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
	const float wheelSpeedR = m_vx + (act.rearLock ? -m_vx : 0.0f) + m_rearSpin * 22.0f * (m_gear == -1 ? -1.0f : 1.0f);
	const float slipF = std::clamp(std::abs(alphaF) / 0.35f, 0.0f, 1.0f);
	const float slipR = std::clamp(std::abs(alphaR) / 0.3f, 0.0f, 1.0f);
	for (int i = 0; i < 4; ++i) {
		VehicleWheelState& w = m_wheels[i];
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
		const float lockSlide = rear && act.rearLock && std::abs(m_vx) > 3.0f ? 0.8f : 0.0f;
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

	// --- Gearbox decision (policy), after the position update: shift timers and automatic shifts.
	const float wheelSpeed = std::abs(m_vx) + m_rearSpin * 22.0f;
	t = Telemetry(h, speed, speed01, beta, vxAbs);
	t.vx = m_vx;
	t.vy = m_vy;
	t.yawRate = m_yawRate;
	t.rearSpin = m_rearSpin;
	t.wheelSpeed = wheelSpeed;
	VehicleGearRequest gr;
	gr.gear = m_gear;
	m_policy->PostStep(in, t, gr);
	m_gear = gr.gear;

	// --- Mechanical gearbox: ratios, rpm and limiter (native).
	EngineRpmGearbox(h, in, wheelSpeed, gr.gearboxShifting);

	// Engine load (shown as BOOST): throttle x rpm with spool lag; no turbo is simulated.
	const float rpm01 = std::clamp((m_rpm - hd.rpmIdle) / (hd.rpmLimiter - hd.rpmIdle), 0.0f, 1.0f);
	m_load = Approach(m_load, in.throttle * (0.25f + 0.75f * rpm01) * (shifting ? 0.3f : 1.0f), h * (in.throttle > m_load ? 3.0f : 6.0f));
}

void VehicleInstance::EngineRpmGearbox(float h, const VehicleInput& in, float wheelSpeed, bool shifting)
{
	const VehicleTuning& hd = m_tuning;
	const float topSpeed = hd.topSpeedKmh / 3.6f;
	const int gears = GearCount();
	const float finalDrive = hd.rpmLimiter * 2.0f * kPi / 60.0f * m_wheelRadius[2] / (topSpeed * 1.04f * GearRatio(gears));
	auto rpmFor = [&](int gear, float v) {
		return std::abs(v) / m_wheelRadius[2] * 60.0f / (2.0f * kPi) * GearRatio(gear) * finalDrive;
	};
	float target = m_gear == -1 ? hd.rpmIdle + std::abs(m_vx) / (hd.reverseMaxKmh / 3.6f) * 4500.0f
		: hd.rpmIdle + rpmFor(m_gear, wheelSpeed);
	// Clutch slip at launch.
	target = std::max(target, hd.rpmIdle + in.throttle * 3000.0f * std::clamp(1.0f - std::abs(m_vx) / 6.0f, 0.0f, 1.0f));
	if (shifting) target *= 0.8f;
	m_limiter = target >= hd.rpmLimiter - 1.0f && in.throttle > 0.1f;
	if (m_limiter) target = hd.rpmLimiter - (std::fmod(m_time * 22.0f, 1.0f) < 0.5f ? 0.0f : 380.0f);
	m_rpm = Approach(m_rpm, std::min(target, hd.rpmLimiter), h * (shifting ? 14.0f : 10.0f));
}

void VehicleInstance::Hold(float dt, float throttle)
{
	m_time += dt;
	m_throttle = throttle;
	m_rpm = Approach(m_rpm, m_tuning.rpmIdle + throttle * 4200.0f, dt * 8.0f);
	m_limiter = false;
	m_load = Approach(m_load, throttle * 0.6f, dt * 4.0f);
	m_vel = glm::vec2(0.0f);
	m_vx = m_vy = m_yawRate = 0.0f;
	m_rearSpin = 0.0f;
	if (m_policy) m_policy->OnHold();
	SampleWheels();
	if (m_collision) UpdateVertical(0.0f);
	for (int i = 0; i < 4; ++i) {
		m_wheels[i].velocity = glm::vec3(0.0f);
		m_wheels[i].slip = m_wheels[i].wheelspin = 0.0f;
	}
	UpdateSuspension(dt);
}

void VehicleInstance::UpdateSuspension(float h)
{
	const VehicleTuning& hd = m_tuning;
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

void VehicleInstance::ApplyTransforms()
{
	if (auto player = m_player.node.lock()) {
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
			const VehicleWheelState& w = m_wheels[i];
			// Hub on the road (clamped to suspension travel relative to the sprung body), low-passed so
			// road noise does not make the wheels jitter.
			const float bodyHub = m_wheelRest[i].y + m_heave + std::sin(m_pitch) * m_wheelRest[i].z + std::sin(m_roll) * m_wheelRest[i].x;
			const float hubTarget = std::clamp(m_wheelRel[i] + m_wheelRadius[i], bodyHub - 0.09f, bodyHub + 0.09f);
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

void VehicleInstance::Advance(float dt, const VehicleInput& in, bool frozen)
{
	dt = std::clamp(dt, 0.0f, VehicleService::kMaxFrameDt);
	m_impact = std::max(0.0f, m_impact - dt * 40.0f);
	m_prevPos = m_pos;
	if (frozen) {
		Hold(dt, in.throttle);
	}
	else {
		m_accum = std::min(m_accum + dt, VehicleService::kMaxAccumulator);
		while (m_accum >= VehicleService::kFixedStep) {
			Step(VehicleService::kFixedStep, in);
			if (m_observer) {
				VehicleSubstepRecord rec;
				rec.h = VehicleService::kFixedStep;
				rec.in = in;
				rec.state = Snapshot();
				if (m_policy) rec.policy = m_policy->Debug();
				m_observer(rec);
			}
			UpdateSuspension(VehicleService::kFixedStep);
			m_accum -= VehicleService::kFixedStep;
		}
		if (m_recoverRequested) {
			m_recoverRequested = false;
			RecoverToRoad();
		}
	}
	m_visDt = dt;
	ApplyTransforms();
}

VehicleState VehicleInstance::Snapshot() const
{
	VehicleState s;
	s.pos = m_pos;
	s.prevPos = m_prevPos;
	s.vel = m_vel;
	s.heading = m_heading;
	s.vx = m_vx;
	s.vy = m_vy;
	s.yawRate = m_yawRate;
	s.ax = m_ax;
	s.ay = m_ay;
	s.steerAngle = m_steerAngle;
	s.time = m_time;
	s.y = m_y;
	s.surface = m_collision != nullptr;
	s.airborne = m_airborne;
	s.groundPitch = m_groundPitch;
	s.groundRoll = m_groundRoll;
	s.heave = m_heave;
	s.playerY = m_playerY;
	s.gear = m_gear;
	s.rpm = m_rpm;
	s.limiter = m_limiter;
	s.rearSpin = m_rearSpin;
	s.slip = m_slip;
	s.load = m_load;
	s.throttle = m_throttle;
	s.impact = m_impact;
	for (int i = 0; i < 4; ++i) s.wheels[i] = m_wheels[i];
	s.wheelRig = m_rigWheels;
	s.spawnPos = m_spawnPos;
	s.spawnHeading = m_spawnHeading;
	s.spawnY = m_spawnY;
	s.teleportSerial = m_teleportSerial;
	return s;
}

// ---------------------------------------------------------------------------------------------
// VehicleService
// ---------------------------------------------------------------------------------------------
VehicleService::VehicleService() = default;
VehicleService::~VehicleService() = default;

VehicleHandle VehicleService::Create(const VehicleDefinition& def, IVehiclePolicy* policy, std::string* error)
{
	const std::string reason = ValidateDefinition(def, policy);
	if (!reason.empty()) {
		if (error) *error = "vehicle '" + def.name + "': " + reason;
		return VehicleHandle();
	}
	size_t slot = m_slots.size();
	for (size_t i = 0; i < m_slots.size(); ++i) {
		if (!m_slots[i].instance) { slot = i; break; }
	}
	if (slot == m_slots.size()) m_slots.emplace_back();
	Slot& s = m_slots[slot];
	s.instance = std::make_unique<VehicleInstance>(def, policy);
	s.instance->ResetToSpawn();
	++m_live;
	VehicleHandle h;
	h.id = static_cast<uint32_t>(slot + 1);
	h.incarnation = s.incarnation;
	if (error) error->clear();
	return h;
}

void VehicleService::Destroy(VehicleHandle h)
{
	if (!Find(h)) return;
	Slot& s = m_slots[h.id - 1];
	s.instance.reset();
	++s.incarnation; // every handle issued for this slot so far is now stale
	--m_live;
}

VehicleInstance* VehicleService::Find(VehicleHandle h)
{
	if (!h.Valid() || h.id > m_slots.size()) return nullptr;
	Slot& s = m_slots[h.id - 1];
	if (!s.instance || s.incarnation != h.incarnation) return nullptr;
	return s.instance.get();
}

const VehicleInstance* VehicleService::Find(VehicleHandle h) const
{
	if (!h.Valid() || h.id > m_slots.size()) return nullptr;
	const Slot& s = m_slots[h.id - 1];
	if (!s.instance || s.incarnation != h.incarnation) return nullptr;
	return s.instance.get();
}

bool VehicleService::IsValid(VehicleHandle h) const { return Find(h) != nullptr; }
size_t VehicleService::Count() const { return m_live; }

bool VehicleService::SetTuning(VehicleHandle h, const VehicleTuning& tuning, std::string* error)
{
	VehicleInstance* inst = Find(h);
	if (!inst) { if (error) *error = "stale or invalid vehicle handle"; return false; }
	if (tuning.gearRatios.empty() || tuning.gearRatios.size() > 8) { if (error) *error = "gearRatios must hold 1..8 ratios"; return false; }
	for (size_t i = 0; i < tuning.gearRatios.size(); ++i) {
		if (!std::isfinite(tuning.gearRatios[i]) || tuning.gearRatios[i] <= 0.05f) { if (error) *error = "gear ratios must be positive"; return false; }
		if (i > 0 && tuning.gearRatios[i] > tuning.gearRatios[i - 1]) { if (error) *error = "gear ratios must fall from first to top gear"; return false; }
	}
	if (!(tuning.mass > 0.0f) || !(tuning.topSpeedKmh > 0.0f)) { if (error) *error = "mass and topSpeedKmh must be positive"; return false; }
	inst->SetTuning(tuning);
	if (error) error->clear();
	return true;
}

void VehicleService::SetSpawn(VehicleHandle h, const glm::vec2& pos, float heading)
{
	if (VehicleInstance* inst = Find(h)) inst->SetSpawn(pos, heading);
}

void VehicleService::SetPlanarPosition(VehicleHandle h, const glm::vec2& pos)
{
	if (VehicleInstance* inst = Find(h)) inst->SetPlanarPosition(pos);
}

void VehicleService::SetSubstepObserver(VehicleHandle h, SubstepObserver observer)
{
	if (VehicleInstance* inst = Find(h)) inst->SetObserver(std::move(observer));
}

void VehicleService::SetPolicy(VehicleHandle h, IVehiclePolicy* policy)
{
	if (VehicleInstance* inst = Find(h)) {
		if (policy) inst->SetPolicy(policy);   // a null policy is refused: the instance keeps its current one
	}
}

bool VehicleService::Advance(VehicleHandle h, float dt, const VehicleInput& in, bool frozen)
{
	VehicleInstance* inst = Find(h);
	if (!inst) return false;
	inst->Advance(dt, in, frozen);
	return true;
}

void VehicleService::Teleport(VehicleHandle h, const glm::vec3& pos, float heading)
{
	if (VehicleInstance* inst = Find(h)) inst->Teleport(pos, heading);
}

void VehicleService::MoveFrom(VehicleService& other)
{
	if (this == &other) return;
	m_slots = std::move(other.m_slots);
	m_live = other.m_live;
	other.m_slots.clear();
	other.m_live = 0;
}

void VehicleService::Teleport(VehicleHandle h, const glm::vec2& pos, float heading)
{
	if (VehicleInstance* inst = Find(h)) inst->Teleport(pos, heading);
}

void VehicleService::ResetToSpawn(VehicleHandle h)
{
	if (VehicleInstance* inst = Find(h)) inst->ResetToSpawn();
}

void VehicleService::RecoverToRoad(VehicleHandle h)
{
	if (VehicleInstance* inst = Find(h)) inst->RecoverToRoad();
}

bool VehicleService::TakeTeleported(VehicleHandle h)
{
	VehicleInstance* inst = Find(h);
	return inst ? inst->TakeTeleported() : false;
}

bool VehicleService::TakeTeleportFx(VehicleHandle h)
{
	VehicleInstance* inst = Find(h);
	return inst ? inst->TakeTeleportFx() : false;
}

VehicleState VehicleService::State(VehicleHandle h) const
{
	const VehicleInstance* inst = Find(h);
	return inst ? inst->Snapshot() : VehicleState();
}

float VehicleService::GroundHeight(VehicleHandle h, const glm::vec2& p) const
{
	const VehicleInstance* inst = Find(h);
	return inst ? inst->GroundHeight(p) : 0.0f;
}

bool VehicleService::SurfaceHeight(VehicleHandle h, const glm::vec2& p, float fromY, float maxDrop, float& outY, glm::vec3* outNormal) const
{
	const VehicleInstance* inst = Find(h);
	return inst ? inst->SurfaceHeight(p, fromY, maxDrop, outY, outNormal) : false;
}

glm::vec2 VehicleService::ResolveCircle(VehicleHandle h, glm::vec2& c, float radius) const
{
	const VehicleInstance* inst = Find(h);
	return inst ? inst->ResolveCircle(c, radius) : glm::vec2(0.0f);
}

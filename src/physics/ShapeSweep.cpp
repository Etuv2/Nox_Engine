#include "ShapeSweep.h"

#include "ShapeOverlap.h"
#include "../PhysicsEngine.h"
#include "../RigidBody.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ShapeSweep {

namespace {

constexpr int kMaxSteps = 4096;      // coarse steps per sweep (the step grows for long sweeps)
constexpr int kBisections = 24;      // refinement of the first overlapping step

bool Finite(const glm::vec3& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The probe as a RigidBody, so the same overlap tests as the character controller apply. Its position is set per step.
void MakeProbeBody(const Probe& p, RigidBody& rb) {
	switch (p.kind) {
	case Kind::SPHERE: rb.setSphere(p.radius); break;
	case Kind::CAPSULE: rb.setCapsule(p.radius, p.halfSegment, 1); break;
	case Kind::BOX: rb.setBox(p.halfExtents); break;
	}
	rb.setOrientation(p.rotation);
}

// Distance from a probe centre to its surface in direction -n (the contact point is that surface point).
float SurfaceExtent(const Probe& p, const glm::vec3& n) {
	if (p.kind != Kind::BOX) return p.radius;
	const glm::mat3 R = glm::mat3_cast(p.rotation);
	const glm::vec3 local = glm::transpose(R) * n;
	return std::abs(local.x) * p.halfExtents.x + std::abs(local.y) * p.halfExtents.y + std::abs(local.z) * p.halfExtents.z;
}

// Overlap test of the probe at a position against the candidate bodies: the deepest overlap wins.
class Tester {
public:
	Tester(RigidBody& probe, const std::vector<std::shared_ptr<RigidBody>>& candidates)
		: m_probe(probe), m_candidates(candidates) {}

	bool At(const glm::vec3& position) {
		m_probe.setPosition(position);
		bool any = false;
		float bestDepth = 0.0f;
		for (const auto& body : m_candidates) {
			glm::vec3 push(0.0f);
			float depth = 0.0f;
			if (!ShapeOverlap::Test(m_probe, *body, 0.0f, push, depth)) continue;
			if (!any || depth > bestDepth) {
				any = true;
				bestDepth = depth;
				m_push = push;
				m_depth = depth;
				m_body = body;
			}
		}
		return any;
	}

	glm::vec3 m_push{ 0.0f, 1.0f, 0.0f };
	float m_depth = 0.0f;
	std::shared_ptr<RigidBody> m_body;

private:
	RigidBody& m_probe;
	const std::vector<std::shared_ptr<RigidBody>>& m_candidates;
};

} // namespace

bool Sweep(PhysicsEngine& physics, const Probe& probe, const glm::vec3& dir, float maxDistance, uint32_t mask,
	Hit& out, const RigidBody* ignore) {
	out = Hit{};
	const float len = glm::length(dir);
	if (!(len > 1e-8f) || !std::isfinite(len) || !(maxDistance > 0.0f) || !std::isfinite(maxDistance) || !Finite(probe.center)) {
		return false;
	}
	const glm::vec3 d = dir / len;

	RigidBody probeBody;
	MakeProbeBody(probe, probeBody);

	// Candidates: bodies whose AABB meets the swept volume (both end positions plus the probe's own extent).
	probeBody.setPosition(probe.center);
	probeBody.computeAABB();
	const glm::vec3 startMin = probeBody.getAABBMin(), startMax = probeBody.getAABBMax();
	probeBody.setPosition(probe.center + d * maxDistance);
	probeBody.computeAABB();
	const glm::vec3 margin(0.05f);
	const glm::vec3 mn = glm::min(startMin, probeBody.getAABBMin()) - margin;
	const glm::vec3 mx = glm::max(startMax, probeBody.getAABBMax()) + margin;

	std::vector<std::shared_ptr<RigidBody>> candidates;
	for (const auto& body : physics.QueryAABB(mn, mx)) {
		if (!body || body.get() == ignore) continue;
		if ((body->getCollisionLayer() & mask) == 0u) continue;
		candidates.push_back(body);
	}
	if (candidates.empty()) return false;

	const float minRadius = probe.kind == Kind::BOX
		? std::max(0.01f, std::min(probe.halfExtents.x, std::min(probe.halfExtents.y, probe.halfExtents.z)))
		: std::max(0.01f, probe.radius);
	float step = std::max(0.01f, 0.5f * minRadius);
	step = std::max(step, maxDistance / static_cast<float>(kMaxSteps));

	Tester test(probeBody, candidates);
	auto fill = [&](float distance) {
		const glm::vec3 centre = probe.center + d * distance;
		out.hit = true;
		out.distance = distance;
		out.normal = glm::normalize(test.m_push);
		out.body = test.m_body;
		out.point = centre - out.normal * SurfaceExtent(probe, out.normal);
	};

	if (test.At(probe.center)) {
		fill(0.0f);
		return true;
	}
	float t = 0.0f;
	while (t < maxDistance) {
		const float next = std::min(maxDistance, t + step);
		if (test.At(probe.center + d * next)) {
			float lo = t, hi = next;   // lo is clear, hi overlaps
			for (int i = 0; i < kBisections; ++i) {
				const float mid = 0.5f * (lo + hi);
				if (test.At(probe.center + d * mid)) hi = mid;
				else lo = mid;
			}
			test.At(probe.center + d * hi);
			fill(hi);
			return true;
		}
		t = next;
	}
	return false;
}

} // namespace ShapeSweep

#include "ShapeOverlap.h"

#include "MeshCollider.h"
#include "../RigidBody.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace ShapeOverlap {

namespace {

using ShapeType = RigidBody::ShapeType;

constexpr int kMaxSamples = 8;

struct Push {
	glm::vec3 direction{ 0.0f, 1.0f, 0.0f };   // pushes the probe out of the body
	float depth = 0.0f;
};

// Sphere (c, r) against sphere (sc, sr). The push moves the first sphere away from the second.
bool SphereSphere(const glm::vec3& c, float r, const glm::vec3& sc, float sr, float skin, Push& out) {
	const glm::vec3 d = c - sc;
	const float dist = glm::length(d);
	const float depth = r + sr - dist;
	if (depth < -skin) return false;
	out.direction = dist > 1e-6f ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f);
	out.depth = depth;
	return true;
}

// Sphere (c, r) against an oriented box. The push moves the sphere out of the box.
bool SphereBox(const glm::vec3& c, float r, const RigidBody& box, float skin, Push& out) {
	const glm::mat3 R = glm::mat3_cast(box.getOrientation());
	const glm::vec3 local = glm::transpose(R) * (c - box.getPosition());
	const glm::vec3 h = box.getHalfExtents();
	const glm::vec3 diff = local - glm::clamp(local, -h, h);
	const float dist2 = glm::dot(diff, diff);
	if (dist2 > 1e-12f) {
		const float dist = std::sqrt(dist2);
		const float depth = r - dist;
		if (depth < -skin) return false;
		out.direction = R * (diff / dist);
		out.depth = depth;
		return true;
	}
	// Centre inside the box: leave through the nearest face.
	int axis = 0;
	float gap = std::numeric_limits<float>::max();
	float sign = 1.0f;
	for (int i = 0; i < 3; ++i) {
		const float g = h[i] - std::abs(local[i]);
		if (g < gap) {
			gap = g;
			axis = i;
			sign = local[i] >= 0.0f ? 1.0f : -1.0f;
		}
	}
	glm::vec3 localPush(0.0f);
	localPush[axis] = sign;
	out.direction = R * localPush;
	out.depth = r + gap;
	return true;
}

// Sphere (c, r) against a one-sided plane. The sphere is pushed up along the normal.
bool SpherePlane(const glm::vec3& c, float r, const RigidBody& plane, float skin, Push& out) {
	const glm::vec3 n = glm::normalize(plane.getPlaneNormal());
	const float dist = glm::dot(c, n) - plane.getPlaneDistance();
	const float depth = r - dist;
	if (depth < -skin) return false;
	out.direction = n;
	out.depth = depth;
	return true;
}

// Sphere (c, r) against a mesh body: the deepest triangle contact. The push is the contact normal (mesh to sphere).
bool SphereMesh(const glm::vec3& c, float r, const RigidBody& mesh, float skin, Push& out) {
	CollisionWorld::SphereContact contact;
	if (!MeshCollider::DeepestSphereContact(mesh, c, r, skin, contact)) return false;
	out.direction = contact.normal;
	out.depth = contact.depth;
	return true;
}

// Sample centres of a capsule's sphere decomposition (both ends included, spacing at most one radius).
int CapsuleSamples(const RigidBody& capsule, glm::vec3* out) {
	glm::vec3 a, b;
	capsule.getCapsuleEnds(a, b);
	const float len = glm::length(b - a);
	const float r = std::max(capsule.getRadius(), 1e-4f);
	const int n = std::clamp(static_cast<int>(std::ceil(len / r)) + 1, 2, kMaxSamples);
	for (int i = 0; i < n; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(n - 1);
		out[i] = glm::mix(a, b, t);
	}
	return n;
}

// Sphere (c, r) against any body. Capsules are tested sample by sample; the deepest sample decides.
bool SphereVsBody(const glm::vec3& c, float r, const RigidBody& b, float skin, Push& out) {
	switch (b.getShapeType()) {
	case ShapeType::SPHERE:
		return SphereSphere(c, r, b.getPosition(), b.getRadius(), skin, out);
	case ShapeType::BOX:
		return SphereBox(c, r, b, skin, out);
	case ShapeType::PLANE:
		return SpherePlane(c, r, b, skin, out);
	case ShapeType::MESH:
		return SphereMesh(c, r, b, skin, out);
	case ShapeType::CAPSULE: {
		glm::vec3 samples[kMaxSamples];
		const int n = CapsuleSamples(b, samples);
		bool any = false;
		Push best;
		for (int i = 0; i < n; ++i) {
			Push p;
			if (SphereSphere(c, r, samples[i], b.getRadius(), skin, p) && (!any || p.depth > best.depth)) {
				best = p;
				any = true;
			}
		}
		if (any) out = best;
		return any;
	}
	}
	return false;
}

// Box a against b. Sphere, capsule (sampled), box and plane are exact or sampled; mesh is not supported.
bool BoxVsBody(const RigidBody& a, const RigidBody& b, float skin, Push& out) {
	switch (b.getShapeType()) {
	case ShapeType::SPHERE: {
		if (!SphereBox(b.getPosition(), b.getRadius(), a, skin, out)) return false;
		out.direction = -out.direction;
		return true;
	}
	case ShapeType::CAPSULE: {
		glm::vec3 samples[kMaxSamples];
		const int n = CapsuleSamples(b, samples);
		bool any = false;
		Push best;
		for (int i = 0; i < n; ++i) {
			Push p;
			if (SphereBox(samples[i], b.getRadius(), a, skin, p) && (!any || p.depth > best.depth)) {
				best = p;
				any = true;
			}
		}
		if (!any) return false;
		best.direction = -best.direction;
		out = best;
		return true;
	}
	case ShapeType::BOX: {
		const glm::mat3 Ra = glm::mat3_cast(a.getOrientation());
		const glm::mat3 Rb = glm::mat3_cast(b.getOrientation());
		const glm::vec3 ha = a.getHalfExtents();
		const glm::vec3 hb = b.getHalfExtents();
		const glm::vec3 d = a.getPosition() - b.getPosition();

		glm::vec3 axes[15];
		int count = 0;
		for (int i = 0; i < 3; ++i) axes[count++] = Ra[i];
		for (int i = 0; i < 3; ++i) axes[count++] = Rb[i];
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) axes[count++] = glm::cross(Ra[i], Rb[j]);
		}

		float bestOverlap = std::numeric_limits<float>::max();
		glm::vec3 bestAxis(0.0f, 1.0f, 0.0f);
		for (int k = 0; k < count; ++k) {
			const float len = glm::length(axes[k]);
			if (len < 1e-6f) continue;   // parallel edges give a degenerate axis
			const glm::vec3 L = axes[k] / len;
			const float ra = ha.x * std::abs(glm::dot(L, Ra[0])) + ha.y * std::abs(glm::dot(L, Ra[1])) + ha.z * std::abs(glm::dot(L, Ra[2]));
			const float rb = hb.x * std::abs(glm::dot(L, Rb[0])) + hb.y * std::abs(glm::dot(L, Rb[1])) + hb.z * std::abs(glm::dot(L, Rb[2]));
			const float dist = glm::dot(d, L);
			const float overlap = ra + rb - std::abs(dist);
			if (overlap < -skin) return false;
			if (overlap < bestOverlap) {
				bestOverlap = overlap;
				bestAxis = dist >= 0.0f ? L : -L;
			}
		}
		out.direction = bestAxis;
		out.depth = bestOverlap;
		return true;
	}
	case ShapeType::PLANE: {
		const glm::vec3 n = glm::normalize(b.getPlaneNormal());
		const glm::mat3 R = glm::mat3_cast(a.getOrientation());
		const glm::vec3 h = a.getHalfExtents();
		const float dist = glm::dot(a.getPosition(), n) - b.getPlaneDistance();
		const float extent = std::abs(glm::dot(n, R[0])) * h.x + std::abs(glm::dot(n, R[1])) * h.y +
			std::abs(glm::dot(n, R[2])) * h.z;
		const float depth = extent - dist;
		if (depth < -skin) return false;
		out.direction = n;
		out.depth = depth;
		return true;
	}
	case ShapeType::MESH: {
		// Box against triangles: the deepest box/triangle contact. The push is the contact normal (mesh to box), which
		// is the direction that moves a out of b.
		std::vector<CollisionWorld::SphereContact> contacts;
		MeshCollider::BoxContacts(b, a.getPosition(), a.getHalfExtents(), a.getOrientation(), skin, contacts);
		if (contacts.empty()) return false;
		const CollisionWorld::SphereContact* deepest = &contacts[0];
		for (const auto& c : contacts) {
			if (c.depth > deepest->depth) deepest = &c;
		}
		out.direction = deepest->normal;
		out.depth = deepest->depth;
		return true;
	}
	}
	return false;
}

} // namespace

bool Test(const RigidBody& a, const RigidBody& b, float skin, glm::vec3& pushA, float& depth) {
	const ShapeType ta = a.getShapeType();
	const ShapeType tb = b.getShapeType();

	// Planes and meshes are static surfaces: a plane never overlaps a plane or a mesh, and a mesh is only tested
	// against sphere-like probes. The probe's push is computed against the surface and negated for the surface.
	if (ta == ShapeType::PLANE) {
		if (tb == ShapeType::PLANE || tb == ShapeType::MESH) return false;
		if (!Test(b, a, skin, pushA, depth)) return false;
		pushA = -pushA;
		return true;
	}
	if (ta == ShapeType::MESH) {
		if (tb == ShapeType::MESH || tb == ShapeType::PLANE) return false;
		if (!Test(b, a, skin, pushA, depth)) return false;
		pushA = -pushA;
		return true;
	}

	if (ta == ShapeType::BOX) {
		Push push;
		if (!BoxVsBody(a, b, skin, push)) return false;
		pushA = push.direction;
		depth = push.depth;
		return true;
	}

	// Sphere or capsule probe: the deepest of its sample spheres decides.
	glm::vec3 samples[kMaxSamples];
	int n = 1;
	if (ta == ShapeType::CAPSULE) {
		n = CapsuleSamples(a, samples);
	}
	else {
		samples[0] = a.getPosition();
	}
	bool any = false;
	Push best;
	for (int i = 0; i < n; ++i) {
		Push p;
		if (SphereVsBody(samples[i], a.getRadius(), b, skin, p) && (!any || p.depth > best.depth)) {
			best = p;
			any = true;
		}
	}
	if (!any) return false;
	pushA = best.direction;
	depth = best.depth;
	return true;
}

} // namespace ShapeOverlap

#include "ShapeOverlap.h"

#include "../RigidBody.h"

#include <cmath>
#include <limits>

namespace ShapeOverlap {

namespace {

using ShapeType = RigidBody::ShapeType;

struct Push {
	glm::vec3 direction{ 0.0f, 1.0f, 0.0f };   // pushes the first shape out of the second
	float depth = 0.0f;
};

bool SphereSphere(const RigidBody& a, const RigidBody& b, float skin, Push& out) {
	const glm::vec3 d = a.getPosition() - b.getPosition();
	const float dist = glm::length(d);
	const float depth = a.getRadius() + b.getRadius() - dist;
	if (depth < -skin) return false;
	out.direction = dist > 1e-6f ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f);
	out.depth = depth;
	return true;
}

// Sphere a against box b; the push moves the sphere out of the box.
bool SphereBox(const RigidBody& sphere, const RigidBody& box, float skin, Push& out) {
	const glm::mat3 R = glm::mat3_cast(box.getOrientation());
	const glm::vec3 local = glm::transpose(R) * (sphere.getPosition() - box.getPosition());
	const glm::vec3 h = box.getHalfExtents();
	const float radius = sphere.getRadius();
	const glm::vec3 diff = local - glm::clamp(local, -h, h);
	const float dist2 = glm::dot(diff, diff);
	if (dist2 > 1e-12f) {
		const float dist = std::sqrt(dist2);
		const float depth = radius - dist;
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
	out.depth = radius + gap;
	return true;
}

// Sphere a against plane b (one-sided: the sphere is pushed up along the normal).
bool SpherePlane(const RigidBody& sphere, const RigidBody& plane, float skin, Push& out) {
	const glm::vec3 n = glm::normalize(plane.getPlaneNormal());
	const float dist = glm::dot(sphere.getPosition(), n) - plane.getPlaneDistance();
	const float depth = sphere.getRadius() - dist;
	if (depth < -skin) return false;
	out.direction = n;
	out.depth = depth;
	return true;
}

// Box a against plane b.
bool BoxPlane(const RigidBody& box, const RigidBody& plane, float skin, Push& out) {
	const glm::vec3 n = glm::normalize(plane.getPlaneNormal());
	const glm::mat3 R = glm::mat3_cast(box.getOrientation());
	const glm::vec3 h = box.getHalfExtents();
	const float dist = glm::dot(box.getPosition(), n) - plane.getPlaneDistance();
	const float extent = std::abs(glm::dot(n, R[0])) * h.x + std::abs(glm::dot(n, R[1])) * h.y +
		std::abs(glm::dot(n, R[2])) * h.z;
	const float depth = extent - dist;
	if (depth < -skin) return false;
	out.direction = n;
	out.depth = depth;
	return true;
}

// Box a against box b: separating axis test over the 15 candidate axes; the push uses the axis of least overlap.
bool BoxBox(const RigidBody& a, const RigidBody& b, float skin, Push& out) {
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

// a is never a plane here. b may be any shape but a plane-plane pair (handled by the caller).
bool TestNonPlane(const RigidBody& a, const RigidBody& b, float skin, Push& out) {
	const ShapeType ta = a.getShapeType();
	const ShapeType tb = b.getShapeType();
	if (ta == ShapeType::SPHERE) {
		if (tb == ShapeType::SPHERE) return SphereSphere(a, b, skin, out);
		if (tb == ShapeType::BOX) return SphereBox(a, b, skin, out);
		return SpherePlane(a, b, skin, out);
	}
	if (ta == ShapeType::BOX) {
		if (tb == ShapeType::SPHERE) {
			if (!SphereBox(b, a, skin, out)) return false;
			out.direction = -out.direction;
			return true;
		}
		if (tb == ShapeType::BOX) return BoxBox(a, b, skin, out);
		return BoxPlane(a, b, skin, out);
	}
	return false;
}

} // namespace

bool Test(const RigidBody& a, const RigidBody& b, float skin, glm::vec3& pushA, float& depth) {
	Push push;
	if (a.getShapeType() == ShapeType::PLANE) {
		if (b.getShapeType() == ShapeType::PLANE) return false;
		if (!TestNonPlane(b, a, skin, push)) return false;
		pushA = -push.direction;   // the push was computed for b, away from the plane
	}
	else {
		if (!TestNonPlane(a, b, skin, push)) return false;
		pushA = push.direction;
	}
	depth = push.depth;
	return true;
}

} // namespace ShapeOverlap

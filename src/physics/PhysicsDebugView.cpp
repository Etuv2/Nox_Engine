#include "PhysicsDebugView.h"

#include "../PhysicsEngine.h"
#include "../RigidBody.h"
#include "../SceneNode.h"
#include "../diagnostics/DebugDraw.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace {

constexpr uint32_t kStaticColor = 0xFF8A8A8Au;
constexpr uint32_t kKinematicColor = 0xFFF0A030u;
constexpr uint32_t kAwakeColor = 0xFF40E070u;
constexpr uint32_t kSleepingColor = 0xFF4A90E2u;
constexpr uint32_t kPlaneColor = 0xFFB0B0B0u;
constexpr uint32_t kAabbColor = 0x80FFFFFFu;
constexpr uint32_t kContactColor = 0xFFFF4040u;
constexpr uint32_t kNormalColor = 0xFFFFFF40u;
constexpr uint32_t kVelocityColor = 0xFF40C0FFu;
constexpr uint32_t kTriggerColor = 0xFFE040E0u;
constexpr uint32_t kCharacterColor = 0xFFFFFFFFu;
constexpr float kVelocityScale = 0.1f;      // seconds of motion drawn
constexpr float kNormalLength = 0.25f;      // metres
constexpr float kContactRadius = 0.03f;     // metres
constexpr float kSleepMarkerRadius = 0.08f; // metres
constexpr float kPlaneHalfSize = 5.0f;      // metres

uint32_t BodyColor(const RigidBody& body, const PhysicsDebugView& view) {
	if (body.IsStatic()) return kStaticColor;
	if (body.IsKinematic()) return kKinematicColor;
	if (view.sleeping && body.isSleeping()) return kSleepingColor;
	return kAwakeColor;
}

// The eight corners of a box with half extents `half` placed by `xf`, joined by its twelve edges.
void DrawOrientedBox(const glm::mat4& xf, const glm::vec3& half, uint32_t color) {
	glm::vec3 corner[8];
	for (int i = 0; i < 8; ++i) {
		const glm::vec3 local((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z);
		corner[i] = glm::vec3(xf * glm::vec4(local, 1.0f));
	}
	for (int i = 0; i < 8; ++i) {
		for (int axis = 0; axis < 3; ++axis) {
			const int j = i ^ (1 << axis);
			if (i < j) DebugDraw::AddLine(corner[i], corner[j], color, 0.0f);
		}
	}
}

void DrawPlanePatch(const glm::vec3& normal, float distance, uint32_t color) {
	const glm::vec3 n = glm::normalize(normal);
	const glm::vec3 center = n * distance;
	const glm::vec3 helper = std::abs(n.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
	const glm::vec3 t1 = glm::normalize(glm::cross(n, helper)) * kPlaneHalfSize;
	const glm::vec3 t2 = glm::normalize(glm::cross(n, t1)) * kPlaneHalfSize;
	const glm::vec3 p0 = center - t1 - t2, p1 = center + t1 - t2, p2 = center + t1 + t2, p3 = center - t1 + t2;
	DebugDraw::AddLine(p0, p1, color, 0.0f);
	DebugDraw::AddLine(p1, p2, color, 0.0f);
	DebugDraw::AddLine(p2, p3, color, 0.0f);
	DebugDraw::AddLine(p3, p0, color, 0.0f);
	DebugDraw::AddLine(p0, p2, color, 0.0f);
	DebugDraw::AddLine(p1, p3, color, 0.0f);
}

void DrawBody(const RigidBody& body, const PhysicsDebugView& view) {
	const glm::vec3 position = body.getPosition();
	const uint32_t color = BodyColor(body, view);
	if (view.colliders) {
		switch (body.getShapeType()) {
		case RigidBody::ShapeType::SPHERE:
			DebugDraw::AddSphere(position, body.getRadius(), color, 0.0f);
			break;
		case RigidBody::ShapeType::BOX: {
			const glm::mat4 xf = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(body.getOrientation());
			DrawOrientedBox(xf, body.getHalfExtents(), color);
			break;
		}
		case RigidBody::ShapeType::PLANE:
			DrawPlanePatch(body.getPlaneNormal(), body.getPlaneDistance(), kPlaneColor);
			break;
		}
	}
	if (view.aabbs) {
		const glm::vec3 mn = body.getAABBMin(), mx = body.getAABBMax();
		DebugDraw::AddBox((mn + mx) * 0.5f, (mx - mn) * 0.5f, kAabbColor, 0.0f);
	}
	if (view.sleeping && body.isSleeping()) {
		DebugDraw::AddSphere(position, kSleepMarkerRadius, kSleepingColor, 0.0f);
	}
	if (view.velocities && body.IsDynamic() && !body.isSleeping()) {
		DebugDraw::AddLine(position, position + body.getVelocity() * kVelocityScale, kVelocityColor, 0.0f);
	}
}

} // namespace

void QueuePhysicsDebugDraw(const PhysicsEngine& physics, const PhysicsDebugView& view) {
	if (!view.Any()) return;

	if (view.colliders || view.aabbs || view.sleeping || view.velocities) {
		for (const auto& body : physics.GetBodies()) {
			if (body) DrawBody(*body, view);
		}
	}

	if (view.contacts) {
		for (const ContactManifold* manifold : physics.GetActiveManifolds()) {
			if (!manifold) continue;
			for (int i = 0; i < manifold->pointCount && i < ContactManifold::MAX_CONTACTS; ++i) {
				const glm::vec3 point = manifold->points[i].point;
				DebugDraw::AddSphere(point, kContactRadius, kContactColor, 0.0f);
				DebugDraw::AddLine(point, point + manifold->normal * kNormalLength, kNormalColor, 0.0f);
			}
		}
	}

	if (view.triggers) {
		physics.ForEachTrigger([](const SceneNode& node, const TriggerSettings& settings) {
			const glm::mat4 xf = node.GetWorldPosition4x4();
			if (settings.shape == TriggerSettings::Shape::SPHERE) {
				DebugDraw::AddSphere(glm::vec3(xf[3]), settings.radius, kTriggerColor, 0.0f);
			}
			else {
				DrawOrientedBox(xf, settings.halfExtents, kTriggerColor);
			}
		});
	}

	if (view.characters) {
		physics.ForEachCharacter([](const SceneNode& node, const CharacterSettings& settings) {
			// The node position is the capsule's feet; the capsule is upright.
			const glm::vec3 feet = glm::vec3(node.GetWorldPosition4x4()[3]);
			const float r = settings.radius;
			const float top = std::max(settings.height - r, r);
			const glm::vec3 bottomCenter = feet + glm::vec3(0.0f, r, 0.0f);
			const glm::vec3 topCenter = feet + glm::vec3(0.0f, top, 0.0f);
			DebugDraw::AddSphere(bottomCenter, r, kCharacterColor, 0.0f);
			DebugDraw::AddSphere(topCenter, r, kCharacterColor, 0.0f);
			const glm::vec3 sides[4] = { { r, 0.0f, 0.0f }, { -r, 0.0f, 0.0f }, { 0.0f, 0.0f, r }, { 0.0f, 0.0f, -r } };
			for (const glm::vec3& side : sides) {
				DebugDraw::AddLine(bottomCenter + side, topCenter + side, kCharacterColor, 0.0f);
			}
		});
	}
}

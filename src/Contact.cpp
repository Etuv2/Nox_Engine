#include "Contact.h"
#include "RigidBody.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {

constexpr float kContactMergeDistanceSq = 0.0001f;

bool ContactPointLess(const ContactPoint& lhs, const ContactPoint& rhs) {
    if (lhs.penetration != rhs.penetration) {
        return lhs.penetration > rhs.penetration;
    }
    if (lhs.featureId != rhs.featureId) {
        return lhs.featureId < rhs.featureId;
    }
    if (lhs.localPointA.x != rhs.localPointA.x) return lhs.localPointA.x < rhs.localPointA.x;
    if (lhs.localPointA.y != rhs.localPointA.y) return lhs.localPointA.y < rhs.localPointA.y;
    if (lhs.localPointA.z != rhs.localPointA.z) return lhs.localPointA.z < rhs.localPointA.z;
    if (lhs.localPointB.x != rhs.localPointB.x) return lhs.localPointB.x < rhs.localPointB.x;
    if (lhs.localPointB.y != rhs.localPointB.y) return lhs.localPointB.y < rhs.localPointB.y;
    return lhs.localPointB.z < rhs.localPointB.z;
}

bool ContactPointsEquivalent(const ContactPoint& lhs, const ContactPoint& rhs) {
    if (lhs.featureId != 0 && rhs.featureId != 0) {
        return lhs.featureId == rhs.featureId;
    }

    const glm::vec3 delta = lhs.point - rhs.point;
    return glm::dot(delta, delta) <= kContactMergeDistanceSq;
}

void PreserveCachedImpulses(ContactPoint& dst, const ContactPoint& src) {
    dst.normalImpulseAccum = src.normalImpulseAccum;
    dst.tangentImpulseAccum1 = src.tangentImpulseAccum1;
    dst.tangentImpulseAccum2 = src.tangentImpulseAccum2;
}

} // namespace

void ContactManifold::addPoint(const ContactPoint& newPoint) {
    for (int i = 0; i < pointCount; ++i) {
        if (!ContactPointsEquivalent(points[i], newPoint)) {
            continue;
        }

        ContactPoint merged = newPoint;
        PreserveCachedImpulses(merged, points[i]);
        points[i] = merged;
        stabilizePointOrder();
        return;
    }

    std::array<ContactPoint, MAX_CONTACTS + 1> candidates{};
    int candidateCount = 0;
    for (int i = 0; i < pointCount; ++i) {
        candidates[candidateCount++] = points[i];
    }
    candidates[candidateCount++] = newPoint;

    std::sort(candidates.begin(), candidates.begin() + candidateCount, ContactPointLess);

    if (candidateCount <= MAX_CONTACTS) {
        pointCount = candidateCount;
        for (int i = 0; i < pointCount; ++i) {
            points[i] = candidates[i];
        }
        stabilizePointOrder();
        return;
    }

    std::array<ContactPoint, MAX_CONTACTS> selected{};
    std::array<bool, MAX_CONTACTS + 1> used{};

    selected[0] = candidates[0];
    used[0] = true;
    int selectedCount = 1;

    while (selectedCount < MAX_CONTACTS) {
        float bestScore = -1.0f;
        int bestIndex = -1;

        for (int candidateIndex = 1; candidateIndex < candidateCount; ++candidateIndex) {
            if (used[candidateIndex]) {
                continue;
            }

            float minDistanceSq = std::numeric_limits<float>::max();
            for (int chosenIndex = 0; chosenIndex < selectedCount; ++chosenIndex) {
                const glm::vec3 delta = candidates[candidateIndex].point - selected[chosenIndex].point;
                minDistanceSq = std::min(minDistanceSq, glm::dot(delta, delta));
            }

            if (minDistanceSq > bestScore) {
                bestScore = minDistanceSq;
                bestIndex = candidateIndex;
            } else if (bestIndex >= 0 && minDistanceSq == bestScore &&
                       ContactPointLess(candidates[candidateIndex], candidates[bestIndex])) {
                bestIndex = candidateIndex;
            }
        }

        if (bestIndex < 0) {
            break;
        }

        selected[selectedCount++] = candidates[bestIndex];
        used[bestIndex] = true;
    }

    pointCount = selectedCount;
    for (int i = 0; i < pointCount; ++i) {
        points[i] = selected[i];
    }
    stabilizePointOrder();
}

void ContactManifold::stabilizePointOrder() {
    std::sort(points.begin(), points.begin() + pointCount, ContactPointLess);
}

void ContactManifold::computeTangentBasis() {
	// Build orthonormal basis from normal
	// Choose reference axis that's not parallel to normal
	glm::vec3 reference = (std::abs(normal.x) < 0.9f) ?
		glm::vec3(1.0f, 0.0f, 0.0f) :
		glm::vec3(0.0f, 1.0f, 0.0f);

	tangent1 = glm::normalize(glm::cross(normal, reference));
	tangent2 = glm::cross(normal, tangent1);
}

uint64_t ContactManifold::makePairKey(uint32_t idA, uint32_t idB) {
	// Ensure consistent ordering
	if (idA > idB) std::swap(idA, idB);
	return (static_cast<uint64_t>(idA) << 32) | static_cast<uint64_t>(idB);
}

void ContactManifold::refreshContacts(float breakingThreshold) {
	if (!bodyA || !bodyB) {
		pointCount = 0;
		return;
	}

	// Update world positions from local coordinates
	updateWorldPositions();

	// Remove separated contacts
	int writeIndex = 0;
	for (int i = 0; i < pointCount; i++) {
		// Compute separation in normal direction
		glm::vec3 worldPointA = bodyA->localToWorld(points[i].localPointA);
		glm::vec3 worldPointB = bodyB->localToWorld(points[i].localPointB);
		glm::vec3 diff = worldPointB - worldPointA;

		float normalSeparation = glm::dot(diff, normal);

		// Also check tangential drift
		glm::vec3 tangentialDiff = diff - normal * normalSeparation;
		float tangentialDist = glm::length(tangentialDiff);

		// Keep point if still in contact
		if (normalSeparation < breakingThreshold && tangentialDist < breakingThreshold * 2.0f) {
			// Update penetration
			points[i].penetration = -normalSeparation;
			points[i].point = (worldPointA + worldPointB) * 0.5f;

			if (writeIndex != i) {
				points[writeIndex] = points[i];
			}
			writeIndex++;
		}
	}

	pointCount = writeIndex;
    stabilizePointOrder();
}

void ContactManifold::warmStart(float warmStartFactor) {
	if (!bodyA || !bodyB || warmStartFactor <= 0.0f) return;

	// Validate that bodies can receive impulses
	bool aCanReceive = bodyA->IsDynamic() && !bodyA->isEditorControlled();
	bool bCanReceive = bodyB->IsDynamic() && !bodyB->isEditorControlled();
	if (!aCanReceive && !bCanReceive) return;

	for (int i = 0; i < pointCount; i++) {
		ContactPoint& cp = points[i];

		// Skip if no cached impulse
		if (cp.normalImpulseAccum == 0.0f &&
			cp.tangentImpulseAccum1 == 0.0f &&
			cp.tangentImpulseAccum2 == 0.0f) {
			continue;
		}

		// Scale cached impulses
		float normalImpulse = cp.normalImpulseAccum * warmStartFactor;
		float tangent1Impulse = cp.tangentImpulseAccum1 * warmStartFactor;
		float tangent2Impulse = cp.tangentImpulseAccum2 * warmStartFactor;

		// Validate impulse magnitude to prevent explosion
		// Large cached impulses can cause instability after scene changes
		float maxImpulse = 100.0f; // Reasonable max impulse
		normalImpulse = glm::clamp(normalImpulse, 0.0f, maxImpulse);
		tangent1Impulse = glm::clamp(tangent1Impulse, -maxImpulse, maxImpulse);
		tangent2Impulse = glm::clamp(tangent2Impulse, -maxImpulse, maxImpulse);

		// Compute total impulse vector
		glm::vec3 impulse = normal * normalImpulse +
			tangent1 * tangent1Impulse +
			tangent2 * tangent2Impulse;

		// Validate impulse is finite
		if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) || !std::isfinite(impulse.z)) {
			// Reset cached impulses if corrupted
			cp.normalImpulseAccum = 0.0f;
			cp.tangentImpulseAccum1 = 0.0f;
			cp.tangentImpulseAccum2 = 0.0f;
			continue;
		}

		// Apply impulses
		if (glm::length(impulse) > 1e-6f) {
			bodyA->applyImpulseAtPoint(-impulse, cp.point);
			bodyB->applyImpulseAtPoint(impulse, cp.point);
		}
	}
}

void ContactManifold::updateWorldPositions() {
	if (!bodyA || !bodyB) return;

	for (int i = 0; i < pointCount; i++) {
		glm::vec3 worldPointA = bodyA->localToWorld(points[i].localPointA);
		glm::vec3 worldPointB = bodyB->localToWorld(points[i].localPointB);
		points[i].point = (worldPointA + worldPointB) * 0.5f;
	}
}

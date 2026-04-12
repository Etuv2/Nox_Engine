#include "PhysicsCollision.h"
#include "RigidBody.h"
#include "Contact.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>

namespace PhysicsCollision {
namespace {

struct SupportVertex {
    glm::vec3 pointA{0.0f};
    glm::vec3 pointB{0.0f};
    glm::vec3 minkowski{0.0f};
};

bool HasConvexSupport(RigidBody::ShapeType shapeType) {
    return shapeType == RigidBody::ShapeType::BOX || shapeType == RigidBody::ShapeType::SPHERE;
}

glm::vec3 SupportPointForBody(const std::shared_ptr<RigidBody>& body, const glm::vec3& direction) {
    if (!body) {
        return glm::vec3(0.0f);
    }

    glm::vec3 dir = direction;
    if (glm::dot(dir, dir) < 1e-8f) {
        dir = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    switch (body->getShapeType()) {
    case RigidBody::ShapeType::SPHERE:
        return body->getPosition() + glm::normalize(dir) * body->getRadius();

    case RigidBody::ShapeType::BOX: {
        glm::vec3 localDir = body->worldToLocalDirection(dir);
        glm::vec3 localSupport(
            localDir.x >= 0.0f ? body->getHalfExtents().x : -body->getHalfExtents().x,
            localDir.y >= 0.0f ? body->getHalfExtents().y : -body->getHalfExtents().y,
            localDir.z >= 0.0f ? body->getHalfExtents().z : -body->getHalfExtents().z);
        return body->localToWorld(localSupport);
    }

    case RigidBody::ShapeType::PLANE:
    default:
        return body->getPosition();
    }
}

SupportVertex GetSupportVertex(const std::shared_ptr<RigidBody>& bodyA,
                               const std::shared_ptr<RigidBody>& bodyB,
                               const glm::vec3& direction) {
    SupportVertex vertex;
    vertex.pointA = SupportPointForBody(bodyA, direction);
    vertex.pointB = SupportPointForBody(bodyB, -direction);
    vertex.minkowski = vertex.pointA - vertex.pointB;
    return vertex;
}

uint32_t QuantizeContactCoordinate(float value) {
    const float scaled = glm::clamp(value * 1024.0f, -2048.0f, 2047.0f);
    return static_cast<uint32_t>(static_cast<int32_t>(std::round(scaled)) + 2048);
}

uint32_t MakeFeatureId(const glm::vec3& localPointA, const glm::vec3& localPointB) {
    uint32_t hash = 2166136261u;
    const std::array<uint32_t, 6> values = {
        QuantizeContactCoordinate(localPointA.x),
        QuantizeContactCoordinate(localPointA.y),
        QuantizeContactCoordinate(localPointA.z),
        QuantizeContactCoordinate(localPointB.x),
        QuantizeContactCoordinate(localPointB.y),
        QuantizeContactCoordinate(localPointB.z)
    };

    for (uint32_t value : values) {
        hash ^= value;
        hash *= 16777619u;
    }

    return hash;
}

void AssignFeatureId(ContactPoint& cp) {
    cp.featureId = MakeFeatureId(cp.localPointA, cp.localPointB);
}

void ReorientManifold(ContactManifold& manifold,
                      const std::shared_ptr<RigidBody>& expectedA,
                      const std::shared_ptr<RigidBody>& expectedB) {
    if (manifold.bodyA == expectedA && manifold.bodyB == expectedB) {
        return;
    }

    if (manifold.bodyA == expectedB && manifold.bodyB == expectedA) {
        std::swap(manifold.bodyA, manifold.bodyB);
        manifold.normal = -manifold.normal;
        for (int i = 0; i < manifold.pointCount; ++i) {
            std::swap(manifold.points[i].localPointA, manifold.points[i].localPointB);
            AssignFeatureId(manifold.points[i]);
        }
        manifold.computeTangentBasis();
    }
}

bool UpdateSimplex(std::vector<SupportVertex>& simplex, glm::vec3& direction) {
    const glm::vec3 origin(0.0f);
    const SupportVertex& a = simplex.back();
    const glm::vec3 ao = origin - a.minkowski;

    if (simplex.size() == 2) {
        const glm::vec3 ab = simplex[0].minkowski - a.minkowski;
        direction = glm::cross(glm::cross(ab, ao), ab);
        if (glm::dot(direction, direction) < 1e-8f) {
            direction = glm::vec3(-ab.y, ab.x, 0.0f);
            if (glm::dot(direction, direction) < 1e-8f) {
                direction = glm::vec3(0.0f, -ab.z, ab.y);
            }
        }
        return false;
    }

    if (simplex.size() == 3) {
        const glm::vec3 ab = simplex[1].minkowski - a.minkowski;
        const glm::vec3 ac = simplex[0].minkowski - a.minkowski;
        const glm::vec3 abc = glm::cross(ab, ac);

        const glm::vec3 abPerp = glm::cross(abc, ab);
        if (glm::dot(abPerp, ao) > 0.0f) {
            simplex.erase(simplex.begin());
            direction = glm::cross(glm::cross(ab, ao), ab);
            return false;
        }

        const glm::vec3 acPerp = glm::cross(ac, abc);
        if (glm::dot(acPerp, ao) > 0.0f) {
            simplex.erase(simplex.begin() + 1);
            direction = glm::cross(glm::cross(ac, ao), ac);
            return false;
        }

        direction = (glm::dot(abc, ao) > 0.0f) ? abc : -abc;
        if (glm::dot(abc, ao) <= 0.0f) {
            std::swap(simplex[0], simplex[1]);
        }
        return false;
    }

    if (simplex.size() == 4) {
        const glm::vec3 ab = simplex[2].minkowski - a.minkowski;
        const glm::vec3 ac = simplex[1].minkowski - a.minkowski;
        const glm::vec3 ad = simplex[0].minkowski - a.minkowski;

        const glm::vec3 abc = glm::cross(ab, ac);
        const glm::vec3 acd = glm::cross(ac, ad);
        const glm::vec3 adb = glm::cross(ad, ab);

        if (glm::dot(abc, ao) > 0.0f) {
            simplex.erase(simplex.begin());
            direction = abc;
            return false;
        }
        if (glm::dot(acd, ao) > 0.0f) {
            simplex.erase(simplex.begin() + 2);
            direction = acd;
            return false;
        }
        if (glm::dot(adb, ao) > 0.0f) {
            simplex.erase(simplex.begin() + 1);
            direction = adb;
            return false;
        }

        return true;
    }

    direction = ao;
    return false;
}

bool ConvexOverlapGJK(const std::shared_ptr<RigidBody>& bodyA,
                      const std::shared_ptr<RigidBody>& bodyB) {
    std::vector<SupportVertex> simplex;
    simplex.reserve(4);

    glm::vec3 direction = bodyB->getPosition() - bodyA->getPosition();
    if (glm::dot(direction, direction) < 1e-8f) {
        direction = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    simplex.push_back(GetSupportVertex(bodyA, bodyB, direction));
    direction = -simplex.back().minkowski;

    for (int iteration = 0; iteration < 20; ++iteration) {
        SupportVertex candidate = GetSupportVertex(bodyA, bodyB, direction);
        if (glm::dot(candidate.minkowski, direction) <= 1e-5f) {
            return false;
        }

        simplex.push_back(candidate);
        if (UpdateSimplex(simplex, direction)) {
            return true;
        }
    }

    return false;
}

bool RequiresConvexPrepass(RigidBody::ShapeType typeA, RigidBody::ShapeType typeB) {
    if (!HasConvexSupport(typeA) || !HasConvexSupport(typeB)) {
        return false;
    }

    return typeA == RigidBody::ShapeType::BOX || typeB == RigidBody::ShapeType::BOX;
}

} // namespace

bool TestCollision(const std::shared_ptr<RigidBody>& bodyA,
                   const std::shared_ptr<RigidBody>& bodyB,
                   ContactManifold& manifold) {
    if (!bodyA || !bodyB) return false;
    
    auto typeA = bodyA->getShapeType();
    auto typeB = bodyB->getShapeType();

    // Transitional convex path: use GJK for convex overlap testing before
    // dispatching to the existing manifold builders. EPA/manifold generation
    // still needs follow-up work, but this gives us a shared convex seam now.
    if (RequiresConvexPrepass(typeA, typeB) && !ConvexOverlapGJK(bodyA, bodyB)) {
        return false;
    }
    
    // Dispatch based on shape types
    if (typeA == RigidBody::ShapeType::SPHERE) {
        if (typeB == RigidBody::ShapeType::SPHERE) {
            return SphereSphere(bodyA, bodyB, manifold);
        } else if (typeB == RigidBody::ShapeType::BOX) {
            return SphereBox(bodyA, bodyB, manifold);
        } else if (typeB == RigidBody::ShapeType::PLANE) {
            return SpherePlane(bodyA, bodyB, manifold);
        }
    } else if (typeA == RigidBody::ShapeType::BOX) {
        if (typeB == RigidBody::ShapeType::SPHERE) {
            bool result = SphereBox(bodyB, bodyA, manifold);
            if (result) {
                ReorientManifold(manifold, bodyA, bodyB);
            }
            return result;
        } else if (typeB == RigidBody::ShapeType::BOX) {
            return BoxBox(bodyA, bodyB, manifold);
        } else if (typeB == RigidBody::ShapeType::PLANE) {
            return BoxPlane(bodyA, bodyB, manifold);
        }
    } else if (typeA == RigidBody::ShapeType::PLANE) {
        if (typeB == RigidBody::ShapeType::SPHERE) {
            bool result = SpherePlane(bodyB, bodyA, manifold);
            if (result) {
                ReorientManifold(manifold, bodyA, bodyB);
            }
            return result;
        } else if (typeB == RigidBody::ShapeType::BOX) {
            bool result = BoxPlane(bodyB, bodyA, manifold);
            if (result) {
                ReorientManifold(manifold, bodyA, bodyB);
            }
            return result;
        }
        // Plane-Plane: no collision (parallel or same plane)
    }
    
    return false;
}

// ============== SPHERE COLLISIONS ==============

bool SphereSphere(const std::shared_ptr<RigidBody>& sphereA,
                  const std::shared_ptr<RigidBody>& sphereB,
                  ContactManifold& manifold) {
    glm::vec3 posA = sphereA->getPosition();
    glm::vec3 posB = sphereB->getPosition();
    float radiusA = sphereA->getRadius();
    float radiusB = sphereB->getRadius();
    
    glm::vec3 diff = posB - posA;
    float distSq = glm::dot(diff, diff);
    float radiusSum = radiusA + radiusB;
    
    if (distSq >= radiusSum * radiusSum) {
        return false;
    }
    
    float dist = std::sqrt(distSq);
    
    manifold.bodyA = sphereA;
    manifold.bodyB = sphereB;
    manifold.pointCount = 1;
    manifold.friction = CombineFriction(sphereA->getFriction(), sphereB->getFriction());
    manifold.restitution = CombineRestitution(sphereA->getRestitution(), sphereB->getRestitution());
    
    if (dist > 1e-6f) {
        manifold.normal = diff / dist;
    } else {
        // Coincident centers - pick arbitrary normal
        manifold.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    manifold.computeTangentBasis();
    
    ContactPoint& cp = manifold.points[0];
    cp.penetration = radiusSum - dist;
    cp.point = posA + manifold.normal * (radiusA - cp.penetration * 0.5f);
    cp.localPointA = sphereA->worldToLocal(cp.point);
    cp.localPointB = sphereB->worldToLocal(cp.point);
    AssignFeatureId(cp);
    
    return true;
}

bool SphereBox(const std::shared_ptr<RigidBody>& sphere,
               const std::shared_ptr<RigidBody>& box,
               ContactManifold& manifold) {
    glm::vec3 spherePos = sphere->getPosition();
    float radius = sphere->getRadius();
    
    glm::vec3 boxPos = box->getPosition();
    glm::mat3 boxRot = glm::mat3_cast(box->getOrientation());
    glm::vec3 halfExtents = box->getHalfExtents();
    
    // Find closest point on OBB to sphere center
    glm::vec3 closestPoint = ClosestPointOnOBB(spherePos, boxPos, boxRot, halfExtents);
    
    // diff points from sphere toward box (A to B direction)
    glm::vec3 diff = closestPoint - spherePos;
    float distSq = glm::dot(diff, diff);
    
    // Use detection threshold for resting contacts
    const float detectionThreshold = 0.005f;
    float radiusWithThreshold = radius + detectionThreshold;
    
    if (distSq >= radiusWithThreshold * radiusWithThreshold) {
        return false;  // No contact
    }
    
    float dist = std::sqrt(distSq);
    
    manifold.bodyA = sphere;
    manifold.bodyB = box;
    manifold.pointCount = 1;
    manifold.friction = CombineFriction(sphere->getFriction(), box->getFriction());
    manifold.restitution = CombineRestitution(sphere->getRestitution(), box->getRestitution());
    
    ContactPoint& cp = manifold.points[0];
    
    if (dist > 1e-6f) {
        // Normal penetration - sphere center outside box
        // Normal points from A (sphere) to B (box)
        manifold.normal = diff / dist;
        
        // Contact point is on the sphere surface, in the direction of the box
        cp.point = spherePos + manifold.normal * radius;
        
        // True penetration depth (positive when overlapping)
        cp.penetration = radius - dist;
    } else {
        // Deep penetration - sphere center inside box
        // Find closest face and project sphere center onto it
        glm::vec3 localPos = box->worldToLocal(spherePos);
        glm::vec3 absLocal = glm::abs(localPos);
        glm::vec3 penetrations = halfExtents - absLocal;
        
        // Find axis with minimum penetration (closest face)
        int minAxis = 0;
        float minPen = penetrations.x;
        if (penetrations.y < minPen) { minPen = penetrations.y; minAxis = 1; }
        if (penetrations.z < minPen) { minPen = penetrations.z; minAxis = 2; }
        
        // Normal points from sphere toward box face (A to B)
        // If sphere is at positive local X, normal should point toward +X face
        glm::vec3 localNormal(0.0f);
        localNormal[minAxis] = (localPos[minAxis] >= 0.0f) ? 1.0f : -1.0f;
        manifold.normal = box->localToWorldDirection(localNormal);
        
        // Contact point is on the box surface
        glm::vec3 localContactPoint = localPos;
        localContactPoint[minAxis] = halfExtents[minAxis] * 
                                      (localPos[minAxis] >= 0.0f ? 1.0f : -1.0f);
        cp.point = box->localToWorld(localContactPoint);
        
        // Penetration is distance from sphere surface to box face
        cp.penetration = minPen + radius;
    }
    
    manifold.computeTangentBasis();
    
    // Store local points correctly for warm starting
    // localPointA is contact point in sphere's local space
    // localPointB is contact point in box's local space
    cp.localPointA = sphere->worldToLocal(cp.point);
    cp.localPointB = box->worldToLocal(cp.point);
    AssignFeatureId(cp);
    
    return true;
}

bool SpherePlane(const std::shared_ptr<RigidBody>& sphere,
                 const std::shared_ptr<RigidBody>& plane,
                 ContactManifold& manifold) {
    glm::vec3 spherePos = sphere->getPosition();
    float radius = sphere->getRadius();
    
    glm::vec3 planeNormal = plane->getPlaneNormal();
    float planeDistance = plane->getPlaneDistance();
    
    // signedDist = distance from sphere center to plane (positive = above plane)
    float signedDist = SignedDistanceToPlane(spherePos, planeNormal, planeDistance);
    
    // Use small detection threshold but don't inflate penetration
    const float detectionThreshold = 0.005f;
    
    // Generate contact if sphere surface is at or below plane + threshold
    // Sphere surface is at signedDist - radius from the plane
    if (signedDist >= radius + detectionThreshold) {
        return false;  // No contact - sphere is too far above plane
    }
    
    manifold.bodyA = sphere;
    manifold.bodyB = plane;
    
    // Normal points from A (sphere) to B (plane) = -planeNormal (into the plane)
    manifold.normal = -planeNormal;
    manifold.computeTangentBasis();
    manifold.pointCount = 1;
    manifold.friction = CombineFriction(sphere->getFriction(), plane->getFriction());
    manifold.restitution = CombineRestitution(sphere->getRestitution(), plane->getRestitution());
    
    ContactPoint& cp = manifold.points[0];
    
    // Penetration is the actual distance the sphere surface is below the plane
    // If signedDist = radius, sphere is exactly touching -> penetration = 0
    // If signedDist < radius, sphere is penetrating -> penetration > 0
    cp.penetration = radius - signedDist;  // True penetration depth
    
    // Contact point is on the plane surface (closest point on plane to sphere center)
    cp.point = spherePos - planeNormal * signedDist;
    cp.localPointA = sphere->worldToLocal(cp.point);
    cp.localPointB = plane->worldToLocal(cp.point);
    AssignFeatureId(cp);
    
    return true;
}

// ============== BOX COLLISIONS ==============

bool BoxBox(const std::shared_ptr<RigidBody>& boxA,
            const std::shared_ptr<RigidBody>& boxB,
            ContactManifold& manifold) {
    glm::vec3 posA = boxA->getPosition();
    glm::vec3 posB = boxB->getPosition();
    glm::mat3 rotA = glm::mat3_cast(boxA->getOrientation());
    glm::mat3 rotB = glm::mat3_cast(boxB->getOrientation());
    glm::vec3 halfA = boxA->getHalfExtents();
    glm::vec3 halfB = boxB->getHalfExtents();
    
    // SAT test with 15 axes:
    // 3 face normals from A, 3 from B, 9 edge cross products
    
    glm::vec3 axes[15];
    int axisIndex = 0;
    
    // Face normals
    for (int i = 0; i < 3; i++) {
        axes[axisIndex++] = rotA[i];
        axes[axisIndex++] = rotB[i];
    }
    
    // Edge cross products
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            glm::vec3 cross = glm::cross(rotA[i], rotB[j]);
            float lenSq = glm::dot(cross, cross);
            if (lenSq > 1e-8f) {
                axes[axisIndex++] = cross / std::sqrt(lenSq);
            } else {
                axes[axisIndex++] = glm::vec3(0.0f); // Parallel edges - skip
            }
        }
    }
    
    float minPenetration = std::numeric_limits<float>::max();
    int minAxis = -1;
    glm::vec3 separatingNormal;
    
    glm::vec3 centerDiff = posB - posA;
    
    for (int i = 0; i < axisIndex; i++) {
        if (glm::dot(axes[i], axes[i]) < 1e-8f) continue;
        
        float minA, maxA, minB, maxB;
        ProjectBoxOntoAxis(posA, rotA, halfA, axes[i], minA, maxA);
        ProjectBoxOntoAxis(posB, rotB, halfB, axes[i], minB, maxB);
        
        // Check for separation
        if (maxA < minB || maxB < minA) {
            return false; // Separating axis found
        }
        
        // Calculate penetration
        float overlap = std::min(maxA, maxB) - std::max(minA, minB);
        if (overlap < minPenetration) {
            minPenetration = overlap;
            minAxis = i;
            separatingNormal = axes[i];
            
            // Ensure normal points from A to B
            if (glm::dot(separatingNormal, centerDiff) < 0.0f) {
                separatingNormal = -separatingNormal;
            }
        }
    }
    
    if (minAxis < 0) return false;
    
    // Setup manifold
    manifold.bodyA = boxA;
    manifold.bodyB = boxB;
    manifold.normal = separatingNormal;
    manifold.computeTangentBasis();
    manifold.friction = CombineFriction(boxA->getFriction(), boxB->getFriction());
    manifold.restitution = CombineRestitution(boxA->getRestitution(), boxB->getRestitution());
    
    // Generate contact points via clipping
    // Find reference and incident faces
    
    int referenceFace = -1;
    float maxDot = -1.0f;
    const glm::mat3* refRot = &rotA;
    const glm::vec3* refPos = &posA;
    const glm::vec3* refHalf = &halfA;
    const std::shared_ptr<RigidBody>* refBody = &boxA;
    const glm::mat3* incRot = &rotB;
    const glm::vec3* incPos = &posB;
    const glm::vec3* incHalf = &halfB;
    
    // Check which box face is most aligned with normal
    for (int i = 0; i < 3; i++) {
        float dot = std::abs(glm::dot(rotA[i], separatingNormal));
        if (dot > maxDot) {
            maxDot = dot;
            referenceFace = i;
            refRot = &rotA; refPos = &posA; refHalf = &halfA; refBody = &boxA;
            incRot = &rotB; incPos = &posB; incHalf = &halfB;
        }
        
        dot = std::abs(glm::dot(rotB[i], separatingNormal));
        if (dot > maxDot) {
            maxDot = dot;
            referenceFace = i;
            refRot = &rotB; refPos = &posB; refHalf = &halfB; refBody = &boxB;
            incRot = &rotA; incPos = &posA; incHalf = &halfA;
        }
    }
    
    // Get incident face (most anti-aligned with normal on incident box)
    int incidentFace = 0;
    float minDotInc = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; i++) {
        float dot = glm::dot((*incRot)[i], separatingNormal);
        if (dot < minDotInc) {
            minDotInc = dot;
            incidentFace = i;
        }
        if (-dot < minDotInc) {
            minDotInc = -dot;
            incidentFace = i;
        }
    }
    
    // Get incident face vertices
    auto incidentVerts = GetBoxFaceVertices(*incPos, *incRot, *incHalf, 
                                            (glm::dot((*incRot)[incidentFace], separatingNormal) < 0.0f) 
                                            ? incidentFace : incidentFace + 3);
    
    // Clip against reference face side planes
    std::vector<glm::vec3> clipped = incidentVerts;
    
    for (int i = 0; i < 3; i++) {
        if (i == referenceFace) continue;
        
        glm::vec3 planeNormal = (*refRot)[i];
        float planeOffset = glm::dot(planeNormal, *refPos) + (*refHalf)[i];
        clipped = ClipPolygonAgainstPlane(clipped, planeNormal, planeOffset);
        
        planeOffset = glm::dot(-planeNormal, *refPos) + (*refHalf)[i];
        clipped = ClipPolygonAgainstPlane(clipped, -planeNormal, planeOffset);
    }
    
    // Keep points below reference face
    glm::vec3 refFaceNormal = (*refRot)[referenceFace];
    if (glm::dot(refFaceNormal, separatingNormal) < 0.0f) {
        refFaceNormal = -refFaceNormal;
    }
    float refFaceOffset = glm::dot(refFaceNormal, *refPos) + (*refHalf)[referenceFace];
    
    manifold.pointCount = 0;
    for (const auto& vert : clipped) {
        float dist = glm::dot(vert, refFaceNormal) - refFaceOffset;
        if (dist < 0.0f && manifold.pointCount < ContactManifold::MAX_CONTACTS) {
            ContactPoint& cp = manifold.points[manifold.pointCount];
            cp.point = vert;
            cp.penetration = -dist;
            cp.localPointA = boxA->worldToLocal(vert);
            cp.localPointB = boxB->worldToLocal(vert);
            AssignFeatureId(cp);
            manifold.pointCount++;
        }
    }
    
    // If no points from clipping, use deepest penetrating corner
    if (manifold.pointCount == 0) {
        auto corners = GetBoxFaceVertices(*incPos, *incRot, *incHalf, incidentFace);
        float deepest = 0.0f;
        glm::vec3 deepestPoint = corners[0];
        
        for (const auto& corner : corners) {
            float dist = glm::dot(corner, refFaceNormal) - refFaceOffset;
            if (dist < deepest) {
                deepest = dist;
                deepestPoint = corner;
            }
        }
        
        ContactPoint& cp = manifold.points[0];
        cp.point = deepestPoint;
        cp.penetration = minPenetration;
        cp.localPointA = boxA->worldToLocal(deepestPoint);
        cp.localPointB = boxB->worldToLocal(deepestPoint);
        AssignFeatureId(cp);
        manifold.pointCount = 1;
    }
    manifold.stabilizePointOrder();
    
    return manifold.pointCount > 0;
}

bool BoxPlane(const std::shared_ptr<RigidBody>& box,
              const std::shared_ptr<RigidBody>& plane,
              ContactManifold& manifold) {
    glm::vec3 boxPos = box->getPosition();
    glm::mat3 boxRot = glm::mat3_cast(box->getOrientation());
    glm::vec3 halfExtents = box->getHalfExtents();
    
    glm::vec3 planeNormal = plane->getPlaneNormal();
    float planeDistance = plane->getPlaneDistance();
    
    // Get all 8 box corners
    glm::vec3 corners[8];
    int idx = 0;
    for (int x = -1; x <= 1; x += 2) {
        for (int y = -1; y <= 1; y += 2) {
            for (int z = -1; z <= 1; z += 2) {
                glm::vec3 local(x * halfExtents.x, y * halfExtents.y, z * halfExtents.z);
                corners[idx++] = boxPos + boxRot * local;
            }
        }
    }
    
    // Use detection threshold for contact generation
    // but DON'T add it to penetration depth
    // Positive dist = above plane, negative dist = below plane (penetrating)
    const float detectionThreshold = 0.005f;  // Detect contacts slightly before penetration
    
    manifold.pointCount = 0;
    manifold.bodyA = box;
    manifold.bodyB = plane;
    
    // Normal points from A (box) to B (plane) = into the plane = -planeNormal
    manifold.normal = -planeNormal;
    manifold.computeTangentBasis();
    manifold.friction = CombineFriction(box->getFriction(), plane->getFriction());
    manifold.restitution = CombineRestitution(box->getRestitution(), plane->getRestitution());
    
    for (int i = 0; i < 8 && manifold.pointCount < ContactManifold::MAX_CONTACTS; i++) {
        float dist = SignedDistanceToPlane(corners[i], planeNormal, planeDistance);
        
        // Generate contact if corner is within detection threshold of plane
        // dist < detectionThreshold means corner is at or below plane + threshold
        if (dist < detectionThreshold) {
            ContactPoint& cp = manifold.points[manifold.pointCount];
            
            // Contact point is the box corner
            cp.point = corners[i];
            
            // Penetration is the actual distance below the plane
            // Negative dist means below plane, so penetration = -dist
            // If dist is positive (above plane), penetration is 0 or negative
            cp.penetration = -dist;  // True penetration depth (positive when below plane)
            
            // For resting contacts (dist near 0), penetration near 0 is correct
            // This allows the box to rest exactly on the plane without being pushed up
            
            // Local points for warm starting
            cp.localPointA = box->worldToLocal(corners[i]);
            
            // Project corner onto plane surface for localPointB
            glm::vec3 planeContactPoint = corners[i] - planeNormal * dist;
            cp.localPointB = plane->worldToLocal(planeContactPoint);
            AssignFeatureId(cp);
            
            manifold.pointCount++;
        }
    }
    manifold.stabilizePointOrder();
    
    return manifold.pointCount > 0;
}

// ============== AABB TESTS ==============

bool AABBOverlap(const glm::vec3& minA, const glm::vec3& maxA,
                 const glm::vec3& minB, const glm::vec3& maxB) {
    return !(maxA.x < minB.x || minA.x > maxB.x ||
             maxA.y < minB.y || minA.y > maxB.y ||
             maxA.z < minB.z || minA.z > maxB.z);
}

bool PointInAABB(const glm::vec3& point,
                 const glm::vec3& min, const glm::vec3& max) {
    return point.x >= min.x && point.x <= max.x &&
           point.y >= min.y && point.y <= max.y &&
           point.z >= min.z && point.z <= max.z;
}

// ============== UTILITY ==============

glm::vec3 ClosestPointOnOBB(const glm::vec3& point,
                            const glm::vec3& boxCenter,
                            const glm::mat3& boxRotation,
                            const glm::vec3& halfExtents) {
    glm::vec3 d = point - boxCenter;
    glm::vec3 result = boxCenter;
    
    for (int i = 0; i < 3; i++) {
        float dist = glm::dot(d, boxRotation[i]);
        dist = glm::clamp(dist, -halfExtents[i], halfExtents[i]);
        result += boxRotation[i] * dist;
    }
    
    return result;
}

glm::vec3 ClosestPointOnPlane(const glm::vec3& point,
                              const glm::vec3& planeNormal,
                              float planeDistance) {
    float dist = glm::dot(point, planeNormal) - planeDistance;
    return point - planeNormal * dist;
}

float SignedDistanceToPlane(const glm::vec3& point,
                            const glm::vec3& planeNormal,
                            float planeDistance) {
    return glm::dot(point, planeNormal) - planeDistance;
}

std::vector<glm::vec3> ClipPolygonAgainstPlane(
    const std::vector<glm::vec3>& polygon,
    const glm::vec3& planeNormal,
    float planeOffset) {
    
    std::vector<glm::vec3> result;
    if (polygon.empty()) return result;
    
    for (size_t i = 0; i < polygon.size(); i++) {
        const glm::vec3& a = polygon[i];
        const glm::vec3& b = polygon[(i + 1) % polygon.size()];
        
        float distA = glm::dot(a, planeNormal) - planeOffset;
        float distB = glm::dot(b, planeNormal) - planeOffset;
        
        if (distA <= 0.0f) {
            result.push_back(a);
        }
        
        if ((distA > 0.0f) != (distB > 0.0f)) {
            float t = distA / (distA - distB);
            result.push_back(a + t * (b - a));
        }
    }
    
    return result;
}

std::vector<glm::vec3> GetBoxFaceVertices(
    const glm::vec3& center,
    const glm::mat3& rotation,
    const glm::vec3& halfExtents,
    int faceIndex) {
    
    std::vector<glm::vec3> vertices;
    vertices.reserve(4);
    
    // Face indices: 0-2 = +axis, 3-5 = -axis
    int axis = faceIndex % 3;
    float sign = (faceIndex < 3) ? 1.0f : -1.0f;
    
    int u = (axis + 1) % 3;
    int v = (axis + 2) % 3;
    
    glm::vec3 faceCenter = center + rotation[axis] * (sign * halfExtents[axis]);
    
    vertices.push_back(faceCenter - rotation[u] * halfExtents[u] - rotation[v] * halfExtents[v]);
    vertices.push_back(faceCenter + rotation[u] * halfExtents[u] - rotation[v] * halfExtents[v]);
    vertices.push_back(faceCenter + rotation[u] * halfExtents[u] + rotation[v] * halfExtents[v]);
    vertices.push_back(faceCenter - rotation[u] * halfExtents[u] + rotation[v] * halfExtents[v]);
    
    return vertices;
}

void ProjectBoxOntoAxis(const glm::vec3& center,
                       const glm::mat3& rotation,
                       const glm::vec3& halfExtents,
                       const glm::vec3& axis,
                       float& outMin, float& outMax) {
    float centerProj = glm::dot(center, axis);
    float radius = std::abs(glm::dot(rotation[0] * halfExtents.x, axis)) +
                   std::abs(glm::dot(rotation[1] * halfExtents.y, axis)) +
                   std::abs(glm::dot(rotation[2] * halfExtents.z, axis));
    
    outMin = centerProj - radius;
    outMax = centerProj + radius;
}

} // namespace PhysicsCollision

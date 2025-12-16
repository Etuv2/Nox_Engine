#include "Contact.h"
#include "RigidBody.h"
#include <algorithm>
#include <cmath>
#include <limits>

void ContactManifold::addPoint(const ContactPoint& newPoint) {
    // If we have room, just add it
    if (pointCount < MAX_CONTACTS) {
        points[pointCount] = newPoint;
        pointCount++;
        return;
    }
    
    // Manifold is full - find the point to replace
    // Strategy: Keep the deepest point and the 4 that form the largest contact area
    
    // Find the deepest existing point
    int deepestIndex = 0;
    float deepestPen = points[0].penetration;
    for (int i = 1; i < pointCount; i++) {
        if (points[i].penetration > deepestPen) {
            deepestPen = points[i].penetration;
            deepestIndex = i;
        }
    }
    
    // If new point is deepest, replace the shallowest existing point
    if (newPoint.penetration > deepestPen) {
        int shallowestIndex = 0;
        float shallowestPen = points[0].penetration;
        for (int i = 1; i < pointCount; i++) {
            if (i != deepestIndex && points[i].penetration < shallowestPen) {
                shallowestPen = points[i].penetration;
                shallowestIndex = i;
            }
        }
        points[shallowestIndex] = newPoint;
        return;
    }
    
    // Otherwise, find the point that contributes least to contact area
    // Use distance from existing points as heuristic
    float maxMinDist = 0.0f;
    int replaceIndex = 0;
    
    for (int i = 0; i < pointCount; i++) {
        if (i == deepestIndex) continue;
        
        float minDist = std::numeric_limits<float>::max();
        for (int j = 0; j < pointCount; j++) {
            if (i != j) {
                float d = glm::length(points[i].point - points[j].point);
                minDist = std::min(minDist, d);
            }
        }
        
        // Check distance to new point too
        float dNew = glm::length(points[i].point - newPoint.point);
        
        // Replace point that is closest to others (least unique)
        if (minDist < maxMinDist || replaceIndex == deepestIndex) {
            maxMinDist = minDist;
            replaceIndex = i;
        }
    }
    
    // Only replace if new point adds diversity
    float newMinDist = std::numeric_limits<float>::max();
    for (int i = 0; i < pointCount; i++) {
        float d = glm::length(points[i].point - newPoint.point);
        newMinDist = std::min(newMinDist, d);
    }
    
    if (newMinDist > maxMinDist) {
        points[replaceIndex] = newPoint;
    }
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
}

void ContactManifold::warmStart(float warmStartFactor) {
    if (!bodyA || !bodyB || warmStartFactor <= 0.0f) return;
    
    // Validate that bodies can receive impulses
    bool aCanReceive = bodyA->IsDynamic();
    bool bCanReceive = bodyB->IsDynamic();
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

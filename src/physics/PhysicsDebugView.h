#pragma once
// Physics debug overlay: view toggles (set by the editor's Physics window, not saved) and the world-space primitives
// they queue through DebugDraw. Core queues them in DrawDebugOverlay, i.e. only for the Scene viewport, right before
// DebugDraw::Draw, so they follow the same projection as script debug lines and are cleared with them each frame.
//
// Colours: static bodies grey, kinematic orange, dynamic green (awake) or blue (sleeping, with the sleeping toggle).
// Contacts are red points with yellow normals; AABBs translucent white; velocities cyan (0.1 s of motion); triggers
// magenta; characters white capsules.
#include <glm/glm.hpp>

class PhysicsEngine;

struct PhysicsDebugView {
	bool colliders = false;   // shapes of every body (sphere, oriented box, plane patch)
	bool aabbs = false;       // body AABBs (broadphase input)
	bool contacts = false;    // contact points and normals of the active manifolds
	bool sleeping = false;    // sleeping bodies tinted blue and marked with a dot
	bool triggers = false;    // trigger volumes
	bool characters = false;  // character controller capsules
	bool velocities = false;  // linear velocity of awake dynamic bodies

	bool Any() const { return colliders || aabbs || contacts || sleeping || triggers || characters || velocities; }
};

// Queues this frame's primitives for the enabled toggles. Does nothing when no toggle is on.
void QueuePhysicsDebugDraw(const PhysicsEngine& physics, const PhysicsDebugView& view);

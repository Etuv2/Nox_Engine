# Physics Completion Notes

## Current Status
- The physics engine still compiles and runs with the existing hybrid narrowphase.
- The remaining unfinished area is the full replacement of primitive-specific SAT/contact generation with one generic convex GJK/EPA manifold path.

## Verified Remaining Rewrite Targets
- `src/PhysicsCollision.cpp::TestCollision`
- `src/PhysicsCollision.cpp::SphereSphere`
- `src/PhysicsCollision.cpp::SphereBox`
- `src/PhysicsCollision.cpp::BoxBox`
- `src/PhysicsEngine.cpp::Narrowphase`
- `src/Contact.h` / `src/Contact.cpp` for manifold-level debug traces and cached-impulse transfer

## Intended End-State
- Non-plane convex pairs route through a single `BuildConvexManifold` path.
- GJK owns overlap/simplex evolution.
- EPA owns penetration depth, normal, and witness/contact generation.
- Cached impulses transfer by `featureId` when manifolds are refreshed.
- Debug output includes simplex evolution, EPA iterations, contact points, normal, depth, and solver state.

## Validation
- The repository build remains green after the renderer/material/TransformID work in this session.
- Full deterministic physics validation still needs to be run after the convex manifold rewrite lands.

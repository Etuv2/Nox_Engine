# Physics: components, queries and events

The physics world is the PhysicsEngine (`src/PhysicsEngine.*`). Scene nodes reach it through registry components
(`src/reflect/components/PhysicsComponents.cpp`), and scripts through `ctx:component`, `ctx:physics_raycast`, the
overlap and sweep queries and the physics events (`src/scripting/LuaPhysics.*`, docs/lua_api.md "Physics"). Nothing
here is game specific: a project decides what its nodes are.

## Components

All four are in the `Physics` category and are scene JSON components (`"components": { "Trigger": { ... } }`).

### RigidBody (dynamics of the node's body)

A node has at most one body. Adding `RigidBody` creates a dynamic unit-mass sphere; `Collider` shapes it.

| Property | Type | Notes |
|---|---|---|
| `body_type` | enum | `static`, `dynamic`, `kinematic` |
| `mass` | float | 0 to 100000 |
| `linear_velocity`, `angular_velocity` | vec3 | runtime (not saved) |
| `gravity_scale`, `linear_damping`, `angular_damping`, `friction`, `restitution` | float | ranges in the inspector |
| `sleeping` | bool | runtime (not saved) |

Methods: `apply_force(v)`, `apply_impulse(v)`, `apply_torque(v)`, `wake()`, `sleep()`.

### Collider (shape and layers of the body)

| Property | Type | Notes |
|---|---|---|
| `shape` | enum | `sphere`, `box`, `plane`, `capsule`, `mesh` |
| `radius` | float | sphere and capsule |
| `height` | float | capsule: total height, feet cap to head cap (at least `2 * radius`) |
| `capsule_axis` | enum | capsule: `x`, `y` or `z` (local axis the capsule runs along; default `y`) |
| `half_extents` | vec3 | box, positive |
| `plane_normal`, `plane_distance` | vec3, float | plane |
| `mesh_path` | asset path | mesh: a `.glb` or `.gltf` whose geometry collides (see "Mesh colliders") |
| `layer`, `mask` | int | collision layer and the layers this body collides with |

Properties that load geometry or dimensions (`height`, `capsule_axis`, `mesh_path`) keep the shape type they find;
`shape` is what switches it. Scene JSON applies keys in alphabetical order, so this order does not matter.

### Mesh colliders

A `mesh` collider is static triangle geometry: the body's position and orientation place the mesh, and the mesh is
loaded once per path and shared (`src/physics/MeshCollider.*`). Nodes named `NOCOLLIDE` in the file are skipped.
Mesh bodies are always static. A mesh body keeps the transform it was given: static bodies are not re-tested each
step, so a mesh that moves is not supported. Uniform or non-uniform scale is not applied
to the mesh (the body transform is rigid); bake the scale into the file.

Dynamic triangle meshes are not supported. Box bodies against meshes produce no contact (see Known limits).

### Trigger (volume that reports overlaps, does not collide)

A trigger can sit on any node, with or without a body. Each physics frame the engine finds the bodies that overlap
its volume (transform: the node's world position and rotation; scale is not applied) and reports the ones that
entered or left since the last frame.

| Property | Type | Notes |
|---|---|---|
| `shape` | enum | `sphere` or `box` |
| `radius` | float | sphere radius, 0.01 to 10000 |
| `half_extents` | vec3 | box half extents, positive |
| `layer` | int | this trigger's layer; a body is reported only when its `mask` includes it |
| `mask` | int | a body is reported only when its `layer` intersects this mask |
| `enabled` | bool | a disabled trigger reports exits for everything it held |

Filtering is symmetric with collision: a body is reported when `body.layer & trigger.mask` and
`body.mask & trigger.layer` are both non-zero. A trigger never reports its own node's body, and triggers do not
report each other.

### CharacterController (kinematic capsule)

A vertical capsule on the node, moved explicitly with `move(displacement)`. It is not a body: it does not push
dynamic bodies, and it collides with the bodies of the physics world (static, kinematic, dynamic, planes and mesh
colliders) and with the world geometry set with `PhysicsEngine::SetWorldCollision`. The capsule is sampled with three
spheres (feet, middle, head). A move is split into substeps no longer than half the radius; after each substep the
capsule is pushed out of what it touches (up to four passes), which slides it along walls and floors.

| Property | Type | Notes |
|---|---|---|
| `height` | float | feet to head, 0.1 to 100 |
| `radius` | float | 0.01 to 10 |
| `step_offset` | float | highest ledge climbed while walking |
| `slope_limit` | float, degrees | steepest walkable surface (0 to 89) |
| `gravity` | bool | when on, the engine integrates `velocity.y` every frame and moves the capsule vertically |
| `velocity` | vec3 | runtime (not saved). Set `velocity.y` to jump |
| `is_on_floor` | bool | read-only runtime: the last move ended on a walkable surface |

Method: `move(displacement)` returns true when the capsule touched something. Horizontal motion is driven by the
script (`move`); gravity only affects the vertical component.

Movement rules:

- A surface steeper than `slope_limit` is a wall. The capsule is pushed out horizontally only, so it cannot climb it.
- A move that is blocked (it gains less than half of what was asked) retries one step higher. The retry moves
  forward at least one radius, so the rounded foot clears a ledge, then drops back down. A walk up a slope is not
  blocked and never takes this path.
- A move that leaves the floor (a step down, or a slope falling away) snaps back to a floor within `step_offset`
  below. A character with upward velocity (a jump) is not snapped.
- Feet sit one radius off a slope along its normal, about 0.06 m above it at 30 degrees, because the capsule is
  sampled with spheres.

## Queries (C++)

`PhysicsEngine` (`src/PhysicsEngine.h`):

- `RayCastClosest(origin, dir, max_dist, mask, hit)`: nearest body hit with its distance, point and normal. Sphere,
  box, plane, capsule and mesh are tested; `dir` need not be normalized. Mesh hits use the triangle normal, facing
  the ray.
- `OverlapSphere(center, radius, mask)` and `OverlapBox(center, half_extents, rotation, mask)`: body ids whose shape
  overlaps the probe, touching included.
- `GetEntityForBody(id)` and `GetBodyForEntity(entity)`: body and entity lookup through the attached node.
- `RefreshBodyBounds(body)`: recomputes a body's bounds and moves its broadphase proxy. Call it after changing a
  body's shape, mesh or position outside the simulation (the scene loader does this through the descriptors).
- `SetWorldCollision(const CollisionWorld*)`: level geometry the character controller collides with, in addition to the
  bodies. Not owned: the collision world must outlive the setting. C++ only; no scene or Lua hook yet.
- `QueryAABB` and `RayCast` (callback form) are unchanged.

Sweeps (`src/physics/ShapeSweep.*`): `ShapeSweep::Sweep(physics, probe, dir, max_dist, mask, hit, ignore)` moves a
sphere, capsule or box probe along `dir` and reports the first body it touches: `distance`, `point`, `normal` (pointing
back at the probe) and `body`. Only bodies whose collision layer intersects `mask` count. Method: the probe is tested
every half radius, then the first overlap is bisected (24 steps). A box probe does not hit meshes (see Known limits).

Overlap and character tests use `src/physics/ShapeOverlap.*`, an exact sphere/box/plane test without the GJK
prepass that the solver uses. A capsule is tested by its sample spheres; a mesh by its triangles.

## Events

Events are raised per physics step and delivered to Lua by the next Lua update.

- Trigger enter and exit: `TriggerSystem::TakeEvents()`. Lua names: `trigger_enter`, `trigger_exit`.
- Contact begin and end: `PhysicsEngine::TakeContactEvents()`, produced by the solver's contact pairs. A pair is
  begin when it was not touching in the previous step and end when it stops. Lua names: `contact_begin`,
  `contact_end`.

Payloads and handler signatures are in docs/lua_api.md "Physics".

## Lua

- `ctx:physics_raycast(origin, dir, max_dist, mask)`: nearest body (sphere, box, plane, capsule, mesh).
- `ctx:overlap_sphere(center, radius, mask)`, `ctx:overlap_box(center, half_extents, mask)`.
- `ctx:physics_sweep_sphere(center, radius, dir, max_dist, mask)`, `ctx:physics_sweep_capsule(a, b, radius, dir,
  max_dist, mask)` (`a`, `b` are the segment ends), `ctx:physics_sweep_box(center, half_extents, dir, max_dist,
  mask)`. Each returns `{ entity, distance, point, normal }` or nil.

## Known limits

- Box bodies do not touch meshes: `box` against `mesh` gives no contact (sphere and capsule probes work). Box sweeps do
  not hit meshes for the same reason.
- Capsule against capsule, capsule against box and sweeps are approximated by sample spheres along the axis. Capsule
  contacts use a sample spacing of one radius, which is exact for spheres and planes and close for walls and edges.
- Mesh contacts give one normal per manifold (the solver's limit): contacts whose normals differ from the deepest by
  more than about 45 degrees are dropped.
- Dynamic meshes and mesh colliders on moving nodes are not supported. A static mesh's bounds are fixed until
  `RefreshBodyBounds` is called. `PreStepSync` does not refresh static bodies whose nodes move.
- There is no automatic mesh collider for scene geometry (a "static collision" flag or a parent component). Give each
  collidable node `shape: mesh` and its `mesh_path`.
- The character controller takes world geometry from `SetWorldCollision` (C++); a scene or Lua hook is not wired.
- Mesh paths are used as given (repo-relative in the scenes), not resolved through the asset packs.

## Performance

Measured on `projects/character-demo/models/level.glb` (76 triangles) with one character walking on it for 600 frames
at the fixed 60 Hz step, Release build: average physics step 0.0033 ms, maximum 0.062 ms (no mesh: 0.0012 ms average).
The mesh query is per sample sphere against the triangles of the broadphase candidates; a typical level stays well
under 1 ms per step.

## Where the code lives

| Piece | File |
|---|---|
| Engine queries, contact events, update hooks, RefreshBodyBounds | `src/PhysicsEngine.h`, `src/PhysicsEngine.cpp` |
| Trigger volumes | `src/physics/TriggerSystem.h`, `src/physics/TriggerSystem.cpp` |
| Character controller | `src/physics/CharacterSystem.h`, `src/physics/CharacterSystem.cpp` |
| Exact shape overlap | `src/physics/ShapeOverlap.h`, `src/physics/ShapeOverlap.cpp` |
| Mesh colliders (cache, sphere contacts) | `src/physics/MeshCollider.h`, `src/physics/MeshCollider.cpp` |
| Shape sweeps | `src/physics/ShapeSweep.h`, `src/physics/ShapeSweep.cpp` |
| Triangle geometry (BVH, sphere contacts, ray) | `src/CollisionWorld.h`, `src/CollisionWorld.cpp` |
| Contact pairs for capsules and meshes | `src/PhysicsCollision.cpp` (`TestCompound`) |
| Descriptors | `src/reflect/components/PhysicsComponents.cpp` |
| Lua queries, sweeps and event delivery | `src/scripting/LuaPhysics.h`, `src/scripting/LuaPhysics.cpp` (pumped from `LuaWorld::Update`) |
| Debug overlay | `src/physics/PhysicsDebugView.cpp` |
| Demo level generator | `tools/make_character_level_glb.py` |
| Tests | `tests/native/test_physics_components.cpp`, `tests/native/test_physics_shapes.cpp` |
| Demo scene | `scenes/test_character_level.json` (project `projects/character-demo`) |

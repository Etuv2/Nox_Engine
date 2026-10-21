# Physics: components, queries and events

The physics world is the PhysicsEngine (`src/PhysicsEngine.*`). Scene nodes reach it through registry components
(`src/reflect/components/PhysicsComponents.cpp`), and scripts through `ctx:component`, `ctx:physics_raycast`, the
overlap and sweep queries and the physics events (`src/scripting/LuaPhysics.*`, docs/lua_api.md "Physics"). Nothing
here is game specific: a project decides what its nodes are.

## Components

All five are in the `Physics` category and are scene JSON components (`"components": { "Trigger": { ... } }`).

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
Mesh bodies are always static, but they follow their node: when the node moves (gizmo, Lua, animation) the body's
transform and broadphase bounds are updated every physics update, and the body's platform velocity (its motion over
the last frame) is recorded for characters standing on it (see "Moving platforms").

A mesh collider's own node scale is not applied by the scene loader (the body transform is rigid): bake the scale into
the file. `StaticCollision` bakes the node's world scale into the geometry instead (see below).

Dynamic triangle meshes are not supported. Box bodies against meshes produce box/triangle contacts (see "Box and mesh").

### Trigger (volume that reports overlaps, does not collide)

A trigger can sit on any node, with or without a body. Each physics frame the engine finds the bodies and character
controllers that overlap its volume (transform: the node's world position and rotation; scale is not applied) and
reports the ones that entered or left since the last frame. A character controller is tested as its capsule (see
"CharacterController").

| Property | Type | Notes |
|---|---|---|
| `shape` | enum | `sphere` or `box` |
| `radius` | float | sphere radius, 0.01 to 10000 |
| `half_extents` | vec3 | box half extents, positive |
| `layer` | int | this trigger's layer; a body is reported only when its `mask` includes it |
| `mask` | int | a body is reported only when its `layer` intersects this mask |
| `enabled` | bool | a disabled trigger reports exits for everything it held |

Filtering is symmetric with collision: a body is reported when `body.layer & trigger.mask` and
`body.mask & trigger.layer` are both non-zero. The same rule applies to character controllers, with the controller's
`layer` and `mask`. A trigger never reports its own node's body, and triggers do not report each other.

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
| `layer`, `mask` | int | the controller's trigger filter (see Trigger) |
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
- A character standing on a moving body is carried by that body: before gravity each physics update the character
  moves by the body's platform velocity times the frame time.

Contacts: each `move()` reports one `controller_hit` per touched body (or level geometry) to Lua, with the normal that
pushes the controller out (see docs/lua_api.md "Physics").

### StaticCollision (collidable models without a collider on each one)

A `StaticCollision` component on a node makes the nodes in its subtree collidable. For every node that has a model
(`MeshRenderer`) and no `RigidBody` of its own, the engine keeps one static mesh body at that node's world transform,
whose geometry is the model's file, loaded once per file and shared (`src/physics/StaticCollision.*`). The node's world
scale is baked into the geometry. Nodes with their own collider are left alone.

| Property | Type | Notes |
|---|---|---|
| `include_children` | bool | true (default): every model in the subtree; false: only the node's own model |
| `layer`, `mask` | int | collision layer and mask of the generated bodies |

Rules:

- The generated bodies belong to the system, not to the nodes. They are not a node's `RigidBody`, so they are not
  written to the scene and do not appear in the Collider inspector. Scene JSON stores only the component.
- They follow the scene: a moved parent moves its level, and a moved or removed child is picked up on the next sync.
  The system resynchronises after a component edit, after a scene load, and at least every 0.25 s. A change to a
  node's scale is applied at that sync.
- Editor: add or remove the component in the inspector, or use Node > "Add collision to selection (recursive)" (one
  undo step for the selection).
- Nothing applies to racing scenes: the racing game uses its own vehicle physics, so do not add `StaticCollision` there.

## Moving platforms

A static mesh body whose node moves (a model with a `StaticCollision`, or a node with a mesh collider) is moved every
physics update, and the body keeps the motion it had over the last frame as its platform velocity. A character standing
on that body is carried by it (see CharacterController). Stopping the node stops the carry. The velocity is a finite
difference of the node's positions, so a platform that jitters carries jittery.

The template `templates/third_person_3d` has a platform that slides along X (`scripts/platform.lua`).

## Box and mesh

A box body against a mesh body produces contacts from the separating-axis test of the box against each triangle
(`CollisionWorld::BoxContacts`, `MeshCollider::BoxContacts`): the 13 candidate axes give the least-overlap normal, which
points from the triangle toward the box. When the triangle's face normal is within 1 cm of the least overlap it is
used, so a box across a triangle split does not tilt and bounce. Contacts, one normal per manifold as for capsules, and
box sweeps (`ShapeSweep`, `ShapeOverlap`) use the same test: a box sweep lands on a mesh floor and stops at a mesh wall.

## Queries (C++)

`PhysicsEngine` (`src/PhysicsEngine.h`):

- `RayCastClosest(origin, dir, max_dist, mask, hit)`: nearest body hit with its distance, point and normal. Sphere,
  box, plane, capsule and mesh are tested; `dir` need not be normalized. Mesh hits use the triangle normal, facing
  the ray.
- `OverlapSphere(center, radius, mask)` and `OverlapBox(center, half_extents, rotation, mask)`: body ids whose shape
  overlaps the probe, touching included.
- `GetEntityForBody(id)` and `GetBodyForEntity(entity)`: body and entity lookup through the attached node.
- `RefreshBodyBounds(body)`: recomputes a body's bounds and moves its broadphase proxy. Call it after changing a
  body's shape, mesh or position outside the simulation (the scene loader does this through the descriptors, and the
  mesh sync does it for moved nodes).
- `SetWorldCollision(const CollisionWorld*)`: level geometry the character controller collides with, in addition to the
  bodies. Not owned: the collision world must outlive the setting. C++ only; no scene or Lua hook yet.
- `StaticCollisions()`, `Triggers()`, `Characters()`: the component state (`src/physics/*.h`).
- `QueryAABB` and `RayCast` (callback form) are unchanged.

Sweeps (`src/physics/ShapeSweep.*`): `ShapeSweep::Sweep(physics, probe, dir, max_dist, mask, hit, ignore)` moves a
sphere, capsule or box probe along `dir` and reports the first body it touches: `distance`, `point`, `normal` (pointing
back at the probe) and `body`. Only bodies whose collision layer intersects `mask` count. Method: the probe is tested
every half radius, then the first overlap is bisected (24 steps).

Overlap and character tests use `src/physics/ShapeOverlap.*`, an exact sphere/box/plane test without the GJK
prepass that the solver uses. A capsule is tested by its sample spheres; a mesh by its triangles, and a box against a
mesh by its box/triangle contacts.

## Events

Events are raised per physics step and delivered to Lua by the next Lua update.

- Trigger enter and exit: `TriggerSystem::TakeEvents()`. Lua names: `trigger_enter`, `trigger_exit`.
- Controller hits: `CharacterSystem::TakeHits()`, filled by `move()`. Lua name: `controller_hit`.
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

- Capsule against box, and capsule against capsule, are approximated by sample spheres along the axis (exact
  capsule-box is not implemented). Capsule contacts use a sample spacing of one radius, which is exact for spheres and
  planes and close for walls and edges.
- Box against mesh uses one contact normal per manifold: contacts whose normals differ from the deepest by more than
  about 45 degrees are dropped. A box resting across a mesh edge settles on the face normal (see "Box and mesh").
- Dynamic meshes are not supported. A mesh body's bounds are updated when its node moves, not between physics updates.
- Mesh bodies bake scale only through `StaticCollision`; a mesh collider in a scene file ignores its node's scale.
- Controller hits are reported per `move()` call: a character pressed against a wall reports the wall on every frame it
  moves into it.
- The character controller takes world geometry from `SetWorldCollision` (C++); a scene or Lua hook is not wired.
- Mesh paths are used as given (repo-relative in the scenes), not resolved through the asset packs.
- A mesh collider on a child node now follows the world transform of the node (the scene loader used to place it at the
  node's local position and never move it).

## Performance

Measured in a Release build, headless (`NOX_PHYSICS_BENCH=1 nox_tests.exe --filter PhysicsBench`; see
`tests/native/test_physics_bench.cpp`). The step time is `PhysicsEngine::Update`, averaged over a 600-frame walk of one
character at the fixed 60 Hz step, with the level's meshes under one `StaticCollision` component:

| Level | Meshes | Triangles | Load (parse + BVH, per file) | First sync (bodies) | Step avg | Step max |
|---|---|---|---|---|---|---|
| `projects/character-demo/models/level.glb` (`scenes/test_character_level.json`) | 1 | 76 | 5.5 ms | 0.07 ms | 0.0029 ms | 0.0086 ms |
| `models/shuto/rep_section/collision/*.glb` (shuto rep section) | 54 | 234 439 | 159 ms | 0.19 ms | 0.031 ms | 0.23 ms |

The loads are cached per file and scale, so they are paid once per level. The character walked 32.7 m of 36 m on the
demo level (the rest is blocked by its walls) and 36 m on the shuto level, on the floor in every frame. The step
includes the periodic `StaticCollision` resync, which walks the subtree every 0.25 s. The older figure for the demo
level (0.0033 ms average, 0.062 ms maximum) came from a different method (the game loop, the level as one hand-made
mesh body), so it is not a like-for-like baseline.

The mesh query is per sample sphere against the triangles of the broadphase candidates; a level of a few hundred
thousand triangles stays well under 1 ms per step with one character.

## Where the code lives

| Piece | File |
|---|---|
| Engine queries, contact events, update hooks, RefreshBodyBounds, moving mesh sync | `src/PhysicsEngine.h`, `src/PhysicsEngine.cpp` |
| Trigger volumes | `src/physics/TriggerSystem.h`, `src/physics/TriggerSystem.cpp` |
| Character controller | `src/physics/CharacterSystem.h`, `src/physics/CharacterSystem.cpp` |
| StaticCollision component state and body sync | `src/physics/StaticCollision.h`, `src/physics/StaticCollision.cpp` |
| Exact shape overlap | `src/physics/ShapeOverlap.h`, `src/physics/ShapeOverlap.cpp` |
| Mesh colliders (cache, sphere and box contacts) | `src/physics/MeshCollider.h`, `src/physics/MeshCollider.cpp` |
| Shape sweeps | `src/physics/ShapeSweep.h`, `src/physics/ShapeSweep.cpp` |
| Triangle geometry (BVH, sphere and box contacts, ray) | `src/CollisionWorld.h`, `src/CollisionWorld.cpp` |
| Contact pairs for capsules, meshes and box/mesh | `src/PhysicsCollision.cpp` (`TestCompound`) |
| Descriptors | `src/reflect/components/PhysicsComponents.cpp` |
| Lua queries, sweeps and event delivery | `src/scripting/LuaPhysics.h`, `src/scripting/LuaPhysics.cpp` (pumped from `LuaWorld::Update`) |
| Node menu action | `src/ImGui/EditorWorkspaceSelection.cpp` (`AddStaticCollisionToSelection`) |
| Debug overlay | `src/physics/PhysicsDebugView.cpp` |
| Demo level generator | `tools/make_character_level_glb.py` |
| Tests | `tests/native/test_physics_components.cpp`, `tests/native/test_physics_shapes.cpp`, `tests/native/test_physics_authoring.cpp`, `tests/native/test_physics_bench.cpp` |
| Demo scene | `scenes/test_character_level.json` (project `projects/character-demo`) |
| Template level (StaticCollision, triggers, moving platform) | `templates/third_person_3d/project` |

# Physics: components, queries and events

The physics world is the PhysicsEngine (`src/PhysicsEngine.*`). Scene nodes reach it through registry components
(`src/reflect/components/PhysicsComponents.cpp`), and scripts through `ctx:component`, `ctx:physics_raycast`, the
overlap queries and the physics events (`src/scripting/LuaPhysics.*`, docs/lua_api.md "Physics"). Nothing here is
game specific: a project decides what its nodes are.

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
| `shape` | enum | `sphere`, `box`, `plane` |
| `radius` | float | sphere |
| `half_extents` | vec3 | box, positive |
| `plane_normal`, `plane_distance` | vec3, float | plane |
| `layer`, `mask` | int | collision layer and the layers this body collides with |

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
dynamic bodies, and it collides with the bodies of the physics world (static, kinematic, dynamic and planes). The
capsule is sampled with three spheres (feet, middle, head). A move is split into substeps no longer than half the
radius; after each substep the capsule is pushed out of what it touches (up to four passes), which slides it along
walls and floors. While walking from the floor it can climb a ledge no higher than `step_offset`.

| Property | Type | Notes |
|---|---|---|
| `height` | float | feet to head, 0.1 to 100 |
| `radius` | float | 0.01 to 10 |
| `step_offset` | float | highest ledge climbed while walking |
| `slope_limit` | float, degrees | steepest walkable surface (0 to 89) |
| `gravity` | bool | when on, the engine integrates `velocity.y` every frame and moves the capsule vertically |
| `velocity` | vec3 | runtime (not saved). Set `velocity.y` to jump |
| `is_on_floor` | bool | read-only runtime: the last move ended on a walkable surface |

Method: `move(displacement)` returns true when the capsule touched something. Horizontal motion is always driven
by the script (`move`); gravity only affects the vertical component.

Limits: the capsule uses the PhysicsEngine bodies only. It does not collide with the vehicle CollisionWorld meshes.
Its own node's body, when it has one, is ignored.

## Queries (C++)

`PhysicsEngine` (`src/PhysicsEngine.h`):

- `RayCastClosest(origin, dir, max_dist, mask, hit)`: nearest body hit with its distance, point and normal. Sphere,
  box and plane are tested exactly; `dir` need not be normalized.
- `OverlapSphere(center, radius, mask)` and `OverlapBox(center, half_extents, rotation, mask)`: body ids whose shape
  overlaps the probe, touching included.
- `GetEntityForBody(id)` and `GetBodyForEntity(entity)`: body and entity lookup through the attached node.
- `QueryAABB` and `RayCast` (callback form) are unchanged.

Overlap and character tests use `src/physics/ShapeOverlap.*`, an exact sphere/box/plane test without the GJK
prepass that the solver uses (see Known limits).

## Events

Events are raised per physics step and delivered to Lua by the next Lua update.

- Trigger enter and exit: `TriggerSystem::TakeEvents()`. Lua names: `trigger_enter`, `trigger_exit`.
- Contact begin and end: `PhysicsEngine::TakeContactEvents()`, produced by the solver's contact pairs. A pair is
  begin when it was not touching in the previous step and end when it stops. Lua names: `contact_begin`,
  `contact_end`.

Payloads and handler signatures are in docs/lua_api.md "Physics".

## Known limits

- Trigger and character results are exact for sphere, box and plane shapes.
- Contact events come from the solver's contact pairs. The solver tests sphere-box and box-box pairs through a GJK
  prepass (`src/PhysicsCollision.cpp`). With a box floor a falling sphere produced no contact events in the tests
  written for this change, so box contacts need a check before scripts rely on them; plane floors produce events.
- The character controller is not wired to CollisionWorld meshes; use bodies for walkable geometry.

## Where the code lives

| Piece | File |
|---|---|
| Engine queries, contact events, update hooks | `src/PhysicsEngine.h`, `src/PhysicsEngine.cpp` |
| Trigger volumes | `src/physics/TriggerSystem.h`, `src/physics/TriggerSystem.cpp` |
| Character controller | `src/physics/CharacterSystem.h`, `src/physics/CharacterSystem.cpp` |
| Exact shape overlap | `src/physics/ShapeOverlap.h`, `src/physics/ShapeOverlap.cpp` |
| Descriptors | `src/reflect/components/PhysicsComponents.cpp` |
| Lua queries and event delivery | `src/scripting/LuaPhysics.h`, `src/scripting/LuaPhysics.cpp` (pumped from `LuaWorld::Update`) |
| Tests | `tests/native/test_physics_components.cpp` |

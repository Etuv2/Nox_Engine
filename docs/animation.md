# Animation: pose blending, crossfades and the Animator state machine

Skinned models play their glTF clips through the `Animator` component (`AnimationComponent` in
`src/ComponentTypes.h`, driven by `AnimationSystem`). This page covers blending, crossfades, the parameter
state machine, marker events and their Lua API. Lua reference: `docs/lua_api.md`, "Animator".

## Pose evaluation

- A pose is one local TRS per scene node (`NodePose`, `src/AnimationPose.h`). Node transforms without channels keep
  their bind pose. A clip sample writes the nodes it targets at a clip time.
- Blending two poses: translation and scale are lerped; rotation is slerped on the shortest arc (one quaternion is
  negated when the two lie in opposite hemispheres) and falls back to nlerp when they are nearly parallel.
  Matrices are never blended directly.
- Skinning: `RenderSystem::UploadBoneMatrices` builds the joint matrices once per skinned entity per render pass.
  When an entity has an animator with a pose, the node transforms come from that pose; otherwise the legacy clip
  path is used unchanged.
- Skinned culling: at load, each joint keeps the bind-space box of the vertices it influences
  (`Scene::Skin::bounds`). Each frame those boxes are moved by the joint matrices and their union gives the
  item's bounds (`SkinBounds.h`), so skinned items are frustum culled like static ones.
- Skin weights: influences are merged to the four largest and normalised at load (`SkinBounds::MergeInfluences`).
  Known asset issue: `realistic_shark.glb` has inverse-bind matrices that do not match its node hierarchy (the
  joint bind products are about 1.8x identity, so the bind pose does not reproduce the mesh). In the circling clip
  the tail-tip joints swing several units away, and the stretched triangles seen in animated poses appear to come
  from that. Not fixed in the engine; the asset likely needs re-exporting.

## Crossfades

- `AnimatorRuntime::PlayClip(scene, clip, loop, blendTime)` starts a clip as a single-clip state. With a positive
  blend time it fades from the pose on screen (or from the legacy clip's current pose) over `blendTime` seconds.
- A state transition with `duration` > 0 fades the same way. `ease` is `linear` (default) or `smoothstep`.
- Interrupting a fade starts the new fade from the pose currently on screen, so the first frame after an interrupt
  does not jump.
- `AnimationSystem::BlendToAnimation` crossfades through the animator. `PlayAnimation` and the Lua `play` method are
  cuts.

## The state machine graph

The graph is JSON text in the Animator `graph` property, so it is saved with the scene. The inspector shows it as
text. It is parsed on the first update after the model is loaded, and clip names resolve against that model.

```json
{
  "params": { "speed": "float", "circling": "bool", "jump": "trigger" },
  "default": "swim",
  "states": [
    { "name": "swim",
      "blend": { "param": "speed", "points": [
        { "clip": "Shark Rig|circling_skeletal.3", "at": 0.0 },
        { "clip": "Shark Rig|swimming_skeletal.3", "at": 1.0 } ] },
      "speed": 1.0, "loop": true,
      "events": [ { "name": "splash", "t": 0.5 } ] },
    { "name": "circle", "clip": "Shark Rig|circling_skeletal.3", "speed": 1.0, "loop": true }
  ],
  "transitions": [
    { "from": "swim", "to": "circle", "duration": 0.6, "ease": "smoothstep",
      "when": [ { "param": "circling", "value": true } ] },
    { "from": "circle", "to": "swim", "duration": 0.6, "ease": "smoothstep",
      "when": [ { "param": "circling", "value": false } ] }
  ]
}
```

- **params**: `float`, `bool` or `trigger`. Names are unique.
- **states**: each has a `name` and either a `clip` (clip name in the model) or a `blend` (1D blend space over one
  float parameter; `points` are sorted by `at`). `speed` multiplies the playback rate (default 1); `loop` defaults to
  true. Missing or unknown clips reject the whole graph; `AnimatorRuntime::LastError` says why.
- **transitions**: `from` is a state name or `"*"` (any state), `to` a state name. `duration` is the crossfade in
  seconds, `exit_time` (optional, normalised 0 to 1) is the playhead the source state must reach first, and `when`
  lists conditions that must all hold. Float conditions use `op` (`>`, `<`, `>=`, `<=`, `==`, `!=`) and `value`. Bool
  conditions take `value` (default true). A trigger condition is satisfied while the trigger is set.
- **default**: the state entered when the graph starts (default: the first state).

### Evaluation rules

- Each update advances the playhead of the current state by `dt * speed / duration`, evaluates the transitions in
  file order and takes the first one that holds. **At most one transition fires per update**, so a jump across two
  edges takes two updates.
- A trigger is consumed by the transition that uses it. A trigger that no transition has used yet stays set.
- Blend spaces are phase-synchronised: every clip in a blend samples the same normalised time, and the playhead
  advances at the blend's weighted duration. Feet keep their cycle across the blend.
- Marker events fire when the playhead crosses `t` (normalised 0 to 1 of the clip). A looping state fires each marker
  once per cycle. A marker at `t` = 0 fires at the wrap, not when the state is entered.
- Entering a state restarts its playhead at 0, so a state that is left and re-entered replays from the start. A
  marker late in the clip may never fire if the state is always left before reaching it. The demo puts its splash
  marker at 0.05 for this reason.

## Events for Lua

Crossing a marker queues an `anim.event` for the Lua world. Payload: `{ entity = handle, name = marker name,
clip = clip name, phase = marker time }`. See `docs/lua_api.md`, "Animator".

## Scene and reflection

- Animator properties: `clip`, `speed`, `loop`, `graph` (saved), `state`, `playing`, `time`, `clip_count` (the last
  four are runtime state, not saved).
- Methods: `play(name)`, `play_index(i)`, `stop()`, `pause()`, `resume()`, `set_morph_weight(i, w)`, plus
  `set_float(name, v)`, `set_bool(name, b)`, `set_trigger(name)` and `current_state()` for the graph.
- The `graph` property is applied when the entity exists. `SceneLoader` retries components that cannot be added
  before the entity exists (`DeserializeComponents` with `deferred`, then `DeserializeComponentsNamed`).
- A `set_*` call on a node without a graph, or with a name of the wrong type, fails and raises a Lua error.

## Limits (not done)

- No additive layers, bone masks, inverse kinematics or root motion. Root motion would have to come from the scene
  graph, not the clip.
- Non-skinned models with a graph drive only the primary clip's node transform.
- `AnimationController` (`AnimationComponent::controller`) is an older path that no scene or editor code creates.
  Its `CrossFadeAnimations` still only sets weights. Crossfades run through `AnimatorRuntime` instead.
- The graph is edited as JSON text in the inspector. There is no structured state or transition editor.

## Measurements

Scenes: `scenes/animation_perf_legacy.json` (20 sharks, each playing one clip through the legacy path) and
`scenes/animation_perf_graph.json` (the same 20 sharks with the state machine). Time is the animation stage
(`AnimationSystem::Update` through `UpdateAnimations`), averaged over 900 frames at fixed 1/60 s, measured on the
same build. Four alternating pairs, with other workers sharing the machine:

| Run | Legacy (ms/frame) | Graph (ms/frame) |
|---|---|---|
| 1 | 0.514 | 0.476 |
| 2 | 0.296 | 0.239 |
| 3 | 0.411 | 0.376 |
| 4 | 0.637 | 0.505 |
| median | 0.46 | 0.43 |

The two paths are within noise of each other. The state machine costs about the same as the legacy path at this
scale. Render-side skinning (`UploadBoneMatrices`) was not measured.

## Tests

`tests/native/test_animation_blend.cpp` covers the blend math (50/50 crossfade midpoint at t = 0.1 of 0.2 s, the
hemisphere rule, continuity when a fade is interrupted, no pop at the start of a fade), the phase-synchronised 1D
blend, a three-state float-driven machine, a trigger consumed once, markers firing once per loop at the right step,
and graph errors.

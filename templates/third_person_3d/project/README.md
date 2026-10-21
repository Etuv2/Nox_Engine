# {{NAME}}

A third-person project from the **Third Person 3D** template.

Controls: WASD or the left stick move, Space or A jumps, Q/E or the right stick orbit the camera.
Collect the coins; each one respawns a few seconds after it is taken.

- `scenes/main.json`: the scene: sun, sky, ground, the level and the player (a `CharacterController` capsule
  with a box child for the body). The `Level` folder has a `StaticCollision` component: the platforms and ledges
  under it are collidable without a collider on each one. `Moving Platform` is a model with its own
  `StaticCollision`.
- `scripts/main.lua`: the entry script. `player.lua` is the controller and follow camera. `platform.lua` slides
  the moving platform. `pickups.lua` spawns coins with `ctx:instantiate` from `prefabs/coin.prefab.json` and
  respawns them with `ctx:after`; each coin has a `Trigger` volume and is taken on `trigger_enter`. `hud.lua`
  draws the counter.
- `input.json`: the action map (keyboard and gamepad).

The ground is a plane collider; the level is mesh geometry from the models (`StaticCollision`), so its shapes
follow the models. A `StaticCollision` component can go on any node; `include_children` covers every model below it.
See `docs/physics.md` in the engine for the rules.

Run it from the editor (Build > Run Standalone) or with
`Nox_Engine.exe --scene {{MAIN_SCENE}} --game`. Package it with
`tools\package.cmd -Profile {{NAME}} -SkipBuild`; the output is `Package\{{NAME}}\Play.cmd`.
The packaging profile (`packaging/{{NAME}}.json`) includes the coin prefab, because Lua instantiates it by path
and the packager cannot see that reference.

Lua API: `docs/lua_api.md`. Packaging: `docs/packaging.md`.

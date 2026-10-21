# {{NAME}}

A third-person project from the **Third Person 3D** template.

Controls: WASD or the left stick move, Space or A jumps, Q/E or the right stick orbit the camera.
Collect the coins; each one respawns a few seconds after it is taken.

- `scenes/main.json`: the scene: sun, sky, ground, platforms and the player (a `CharacterController` capsule
  with a box child for the body).
- `scripts/main.lua`: the entry script. `player.lua` is the controller and follow camera. `pickups.lua` spawns
  coins with `ctx:instantiate` from `prefabs/coin.prefab.json` and respawns them with `ctx:after`. `hud.lua`
  draws the counter.
- `input.json`: the action map (keyboard and gamepad).

The controller uses the physics world as it is: platforms are static boxes (`RigidBody` + `Collider`), and the
character walks on boxes and planes. Walkable mesh geometry needs more than this template provides.

Run it from the editor (Build > Run Standalone) or with
`Nox_Engine.exe --scene {{MAIN_SCENE}} --game`. Package it with
`tools\package.cmd -Profile {{NAME}} -SkipBuild`; the output is `Package\{{NAME}}\Play.cmd`.
The packaging profile (`packaging/{{NAME}}.json`) includes the coin prefab, because Lua instantiates it by path
and the packager cannot see that reference.

Lua API: `docs/lua_api.md`. Packaging: `docs/packaging.md`.

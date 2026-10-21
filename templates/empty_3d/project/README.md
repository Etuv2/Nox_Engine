# {{NAME}}

A 3D project from the **Empty 3D** template.

- `scenes/main.json`: the main scene (camera, sun, floor, sky). Edit it in the editor.
- `scripts/main.lua`: the entry script. `begin_play` runs when Play starts.
- `project.json`: the project descriptor. `input.json`: the action map (empty for now).

Run it from the editor (Build > Run Standalone) or with
`Nox_Engine.exe --scene {{MAIN_SCENE}} --game`. Package it with
`tools\package.cmd -Profile {{NAME}} -SkipBuild`; the output is `Package\{{NAME}}\Play.cmd`.

Lua API: `docs/lua_api.md`. Packaging: `docs/packaging.md`.

-- {{NAME}}: Third Person 3D template (docs/lua_api.md).
-- player.lua   the controller (CharacterController.move, jump) and the follow camera
-- pickups.lua  coins: ctx:instantiate, collected by distance, respawned with ctx:after
-- hud.lua      a text HUD drawn with ui.text
-- Keep the controller simple: it is the part a game usually replaces first.

require("player")
require("pickups")
require("hud")

nox.on("begin_play", function(ctx)
  ctx:log("{{NAME}}: begin_play, move with WASD or the left stick, jump with Space or A")
end)

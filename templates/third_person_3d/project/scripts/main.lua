-- {{NAME}}: Third Person 3D template (docs/lua_api.md).
-- player.lua   the controller (CharacterController.move, jump) and the follow camera
-- platform.lua a moving platform (StaticCollision on the node; the engine carries the player on it)
-- pickups.lua  coins: ctx:instantiate, a Trigger volume on each coin, collected on trigger_enter, respawned with ctx:after
-- hud.lua      a text HUD drawn with ui.text
-- Keep the controller simple: it is the part a game usually replaces first.

require("player")
require("platform")
require("pickups")
require("hud")

nox.on("begin_play", function(ctx)
  ctx:log("{{NAME}}: begin_play, move with WASD or the left stick, jump with Space or A")
end)

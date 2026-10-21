-- Coins (docs/lua_api.md: "Scenes and saves", "Lifecycle, timers and coroutines", "Physics").
-- A coin is an instance of prefabs/coin.prefab.json made with ctx:instantiate. The prefab has a Trigger volume:
-- when the player's capsule enters it, the engine raises trigger_enter and the coin is collected. A new coin appears
-- on the same spot RESPAWN_SECONDS later (ctx:after).

local M = {
  collected = 0,
  total = 0,
}

local COIN_PREFAB = "{{PROJECT_DIR}}/prefabs/coin.prefab.json"
local RESPAWN_SECONDS = 4.0

-- One coin per ledge (scenes/main.json) and one on the ground. The scene's layout runs away from the start
-- along -Z, which is the direction the follow camera looks at the start.
local SPOTS = {
  { -5.0, 2.6, -4.0 },    -- Ledge A
  { 5.0, 3.0, -1.0 },     -- Ledge B
  { 0.0, 3.9, -8.0 },     -- Ledge C
  { -6.0, 4.9, -10.5 },   -- Ledge D
  { -2.0, 1.0, -2.5 },    -- ground, beside the start platform
}

local coins = {}   -- coins[i] is the handle of the coin for SPOTS[i], or nil while it is waiting to respawn

local function spawn(ctx, index)
  local p = SPOTS[index]
  coins[index] = ctx:instantiate(COIN_PREFAB, { name = "Coin " .. index, position = p })
end

M.total = #SPOTS

nox.on("begin_play", function(ctx)
  M.collected = 0
  coins = {}
  for i = 1, #SPOTS do spawn(ctx, i) end
end)

-- trigger_enter: { trigger = handle, other = handle or nil }. A coin is taken by the player, not by any other body.
nox.on("trigger_enter", function(ctx, ev)
  local player = ctx:find("Player")
  if not player or ev.other ~= player then return end
  for i = 1, #SPOTS do
    local coin = coins[i]
    if coin and ev.trigger == coin then
      ctx:destroy(coin)
      coins[i] = nil
      M.collected = M.collected + 1
      ctx:after(RESPAWN_SECONDS, function(c) spawn(c, i) end)
      return
    end
  end
end)

return M

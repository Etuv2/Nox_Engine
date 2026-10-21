-- Coins (docs/lua_api.md: "Scenes and saves", "Lifecycle, timers and coroutines").
-- A coin is an instance of prefabs/coin.prefab.json made with ctx:instantiate. It is collected when the player
-- comes within COLLECT_RADIUS, and a new coin appears on the same spot RESPAWN_SECONDS later (ctx:after).
-- The controller is not a physics body, so collection is a distance test, not a trigger event.

local M = {
  collected = 0,
  total = 0,
}

local COIN_PREFAB = "{{PROJECT_DIR}}/prefabs/coin.prefab.json"
local RESPAWN_SECONDS = 4.0
local COLLECT_RADIUS = 1.1
local CHEST_HEIGHT = 0.9   -- the collection point is this far above the player's feet

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

nox.system{
  name = "coin_pickup",
  order = 5,
  update = function(ctx, dt)
    local player = ctx:find("Player")
    if not player then return end
    local px, py, pz = ctx:position(player)
    py = py + CHEST_HEIGHT
    for i = 1, #SPOTS do
      local coin = coins[i]
      if coin and ctx:is_valid(coin) then
        local cx, cy, cz = ctx:position(coin)
        local dx, dy, dz = cx - px, cy - py, cz - pz
        if dx * dx + dy * dy + dz * dz < COLLECT_RADIUS * COLLECT_RADIUS then
          ctx:destroy(coin)
          coins[i] = nil
          M.collected = M.collected + 1
          ctx:after(RESPAWN_SECONDS, function(c) spawn(c, i) end)
        end
      end
    end
  end,
}

return M

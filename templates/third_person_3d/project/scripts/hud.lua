-- HUD (docs/lua_api.md, "UI"): a coin counter and the controls. ui.text with "" uses the default font.

local pickups = require("pickups")

local PANEL = { r = 10, g = 12, b = 16, a = 190 }
local ACCENT = 0xFFFFD27F
local DIM = 0xFFB8C4D0

nox.on_draw(function(ctx, ui, dt)
  local _, h = ui.viewport()
  ui.rect_filled(16, 16, 240, 58, PANEL, 8)
  ui.text("", "COINS " .. pickups.collected .. " / " .. pickups.total, 32, 28, 24, ACCENT)
  ui.text("", "WASD move   Space jump   Q/E orbit", 16, h - 30, 16, DIM)
end)

-- HUD demo (docs/lua_api.md "UI" and "Input"): reads the project action map through ctx and draws with ui.*.
-- Nothing here is engine-specific. Fonts "label" and "digits" and the icon "hud/speedometer" are declared in project.json.

local Menu = require("menu")
local Music = require("music")

local hud = { throttle = 0, steer = 0, shots = 0, visible = true }

nox.system{
  name = "hud_input",
  order = -10,
  update = function(ctx, dt)
    -- Menus own the pointer; while one is open the car does not move and the engine fades out (audio paused).
    local blocked = Menu.blocking()
    Music.menuChanged(blocked)
    ctx:set_wants_pointer(blocked or hud.visible)
    if blocked then
      ctx:audio_set({ paused = true })
      return
    end
    local target = ctx:action_down("accelerate") and 1 or 0
    hud.throttle = hud.throttle + (target - hud.throttle) * math.min(1, dt * 6)
    hud.steer = ctx:axis("steer")
    if ctx:action_pressed("fire") then
      hud.shots = hud.shots + 1
      ctx:audio_cue("marker")
    end
    -- Procedural engine: the parameters are published once per frame (no Lua runs in the audio callback).
    ctx:audio_set({
      active = true,
      rpm = 900 + 6000 * hud.throttle,
      rpm_max = 7000,
      throttle = hud.throttle,
      speed01 = hud.throttle * 0.8,
      slip01 = math.abs(hud.steer) * hud.throttle,
      boost = ctx:action_down("fire"),
    })
    if ctx:action_pressed("hud_toggle") then hud.visible = not hud.visible end
  end,
}

local PANEL_BG = { r = 10, g = 12, b = 16, a = 190 }
local EDGE = 0xFF3A4A5A
local ACCENT = 0xFF5FD1FF
local WARM = 0xFFFFD27F
local DIM = 0xFFB8C4D0

nox.on_draw(function(ctx, ui, dt)
  if not hud.visible or not Menu.showHud or Menu.blocking() then return end
  local w, h = ui.viewport()
  local x, y = 24, h - 140
  local pw, ph = 300, 116

  ui.rect_filled(x, y, pw, ph, PANEL_BG, 8)
  ui.rect(x, y, pw, ph, EDGE, 1, 8)
  ui.image("hud/speedometer", x + 12, y + 14, 40, 40, 0xFFFFFFFF)
  ui.text("label", "THROTTLE", x + 64, y + 14, 18, DIM)

  -- Throttle bar, clipped to its track.
  local bx, by, bw = x + 64, y + 40, 220
  ui.rect(bx, by, bw, 10, EDGE, 1)
  ui.clip_push(bx, by, bw, 10)
  ui.rect_filled(bx, by, bw * hud.throttle, 10, ACCENT)
  ui.clip_pop()

  -- Steering: a centred tick that moves with the steer axis.
  local sx, sy = x + 64, y + 72
  ui.line(sx, sy, sx + bw, sy, EDGE, 2)
  local tick = sx + bw * 0.5 + (bw * 0.5) * hud.steer
  ui.circle_filled(tick, sy, 5, WARM)
  ui.text("label", "STEER", x + 64, y + 84, 14, DIM)

  -- Shot counter (digits font, right aligned).
  ui.text("digits", string.format("%03d", hud.shots), x + pw - 14, y + 14, 34, WARM, "right", "top")

  -- Gauge arc for the throttle, from -135 to +135 degrees.
  local gx, gy = x + pw + 40, y + ph * 0.5
  local a0 = -math.pi * 0.75
  local a1 = a0 + math.pi * 1.5 * hud.throttle
  ui.arc(gx, gy, 28, a0, math.pi * 0.75, 0xFF2A3440, 4)
  if hud.throttle > 0.01 then ui.arc(gx, gy, 28, a0, a1, ACCENT, 4) end

  -- Pointer marker (overlay coordinates).
  local px, py = ui.pointer()
  ui.circle(px, py, 6, WARM, 1)
end)

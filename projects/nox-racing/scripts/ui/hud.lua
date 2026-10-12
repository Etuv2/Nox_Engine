-- Racing HUD: instrument cluster, minimap, event/race panel, countdown, results, menus, messages.
-- Port of src/RacingHUD.cpp with the same layout (640x360 reference canvas, anchors from config).
--
-- Contract (scripts/ui/README.md): the session calls
--   local act = hud.frame(state, input, dt)      -- from a system; returns the UI actions
--   hud.draw(ui, dt)                             -- from a draw handler (uses the last frame's state)
-- Presentation only: nothing here changes race state. State fields mirror HudState (src/RacingHUD.h).

local D = require("ui.draw")
local config = require("ui.config")
local menus = require("ui.menus")
local minimap = require("ui.minimap")

local M = {}

local anim = {
  init = false, time = 0.0,
  rpm = 900.0, boost = 0.0, n2o = 1.0, speed = 0.0,
}

-- Last frame's inputs for draw.
local cur = { state = nil, view = nil, dt = 0.0, pointer = { hovered = -1, pressed = -1, start_hovered = false, start_pressed = false } }

local function black(alpha) return D.rgba(0, 0, 0, alpha) end

local function menu_open(state)
  return state.menu ~= nil and state.menu.title ~= nil and state.menu.title ~= "" and #(state.menu.items or {}) > 0
end

-- Pointer local to the overlay (regions are recorded in overlay coordinates).
local function local_pointer(view, p)
  if not p then return nil end
  return { x = p.x - view.x, y = p.y - view.y }
end

-- ---- Frame: actions for the session ------------------------------------------------------------
-- input = {
--   view = { x, y, w, h }          overlay rectangle in window coordinates (standalone: the window)
--   pointer = { x, y } | nil       window coordinates (ctx:pointer_position())
--   moved, pressed, released, down (left button), wheel (notches, + = up)
--   selected = 1-based menu selection (the session's)
-- }
-- Returns { menu_select = i|nil, menu_click = i|nil, menu_wheel = -1|0|1, start_click = bool }.
function M.frame(state, input, dt)
  local view = input.view
  cur.state = state
  cur.view = view
  cur.dt = math.min(math.max(dt or 0.0, 0.0), 0.1)
  local open = menu_open(state)
  local step = menus.pointer_step({
    pointer = local_pointer(view, input.pointer),
    moved = input.moved, pressed = input.pressed, released = input.released, down = input.down,
    wheel = input.wheel, menu_open = open, selected = input.selected,
    start_visible = (state.phase == "near_event") and not open,
  })
  cur.pointer = step
  return {
    menu_select = step.select,
    menu_click = step.click,
    menu_wheel = step.wheel,
    start_click = step.start_click,
  }
end

-- ---- Animation ---------------------------------------------------------------------------------
local function update(state, dt)
  local cfg = config.get()
  local g = cfg.gauges
  local boost_t = D.clamp01((state.load or 0.0) / math.max(0.01, g.boost_max))
  local n2o_t = D.clamp01(state.nitro or 1.0)
  local max_rpm = math.max(1000.0, g.rpm_max)
  if not anim.init then
    anim.init = true
    anim.rpm = state.rpm or 900.0
    anim.boost = boost_t
    anim.n2o = n2o_t
    anim.speed = state.speed_kmh or 0.0
  end
  anim.rpm = D.damp(anim.rpm, D.clamp((state.rpm or 0.0), 0.0, max_rpm * 1.02), 14.0, dt)
  anim.boost = D.damp(anim.boost, boost_t, 9.0, dt)
  anim.n2o = D.damp(anim.n2o, n2o_t, 6.0, dt)
  local kmh = (g.speed_units == "mph") and (state.speed_kmh or 0.0) * 0.621371 or (state.speed_kmh or 0.0)
  anim.speed = D.damp(anim.speed, kmh, 8.0, dt)
  minimap.update(state, dt)
end

-- ---- Instrument cluster ------------------------------------------------------------------------
local function companion_gauge(ui, xf, cx, cy, r_ref, value, fill_col, needle_value)
  local cfg = config.get()
  local c = cfg.colors
  local r = D.L(xf, r_ref)
  local SS, SW = D.SWEEP_START, D.SWEEP
  D.arc(ui, cx, cy, r * 0.97, SS, SS + SW, c.accent, D.L(xf, 0.5), c.glow * 0.5, 0.6)
  for i = 0, 10 do
    local a = SS + SW * i / 10
    local dx, dy = D.dir(a)
    D.seg(ui, cx + dx * r * 0.82, cy + dy * r * 0.82, cx + dx * r * 0.95, cy + dy * r * 0.95, c.text, D.L(xf, 0.6), 0.0, 0.55)
  end
  local v = D.clamp01(value)
  if v > 0.001 then D.arc(ui, cx, cy, r * 0.97, SS, SS + SW * v, fill_col, D.L(xf, 1.1), c.glow) end
  local na = SS + SW * D.clamp01(needle_value)
  local dx, dy = D.dir(na)
  D.seg(ui, cx - dx * r * 0.2, cy - dy * r * 0.2, cx + dx * r * 0.88, cy + dy * r * 0.88, c.needle, D.L(xf, 1.1), c.glow)
  ui.circle_filled(cx, cy, D.L(xf, 2.2), D.col(c.needle))
end

local function draw_cluster(ui, state, view, H)
  local cfg = config.get()
  local c = cfg.colors
  local g = cfg.gauges
  local xf = D.xf(view, cfg.placement.cluster, H)
  local P = function(x, y) return D.P(xf, x, y) end
  local L = function(v) return D.L(xf, v) end
  local glow = c.glow
  local SS, SW = D.SWEEP_START, D.SWEEP
  local rpm_max = math.max(1000.0, g.rpm_max)
  local red_frac = D.clamp01(g.redline / rpm_max)
  local red_pulse = (state.redline or state.limiter) and true or false
  local pulse = red_pulse and (0.55 + 0.45 * math.sin(anim.time * 18.0)) or 0.0

  -- Translucent dark backing.
  do
    local x0, y0 = P(460, 236)
    local x1, y1 = P(614, 356)
    ui.rect_filled(x0, y0, x1 - x0, y1 - y0, black(c.backing_alpha), L(8.0))
  end

  -- Tachometer.
  do
    local tx, ty = P(557, 291)
    local R = L(45.0)
    ui.circle_filled(tx, ty, R * 1.02, black(c.backing_alpha * 0.5))
    D.arc(ui, tx, ty, R * 0.97, SS, SS + SW, c.accent, L(0.6), glow * 0.7)
    D.arc(ui, tx, ty, R * 0.97, SS + SW * red_frac, SS + SW, c.redline, L(1.6), glow, 0.75 + 0.25 * pulse)

    -- Ticks: minor every 200 rpm, major every 1000 rpm.
    local steps = math.max(1, math.floor(rpm_max / 200.0 + 0.5))
    for i = 0, steps do
      local f = i / steps
      local a = SS + SW * f
      local dx, dy = D.dir(a)
      local major = (i % 5) == 0
      local hi = i * 200.0 >= g.redline
      local tc = hi and c.redline or c.text
      local r0 = R * (major and 0.80 or 0.90)
      local r1 = R * (major and 0.97 or 0.94)
      D.seg(ui, tx + dx * r0, ty + dy * r0, tx + dx * r1, ty + dy * r1, tc, L(major and 1.3 or 0.6), glow * 0.5,
        hi and (0.8 + 0.2 * pulse) or 0.85)
    end
    -- Numbered labels 0..N (x1000 rpm).
    local labels = math.floor(rpm_max / 1000.0 + 0.001)
    for k = 0, labels do
      local f = k * 1000.0 / rpm_max
      local dx, dy = D.dir(SS + SW * f)
      local hi = k * 1000.0 >= g.redline
      local px = L(11.0)
      D.text_glow(ui, cfg.fonts.label, px, tx + dx * R * 0.64, ty + dy * R * 0.64,
        hi and c.redline or c.accent, tostring(k), "center", glow * 0.6, hi and (0.85 + 0.15 * pulse) or 0.95)
    end
    if red_pulse then ui.circle(tx, ty, R * 1.04, D.col(c.redline, 0.35 * pulse), L(1.5)) end

    -- Needle (damped) and hub.
    do
      local dx, dy = D.dir(SS + SW * D.clamp01(anim.rpm / rpm_max))
      D.seg(ui, tx - dx * R * 0.22, ty - dy * R * 0.22, tx + dx * R * 0.92, ty + dy * R * 0.92, c.needle, L(1.6), glow * 1.2)
      ui.circle_filled(tx, ty, L(3.2), D.rgba(0.08, 0.02, 0.02, 1.0))
      ui.circle(tx, ty, L(3.2), D.col(c.needle), L(0.9))
    end
  end

  -- Digital speed box (blue) with unlit ghost segments.
  do
    local x0, y0 = P(537, 318)
    local x1, y1 = P(578, 334)
    local mid = (y0 + y1) * 0.5
    ui.rect_filled(x0, y0, x1 - x0, mid - y0, D.col(c.speed, 0.95))
    ui.rect_filled(x0, mid, x1 - x0, y1 - mid, D.col(c.speed, 0.55))
    ui.rect(x0, y0, x1 - x0, y1 - y0, D.col(c.speed, 0.6), L(0.6), L(1.5))
    local rx, ry = P(575.5, 326.5)
    local dpx = L(14.0)
    ui.text(cfg.fonts.digital, "888", rx, ry, dpx, D.rgba(0.04, 0.10, 0.22, 0.12), "right", "middle")
    local spd = string.format("%d", math.floor(math.max(0.0, anim.speed) + 0.5))
    ui.text(cfg.fonts.digital, spd, rx, ry, dpx, D.rgba(0.03, 0.08, 0.20, 1.0), "right", "middle")
    local ux, uy = P(557.5, 340.5)
    ui.text(cfg.fonts.label, g.speed_units, ux, uy, L(7.0), D.col(c.speed, 0.9), "center", "middle")
  end

  -- Gear box.
  do
    local x0, y0 = P(516, 322)
    local x1, y1 = P(529, 339)
    ui.rect_filled(x0, y0, x1 - x0, y1 - y0, black(0.55), L(2.0))
    if red_pulse then ui.rect_filled(x0, y0, x1 - x0, y1 - y0, D.col(c.redline, 0.35 * pulse), L(2.0)) end
    ui.rect(x0, y0, x1 - x0, y1 - y0, D.col(red_pulse and c.redline or c.accent, 0.7), L(0.7), L(2.0))
    local gear
    if (state.gear or 0) < 0 then gear = "R"
    elseif (state.gear or 0) == 0 then gear = "N"
    else gear = tostring(state.gear) end
    local gx, gy = P(522.5, 330.5)
    D.text_glow(ui, cfg.fonts.label, L(14.0), gx, gy, red_pulse and c.redline or c.text, gear, "center", glow * 0.5)
  end

  -- Companion gauges: BOOST above, N2O below.
  do
    local gx, gy = P(481, 277)
    companion_gauge(ui, xf, gx, gy, 21.0, anim.boost, c.accent, anim.boost)
  end
  do
    local bx, by = P(481, 288)
    local px = L(6.5)
    D.text_glow(ui, cfg.fonts.label, px, bx, by, c.accent, g.boost_label, "center", glow * 0.4, 0.9)
    D.icon_left_of(ui, "gauge/boost", bx - D.text_w(ui, cfg.fonts.label, px, g.boost_label) * 0.5, by, L(8.0), c.accent, 0.9)
  end
  do
    local nx, ny = P(481, 324)
    local rN = L(21.0)
    local v = D.clamp01(anim.n2o)
    local SS2, SW2 = SS, SW
    D.arc(ui, nx, ny, rN * 0.97, SS2, SS2 + SW2, c.text, L(0.6), 0.0, 0.25)
    if v > 0.001 then
      local flick = 0.5 + 0.5 * math.sin(anim.time * 40.0)
      local k = (state.nitro_active) and (0.85 + 0.15 * flick) or 1.0
      D.arc(ui, nx, ny, rN * 0.97, SS2, SS2 + SW2 * v, c.speed, L(1.1), glow * k)
    end
    if state.nitro_recharging and v < 0.999 then
      -- Recharge comet travelling along the filled part of the arc.
      local head = v * math.fmod(anim.time * 0.9, 1.0)
      local tail = math.max(0.0, head - 0.12)
      D.arc(ui, nx, ny, rN * 0.97, SS2 + SW2 * tail, SS2 + SW2 * head, c.text, L(1.2), glow)
    end
    local dx, dy = D.dir(SS2 + SW2 * D.clamp01(anim.n2o))
    D.seg(ui, nx - dx * rN * 0.2, ny - dy * rN * 0.2, nx + dx * rN * 0.86, ny + dy * rN * 0.86, c.needle, L(1.1), glow)
    ui.circle_filled(nx, ny, L(2.2), D.col(c.needle))
  end
  do
    local bx, by = P(481, 334)
    local px = L(6.5)
    D.text_glow(ui, cfg.fonts.label, px, bx, by, c.text, g.n2o_label, "center", glow * 0.4, 0.9)
    D.icon_left_of(ui, "gauge/n2o", bx - D.text_w(ui, cfg.fonts.label, px, g.n2o_label) * 0.5, by, L(8.0), c.text, 0.9)
  end
end

-- ---- World-anchored event flag and checkpoint gate ----------------------------------------------
local function label_at(ui, xf, ref, x, y, c, text, alpha, halign)
  local px = D.L(xf, ref)
  local cfg = config.get()
  D.text_glow(ui, cfg.fonts.label, px, x, y, c, text, halign, cfg.colors.glow * 0.5, alpha)
end

local function draw_event_marker(ui, state, view, H)
  local m = state.event_marker
  if not m or not m.visible or menu_open(state) then return end
  local cfg = config.get()
  local c = cfg.colors
  local d = math.max(0.0, m.distance or 0.0)
  local f = D.clamp(1.15 - d / 700.0, 0.55, 1.15)
  local alpha = D.clamp(1.25 - d / 900.0, 0.35, 1.0)
  local bx = view.x + D.clamp(m.x, 0.03, 0.97) * view.w
  local by = view.y + D.clamp(m.y, 0.03, 0.97) * view.h
  local xf = D.xf_at(bx, by, H / D.REF_H * cfg.placement.event.scale * f)
  local P = function(x, y) return D.P(xf, x, y) end
  -- Flag: pole and pennant.
  local x1, y1 = P(0, 8)
  local x2, y2 = P(0, -9)
  D.seg(ui, x1, y1, x2, y2, c.text, D.L(xf, 0.9), c.glow * 0.4, alpha)
  local f0x, f0y = P(0, -9)
  local f1x, f1y = P(11, -6)
  local f2x, f2y = P(0, -3)
  local ix, iy = P(6, -8)
  if not D.icon(ui, "event/" .. (state.event_type or ""), ix, iy, D.L(xf, 14.0), c.lavender, alpha) then
    ui.polygon_filled({ f0x, f0y, f1x, f1y, f2x, f2y }, D.col(c.lavender, alpha))
  end
  -- Titles.
  local tx, ty = P(0, 17)
  label_at(ui, xf, 16.0, tx, ty, c.lavender, state.event_type or "", alpha, "center")
  local sub = state.event_name or ""
  if state.event_difficulty and state.event_difficulty ~= "" then sub = sub .. " (" .. state.event_difficulty .. ")" end
  local sx, sy = P(0, 28)
  label_at(ui, xf, 10.5, sx, sy, c.lavender, sub, alpha * 0.9, "center")
  local dx, dy = P(0, 39)
  label_at(ui, xf, 8.0, dx, dy, c.text, string.format("%d m", math.floor(d + 0.5)), alpha * 0.8, "center")
  -- Chevron pointing down at the marker.
  local ax, ay = P(-4, 46)
  local bxp, byp = P(0, 50)
  D.seg(ui, ax, ay, bxp, byp, c.text, D.L(xf, 0.9), 0.0, 0.7 * alpha)
  local cx2, cy2 = P(4, 46)
  D.seg(ui, cx2, cy2, bxp, byp, c.text, D.L(xf, 0.9), 0.0, 0.7 * alpha)
end

local function draw_checkpoint_marker(ui, state, view, H)
  local m = state.checkpoint_marker
  if not m or not m.visible then return end
  local cfg = config.get()
  local c = cfg.colors
  local d = math.max(0.0, m.distance or 0.0)
  local f = D.clamp(1.0 - d / 800.0, 0.5, 1.0)
  local alpha = D.clamp(1.2 - d / 900.0, 0.3, 1.0)
  local bx = view.x + D.clamp(m.x, 0.03, 0.97) * view.w
  local by = view.y + D.clamp(m.y, 0.03, 0.97) * view.h
  local xf = D.xf_at(bx, by, H / D.REF_H * f)
  local P = function(x, y) return D.P(xf, x, y) end
  -- Gate: two posts and a lintel, with a chevron above.
  local function seg(ax, ay, bx2, by2, thick)
    local x1, y1 = P(ax, ay)
    local x2, y2 = P(bx2, by2)
    D.seg(ui, x1, y1, x2, y2, c.accent, D.L(xf, thick), c.glow, alpha)
  end
  seg(-9, 6, -9, -7, 1.2)
  seg(9, 6, 9, -7, 1.2)
  seg(-9, -7, 9, -7, 1.2)
  local ix, iy = P(0, -14)
  if not D.icon(ui, "map/checkpoint", ix, iy, D.L(xf, 11.0), c.text, 0.9 * alpha) then
    local x1, y1 = P(-5, -13)
    local x2, y2 = P(0, -17)
    D.seg(ui, x1, y1, x2, y2, c.text, D.L(xf, 0.9), 0.0, 0.8 * alpha)
    x1, y1 = P(5, -13)
    D.seg(ui, x1, y1, x2, y2, c.text, D.L(xf, 0.9), 0.0, 0.8 * alpha)
  end
  local tx, ty = P(0, 16)
  label_at(ui, xf, 8.5, tx, ty, c.text, string.format("%d m", math.floor(d + 0.5)), alpha * 0.9, "center")
end

-- ---- Top-right event / race panel and SPEEDWALL block ------------------------------------------
local function draw_panel(ui, state, view, H)
  local phase = state.phase
  if phase == "free_roam" or phase == "paused" then return end
  local cfg = config.get()
  local c = cfg.colors
  local glow = c.glow
  local xf = D.xf(view, cfg.placement.panel, H)
  local P = function(x, y) return D.P(xf, x, y) end
  local L = function(v) return D.L(xf, v) end
  local label = cfg.fonts.label
  local digital = cfg.fonts.digital

  if phase == "near_event" then
    local kpx = L(9.0)
    local cx, cy = P(552, 75.5)
    local start_txt = "START EVENT"
    D.txt(ui, label, kpx, cx, cy, c.text, start_txt, "right")
    local w_txt = D.text_w(ui, label, kpx, start_txt)
    D.icon_left_of(ui, "key/" .. (state.start_key or "Enter"), cx - w_txt, cy, L(9.0), c.text, 1.0)
    -- Clickable prompt: the region is recorded for the next frame's hit test.
    do
      local w = w_txt + L(14.0)
      local ax, ay = cx - w, select(2, P(0, 68))
      local bx, by = P(602, 83)
      menus.record_start({ ax, ay, bx, by })
      if cur.pointer.start_hovered or cur.pointer.start_pressed then
        local a = cur.pointer.start_pressed and 0.45 or 0.22
        ui.rect_filled(ax, ay, bx - ax, by - ay, D.col(c.lavender, a), L(2.0))
        ui.rect(ax, ay, bx - ax, by - ay, D.col(c.lavender, 0.9), L(0.7), L(2.0))
      end
    end
    local kx, ky = P(559, 70)
    local kx2, ky2 = P(600, 81)
    ui.rect_filled(kx, ky, kx2 - kx, ky2 - ky, D.col(c.text, 0.95), L(1.5))
    local kkx, kky = P(579.5, 75.5)
    D.txt(ui, label, L(7.5), kkx, kky, { 0.05, 0.05, 0.08, 1.0 }, state.start_key or "Enter", "center")
  end

  -- Event title and name.
  do
    local tx, ty = P(547, 103)
    D.text_glow(ui, label, L(16.0), tx, ty, c.lavender, state.event_type or "", "center", glow * 0.5)
    local nx, ny = P(543, 120)
    D.text_glow(ui, label, L(10.5), nx, ny, c.text, state.event_name or "", "center", glow * 0.3, 0.92)
  end

  if phase == "racing" or phase == "finished" then
    local tx, ty = P(547, 140)
    D.txt(ui, digital, L(18.0), tx, ty, c.speed, D.format_time(state.race_time), "center")
    local spx = L(9.0)
    local cpx, cpy = P(498, 158)
    D.txt(ui, label, spx, cpx, cpy, c.text, string.format("CP %d/%d", state.checkpoint or 0, state.checkpoint_count or 0), "left")
    local last = "LAST "
    local splits = state.splits or {}
    if #splits > 0 then last = last .. string.format("%+.2f", splits[#splits]) else last = last .. "--" end
    local lx, ly = P(601, 158)
    D.txt(ui, label, spx, lx, ly, c.text, last, "right", true, 0.85)
  end

  if phase == "near_event" or phase == "countdown" or phase == "racing" or phase == "finished" then
    if phase == "near_event" or phase == "countdown" then
      local sx, sy = P(601, 150)
      D.txt(ui, label, L(7.5), sx, sy, { 0.85, 0.88, 0.95, 0.85 }, "SPEEDWALL", "right")
    end
    local ax, ay = P(496, 164)
    local bx, by = P(604, 181)
    ui.rect_filled(ax, ay, bx - ax, by - ay, black(0.35), L(2.0))
    local bpx = L(9.0)
    local hx, hy = P(502, 172.5)
    D.txt(ui, label, bpx, hx, hy, c.text, state.best_holder or "You", "left")
    local btx, bty = P(600, 172.5)
    D.txt(ui, label, bpx, btx, bty, c.text, D.format_time(state.best_time), "right")
  end
end

-- ---- Countdown, results, menus, centre message --------------------------------------------------
local function draw_centre(ui, state, view, H)
  local cfg = config.get()
  local c = cfg.colors
  local glow = c.glow
  local xf = D.xf(view, { anchor = "MC", offset = { 0, 0 }, scale = 1 }, H)
  local function cp(dx, dy) return D.P(xf, 320.0 + dx, 180.0 + dy) end
  local L = function(v) return D.L(xf, v) end
  local label = cfg.fonts.label
  local digital = cfg.fonts.digital
  local phase = state.phase

  if phase == "countdown" then
    local cd = math.max(0.0, state.countdown or 0.0)
    local n = math.ceil(cd)
    local text = n > 0 and string.format("%d", n) or "GO"
    local frac = cd - math.floor(cd)
    local pop = 1.0 + 0.18 * frac * frac
    local px = L(56.0) * pop
    local x, y = cp(0, 0)
    D.text_glow(ui, label, px, x, y, c.text, text, "center", glow)
  end

  if phase == "finished" then
    local hx, hy = cp(0, -84)
    local hpx = L(14.0)
    D.text_glow(ui, label, hpx, hx, hy, c.lavender, "FINISHED", "center", glow * 0.5)
    D.icon_left_of(ui, "result/finished", hx - D.text_w(ui, label, hpx, "FINISHED") * 0.5, hy, L(16.0), c.lavender, 1.0)
    local tx, ty = cp(0, -60)
    local t = state.last_time and state.last_time >= 0 and state.last_time or (state.race_time or 0.0)
    D.txt(ui, digital, L(22.0), tx, ty, c.speed, D.format_time(t), "center")
    local bx, by = cp(0, -40)
    local best = "BEST " .. D.format_time(state.best_time)
    D.txt(ui, label, L(10.0), bx, by, c.text, best, "center", true, 0.9)
    if state.new_record then
      local p = 0.6 + 0.4 * math.sin(anim.time * 6.0)
      local rx, ry = cp(0, -28)
      local rpx = L(11.0)
      D.text_glow(ui, label, rpx, rx, ry, c.accent, "NEW RECORD", "center", glow, p)
      D.icon_left_of(ui, "result/record", rx - D.text_w(ui, label, rpx, "NEW RECORD") * 0.5, ry, L(12.0), c.accent, p)
    end
  end

  local title = state.menu and state.menu.title or ""
  if title ~= "" and menu_open(state) then
    local top = menus.menu_top(phase)
    local mpx = L(14.0)
    local tx, ty = cp(0, top)
    D.text_glow(ui, label, mpx, tx, ty, c.lavender, title, "center", glow * 0.4)
    D.icon_left_of(ui, "menu/" .. title, tx - D.text_w(ui, label, mpx, title) * 0.5, ty, L(15.0), c.lavender, 0.9)
    -- Modal: the whole view blocks clicks below and takes the wheel.
    menus.record_modal({ view.x, view.y, view.x + view.w, view.y + view.h })
    local items = state.menu.items
    local sel = state.menu.selected or 1
    local cx, cy = D.P(xf, 320.0, 180.0)
    local s = L(1.0)
    for i = 1, #items do
      local y = menus.item_y(top, i)
      local sx, sy = cp(0, y)
      local rect = menus.item_rect(cx, cy, s, top, i)
      menus.record_item(i, rect)
      local hov = cur.pointer.hovered == i
      local prs = cur.pointer.pressed == i
      local is_sel = (i == sel)
      if is_sel or hov or prs then
        local inset = prs and L(0.8) or 0.0
        local ax, ay, bx2, by2 = rect[1] + inset, rect[2] + inset, rect[3] - inset, rect[4] - inset
        local fill = prs and 0.55 or (is_sel and 0.30 or 0.14)
        ui.rect_filled(ax, ay, bx2 - ax, by2 - ay, D.col(c.lavender, fill), L(2.0))
        ui.rect(ax, ay, bx2 - ax, by2 - ay, D.col(hov and c.text or c.lavender, hov and 0.95 or 0.8), L(hov and 1.0 or 0.7), L(2.0))
      end
      local ipx = L(11.0)
      local text_col = is_sel and c.text or { 0.85, 0.85, 0.9, 0.7 }
      D.txt(ui, label, ipx, sx, sy, text_col, items[i], "center")
      local icol = is_sel and c.text or { 0.85, 0.85, 0.9, 1.0 }
      D.icon_left_of(ui, "menu/" .. items[i], sx - D.text_w(ui, label, ipx, items[i]) * 0.5, sy, L(11.0), icol, is_sel and 1.0 or 0.7)
    end
  end

  local msg = state.message
  local ma = D.clamp(state.message_alpha or 0.0, 0.0, 1.0)
  if msg and msg ~= "" and ma > 0.001 then
    local px = L(14.0)
    local mx, my = cp(0, -78)
    D.text_glow(ui, label, px, mx, my, c.text, msg, "center", glow, ma)
    local id = D.message_icon_id(msg)
    D.icon_id(ui, id, mx - D.text_w(ui, label, px, msg) * 0.5 - L(15.0) * 0.3 - L(15.0) * 0.5, my, L(15.0), c.redline, ma)
  end
end

local function draw_hint(ui, state, view, H)
  local hint = state.hint
  if not hint or hint == "" then return end
  local cfg = config.get()
  local xf = D.xf(view, cfg.placement.hint, H)
  local px = D.L(xf, 9.0)
  local x, y = D.P(xf, 14, 14)
  D.txt(ui, cfg.fonts.label, px, x, y, { 1, 1, 1, 0.7 }, hint, "left", false)
end

-- Draws the HUD into the overlay (called from the draw handler). The view is the overlay rect
-- (ui.viewport()); the state and pointer regions are the ones of the last frame.
function M.draw(ui, dt)
  local state = cur.state
  if not state then return end
  local w, h = ui.viewport()
  local view = { x = 0, y = 0, w = w, h = h }
  if w <= 1 or h <= 1 then return end
  dt = math.min(math.max(dt or cur.dt, 0.0), 0.1)
  anim.time = anim.time + dt
  update(state, dt)
  menus.begin_draw()
  minimap.draw(ui, state, D.xf(view, config.get().placement.minimap, h))
  draw_cluster(ui, state, view, h)
  draw_event_marker(ui, state, view, h)
  draw_checkpoint_marker(ui, state, view, h)
  draw_panel(ui, state, view, h)
  draw_centre(ui, state, view, h)
  draw_hint(ui, state, view, h)
end

-- Tests and the preview use the last frame's pointer feedback.
function M.pointer_feedback() return cur.pointer end

return M

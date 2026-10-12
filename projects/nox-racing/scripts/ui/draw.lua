-- Drawing helpers over the generic ui.* primitives (docs/lua_api.md "UI").
-- Layout is authored on the 640x360 reference canvas and scaled by view height / 360, anchored to a
-- point of the view rectangle, exactly as src/RacingHUD.cpp (MakeXf / Xf::P / Xf::L).

local config = require("ui.config")

local D = {}

D.PI = math.pi
D.REF_W = 640
D.REF_H = 360
-- Tachometer / gauge sweep: from the lower-left (150 deg) clockwise over the top to the lower-right.
D.SWEEP_START = 150 * math.pi / 180
D.SWEEP = 240 * math.pi / 180

function D.clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

function D.clamp01(v) return D.clamp(v, 0.0, 1.0) end

-- Frame-rate independent smoothing (RacingHUD Damp).
function D.damp(cur, target, rate, dt)
  return cur + (target - cur) * (1.0 - math.exp(-rate * dt))
end

function D.wrap_angle(a)
  while a > D.PI do a = a - 2 * D.PI end
  while a < -D.PI do a = a + 2 * D.PI end
  return a
end

-- Colour: a float rgba (config) to the engine's integer table, with an alpha multiplier.
function D.col(v, alpha)
  local a = D.clamp01((v[4] or 1.0) * (alpha or 1.0))
  return {
    r = math.floor(D.clamp01(v[1]) * 255 + 0.5),
    g = math.floor(D.clamp01(v[2]) * 255 + 0.5),
    b = math.floor(D.clamp01(v[3]) * 255 + 0.5),
    a = math.floor(a * 255 + 0.5),
  }
end

-- Plain RGBA in 0..1 (not from config).
function D.rgba(r, g, b, a)
  return D.col({ r, g, b, a or 1.0 }, 1.0)
end

local ANCHOR_X = { L = 0.0, C = 0.5, R = 1.0 }
local ANCHOR_Y = { T = 0.0, M = 0.5, B = 1.0 }

-- Transform of one placed element: base point in view pixels, reference anchor, offset, scale.
function D.xf(view, placement, viewH)
  local a = string.upper(placement.anchor or "TL")
  local fx = ANCHOR_X[string.sub(a, 2, 2)] or 0.0
  local fy = ANCHOR_Y[string.sub(a, 1, 1)] or 0.0
  return {
    bx = view.x + fx * view.w,
    by = view.y + fy * view.h,
    rx = fx * D.REF_W,
    ry = fy * D.REF_H,
    ox = placement.offset[1] or 0,
    oy = placement.offset[2] or 0,
    s = viewH / D.REF_H * (placement.scale or 1.0),
  }
end

-- Free transform (markers placed at a normalised screen point).
function D.xf_at(bx, by, s)
  return { bx = bx, by = by, rx = 0, ry = 0, ox = 0, oy = 0, s = s }
end

function D.P(x, px, py)
  return x.bx + (px - x.rx + x.ox) * x.s, x.by + (py - x.ry + x.oy) * x.s
end

function D.L(x, v) return v * x.s end

-- Glowing polyline: wide low-alpha layers under a sharp core (RacingHUD Stroke). glow in 0..n.
function D.stroke(ui, pts, c, thick, glow, alpha)
  alpha = alpha or 1.0
  if thick <= 0 then return end
  if glow > 0 then
    ui.polyline(pts, D.col(c, 0.08 * glow * alpha), thick * 4.5, false)
    ui.polyline(pts, D.col(c, 0.20 * glow * alpha), thick * 2.2, false)
  end
  ui.polyline(pts, D.col(c, alpha), thick, false)
end

function D.seg(ui, x1, y1, x2, y2, c, thick, glow, alpha)
  alpha = alpha or 1.0
  if glow and glow > 0 then
    ui.line(x1, y1, x2, y2, D.col(c, 0.08 * glow * alpha), thick * 4.5)
    ui.line(x1, y1, x2, y2, D.col(c, 0.20 * glow * alpha), thick * 2.2)
  end
  ui.line(x1, y1, x2, y2, D.col(c, alpha), thick)
end

-- Arc with the same glow layering. Angles in radians, screen convention (y down).
function D.arc(ui, cx, cy, r, a0, a1, c, thick, glow, alpha)
  alpha = alpha or 1.0
  if glow and glow > 0 then
    ui.arc(cx, cy, r, a0, a1, D.col(c, 0.08 * glow * alpha), thick * 4.5)
    ui.arc(cx, cy, r, a0, a1, D.col(c, 0.20 * glow * alpha), thick * 2.2)
  end
  ui.arc(cx, cy, r, a0, a1, D.col(c, alpha), thick)
end

function D.dir(a) return math.cos(a), math.sin(a) end

-- Text with a soft halo: eight low-alpha copies around the core (RacingHUD TxtGlow).
function D.text_glow(ui, font, px, x, y, c, s, halign, glow, alpha, valign)
  alpha = alpha or 1.0
  valign = valign or "middle"
  if glow and glow > 0 and s and s ~= "" then
    local d = math.max(0.6, px * 0.05)
    local g = D.col(c, 0.14 * glow * alpha)
    local offs = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 0.7, 0.7 }, { -0.7, 0.7 }, { 0.7, -0.7 }, { -0.7, -0.7 } }
    for _, o in ipairs(offs) do
      ui.text(font, s, x + o[1] * d, y + o[2] * d, px, g, halign, valign)
    end
  end
  ui.text(font, s, x, y, px, D.col(c, alpha), halign, valign)
end

-- Plain text (RacingHUD Txt). vcentre centres the line on y.
function D.txt(ui, font, px, x, y, c, s, halign, vcentre, alpha)
  if not s or s == "" or px <= 0.5 then return end
  ui.text(font, s, x, y, px, D.col(c, alpha or 1.0), halign or "left", vcentre == false and "top" or "middle")
end

function D.text_w(ui, font, px, s)
  if not s or s == "" then return 0 end
  local w = ui.text_size(font, s, px)
  return w or 0
end

-- Icon by atlas id centred on (cx, cy). Unknown ids draw nothing (ui.image contract).
function D.icon_id(ui, id, cx, cy, size, c, alpha)
  if not id then return false end
  ui.image(id, cx - size * 0.5, cy - size * 0.5, size, size, D.col(c, alpha or 1.0))
  return true
end

-- Icon for a configured role ("gauge/boost", "menu/Resume", ...), or false when unmapped.
function D.icon(ui, role, cx, cy, size, c, alpha)
  return D.icon_id(ui, config.icon(role), cx, cy, size, c, alpha)
end

-- Icon whose right edge sits a small gap left of leftX.
function D.icon_left_of(ui, role, leftX, y, size, c, alpha)
  local gap = size * 0.3
  return D.icon(ui, role, leftX - gap - size * 0.5, y, size, c, alpha)
end

-- Warning icon for a centre message: the "warning/<prefix>" role whose prefix the text starts with.
function D.message_icon_id(msg)
  for key, id in pairs(config.get().icon_roles) do
    if string.sub(key, 1, 8) == "warning/" then
      local prefix = string.sub(key, 9)
      if prefix ~= "" and string.sub(msg, 1, #prefix) == prefix then return id end
    end
  end
  return nil
end

-- "m:ss.cc" like RacingHUD FormatTime; "--:--.--" when negative.
function D.format_time(s)
  if s == nil or s < 0 then return "--:--.--" end
  local cs = math.floor(s * 100 + 0.5)
  return string.format("%02d:%02d.%02d", cs // 6000, (cs // 100) % 60, cs % 100)
end

return D

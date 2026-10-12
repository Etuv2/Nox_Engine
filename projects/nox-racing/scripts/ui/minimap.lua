-- Heading-up translucent minimap (RacingHUD::DrawMinimap). Roads come from a nox.roads handle as
-- edge polylines (net:edge_polylines), cached per handle and culled per frame.

local D = require("ui.draw")
local config = require("ui.config")
local roads = require("ui.roads")

local M = {}

-- Smoothed view state (damped toward the frame's car heading and speed-based radius).
local anim = { init = false, heading = 0.0, radius = 60.0 }

-- Advance the smoothed heading/radius. state.car_heading (rad), state.speed_kmh.
function M.update(state, dt)
  local cfg = config.get()
  local m = cfg.minimap
  if not anim.init then
    anim.init = true
    anim.heading = state.heading or 0.0
    anim.radius = m.radius_m
  end
  anim.heading = anim.heading + D.wrap_angle((state.heading or 0.0) - anim.heading) * (1.0 - math.exp(-8.0 * dt))
  local target = m.radius_m + math.max(0.0, state.speed_kmh or 0.0) * m.zoom_with_speed
  anim.radius = D.damp(anim.radius, target, 2.5, dt)
end

-- Draws the minimap at its placement. xf: the cluster-style transform for the minimap anchor.
function M.draw(ui, state, xf)
  local cfg = config.get()
  local m = cfg.minimap
  local cx, cy = D.P(xf, 88, 280)
  local rpx = D.L(xf, m.radius_px)
  local op = D.clamp01(m.opacity)
  local k = rpx / math.max(5.0, anim.radius)
  -- Heading-up: forward = (sin h, cos h) in (x, z); right = (-cos h, sin h). Screen up = forward.
  local sh, ch = math.sin(anim.heading), math.cos(anim.heading)
  local carX, carZ = state.car.x, state.car.z
  local function to_map(wx, wz)
    local dx, dz = wx - carX, wz - carZ
    local rx = -ch * dx + sh * dz
    local fw = sh * dx + ch * dz
    return cx + rx * k, cy - fw * k
  end

  -- Soft feathered disc: faint stacked rings, no opaque fill or hard frame.
  for i = 0, 4 do
    local t = 1.0 - 0.16 * i
    ui.circle_filled(cx, cy, rpx * t, D.rgba(0.02, 0.03, 0.05, 0.05 * op))
  end

  -- Roads and route are clipped to the map square (the native code clips to the disc).
  ui.clip_push(cx - rpx, cy - rpx, rpx * 2, rpx * 2)
  if state.roads then
    local reach = math.max(5.0, anim.radius) * 1.5
    local minX, maxX = carX - reach, carX + reach
    local minZ, maxZ = carZ - reach, carZ + reach
    local road_col = { 0.82, 0.90, 1.0, 1.0 }
    for _, line in ipairs(roads.lines(state.roads)) do
      if line.maxx >= minX and line.minx <= maxX and line.maxz >= minZ and line.minz <= maxZ then
        local pts = {}
        local flat = line.pts
        for i = 1, #flat - 1, 2 do
          local px, py = to_map(flat[i], flat[i + 1])
          pts[#pts + 1] = px
          pts[#pts + 1] = py
        end
        ui.polyline(pts, D.col(road_col, 0.62 * op), D.L(xf, m.road_px), false)
      end
    end
  end

  -- Route: highlighted polyline in the accent colour.
  local route = state.route or {}
  if #route >= 2 then
    local pts = {}
    for _, p in ipairs(route) do
      local px, py = to_map(p.x, p.z)
      pts[#pts + 1] = px
      pts[#pts + 1] = py
    end
    D.stroke(ui, pts, cfg.colors.accent, D.L(xf, 2.0), cfg.colors.glow * 0.6, op)
  end

  -- Markers: inside the disc at their position, otherwise clamped to the rim (dimmed).
  local accent, lavender, text = cfg.colors.accent, cfg.colors.lavender, cfg.colors.text
  for _, mk in ipairs(state.markers or {}) do
    local px, py = to_map(mk.x, mk.z)
    local ux, uy = px - cx, py - cy
    local dist = math.sqrt(ux * ux + uy * uy)
    local alpha = op
    local lim = rpx - D.L(xf, 5.0)
    local clamped = false
    local draw = true
    if dist > lim then
      if dist < 1e-4 then
        draw = false
      else
        px, py = cx + ux * lim / dist, cy + uy * lim / dist
        alpha = alpha * 0.55
        clamped = true
      end
    end
    if draw then
      local r = D.L(xf, 4.2)
      -- Off-map checkpoints become a small rim dot (no overlapping icon clusters on the rim).
      if clamped and mk.kind == "checkpoint" then
        ui.circle_filled(px, py, r * 0.45, D.col(accent, 0.7 * alpha))
      elseif mk.kind == "event" then
        if not D.icon_id(ui, config.icon("event/" .. (mk.label or "")), px, py, r * 2.4, lavender, 0.95 * alpha) then
          ui.circle_filled(px, py, r, D.col(lavender, 0.9 * alpha))
          ui.circle_filled(px, py, r * 0.4, D.rgba(1, 1, 1, alpha))
        end
      elseif mk.kind == "checkpoint" then
        if not D.icon(ui, "map/checkpoint", px, py, r * 2.6, accent, alpha) then
          ui.circle(px, py, r, D.col(accent, alpha), D.L(xf, 1.1))
        end
      elseif mk.kind == "next_checkpoint" then
        ui.circle_filled(px, py, r * 1.2, D.col(accent, 0.25 * alpha))
        ui.circle(px, py, r * 1.2, D.col(text, alpha), D.L(xf, 1.4))
        D.icon(ui, "map/next_checkpoint", px, py, r * 2.4, text, alpha)
      elseif mk.kind == "finish" then
        if not D.icon(ui, "map/finish", px, py, r * 2.6, text, 0.95 * alpha) then
          ui.rect_filled(px - r, py - r, 2 * r, 2 * r, D.rgba(1, 1, 1, 0.9 * alpha), D.L(xf, 1.0))
          ui.rect_filled(px - r * 0.5, py - r * 0.5, r, r, D.rgba(0, 0, 0, 0.9 * alpha))
        end
      elseif mk.kind == "start" then
        if not D.icon(ui, "map/start", px, py, r * 2.6, { 0.45, 1.0, 0.55, 1.0 }, 0.95 * alpha) then
          ui.circle_filled(px, py, r * 0.8, D.rgba(0.45, 1.0, 0.55, 0.95 * alpha))
        end
      elseif mk.kind == "place" then
        ui.rect_filled(px - r, py - r, 2 * r, 2 * r, D.rgba(0.2, 0.45, 0.95, 0.9 * alpha), D.L(xf, 1.2))
        if not D.icon(ui, "map/place", px, py, r * 1.6, { 1, 1, 1, 1 }, alpha) then
          ui.text("", "P", px, py - r * 0.8, r * 1.8, D.rgba(1, 1, 1, alpha), "center", "top")
        end
      end
    end
  end
  ui.clip_pop()

  -- Player arrow at the centre (heading-up, so it always points up).
  local sc = D.L(xf, 1.0)
  local tipx, tipy = cx, cy - 6.0 * sc
  local blx, bly = cx - 4.2 * sc, cy + 4.4 * sc
  local brx, bry = cx + 4.2 * sc, cy + 4.4 * sc
  ui.polygon_filled({ tipx, tipy, blx, bly, brx, bry }, D.col(text, op))
  ui.polyline({ tipx, tipy, blx, bly, brx, bry }, D.col(accent, op), D.L(xf, 0.8), true)
end

return M

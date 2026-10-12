-- Full-screen world map (port of src/WorldMapUI.cpp, reduced: see scripts/ui/README.md "Map limits").
-- Pan (keys / stick / drag), zoom (wheel about the cursor, triggers, keys), recentre, event selection
-- (click, cycle), waypoint on the selected event (Enter / button) or on the nearest road of a click
-- (nox.roads:nearest), clear (right click / button), close.
--
-- Contract: local act = worldmap.frame(ctx, input, dt) from a system; worldmap.draw(ui, dt) from a
-- draw handler. Presentation only: the session applies the returned actions to race state.

local D = require("ui.draw")
local config = require("ui.config")
local roads = require("ui.roads")

local M = {}

local TOLERANCE_VISIBLE_M = 12000.0   -- start zoom: about this many metres high
local MIN_VISIBLE_M = 500.0           -- closest zoom
local REF_H = 1080.0

local view_state = {
  open = false, init = false, centre = { 0.0, 0.0 }, ppm = 0.05,
  selected = 1, hovered = nil, dragging = false,
  press_view = false, press_marker = nil, press_button = nil, prev = nil,
  hover_button = nil, pressed_button = nil,
  size = { 0, 0 }, time = 0.0,
  regions = { markers = {}, buttons = {}, view = nil },
}

-- Button ids.
local BTN_WAYPOINT, BTN_CENTER, BTN_BACK, BTN_CLEAR = 1, 2, 3, 4

function M.is_open() return view_state.open end

-- ctx.selected_event is 1-based (the session's index + 1).
function M.open(ctx)
  view_state.open = true
  view_state.init = false
  view_state.selected = math.max(1, math.min(ctx.selected_event or 1, math.max(1, #(ctx.events or {}))))
  view_state.hovered = nil
  view_state.dragging = false
  view_state.press_view, view_state.press_marker, view_state.press_button = false, nil, nil
  view_state.hover_button, view_state.pressed_button = nil, nil
  view_state.centre = { ctx.player.x, ctx.player.z }
end

function M.close() view_state.open = false end

-- Map bounds in metres: the network extent (or the player's neighbourhood when there is none).
local function map_bounds(ctx)
  local b = ctx.roads and roads.bounds(ctx.roads)
  if b then return b end
  return { ctx.player.x - 1000, ctx.player.z - 1000, ctx.player.x + 1000, ctx.player.z + 1000 }
end

local function scale_for(h)
  return math.max(0.6, math.min(2.2, h / REF_H))
end

local function default_ppm(w, h, b)
  local by_height = h / TOLERANCE_VISIBLE_M
  local fit = math.min(w / math.max(1.0, b[3] - b[1]), h / math.max(1.0, b[4] - b[2])) * 0.95
  local max_ppm = h / MIN_VISIBLE_M
  local v = math.min(by_height, fit)
  return D.clamp(v, h / (TOLERANCE_VISIBLE_M * 2.0), max_ppm)
end

local function world_to_screen(wx, wz, w, h)
  local c = view_state.centre
  return w * 0.5 + (wx - c[1]) * view_state.ppm, h * 0.5 + (wz - c[2]) * view_state.ppm
end

local function screen_to_world(sx, sy, w, h)
  local c = view_state.centre
  return c[1] + (sx - w * 0.5) / view_state.ppm, c[2] + (sy - h * 0.5) / view_state.ppm
end

local function zoom_about(steps, ox, oy, w, h, b)
  local ppm = view_state.ppm
  local ax, az = screen_to_world(w * 0.5 + ox, h * 0.5 + oy, w, h)
  local min_ppm = h / (TOLERANCE_VISIBLE_M * 2.0)
  local fit = math.min(w / math.max(1.0, b[3] - b[1]), h / math.max(1.0, b[4] - b[2])) * 0.95
  local max_ppm = h / MIN_VISIBLE_M
  local v = ppm * (1.2 ^ steps)
  view_state.ppm = D.clamp(v, math.min(min_ppm, fit), max_ppm)
  -- Keep the anchor under the cursor.
  view_state.centre[1] = ax - (w * 0.5 + ox - w * 0.5) / view_state.ppm
  view_state.centre[2] = az - (h * 0.5 + oy - h * 0.5) / view_state.ppm
end

local function init_view(ctx, w, h)
  if view_state.init then return end
  view_state.init = true
  view_state.ppm = default_ppm(w, h, map_bounds(ctx))
  view_state.centre = { ctx.player.x, ctx.player.z }
end

-- Marker positions in overlay coordinates for the current view.
local function marker_screens(ctx, w, h)
  local out = {}
  for i, ev in ipairs(ctx.events or {}) do
    local sx, sy = world_to_screen(ev.x, ev.z, w, h)
    out[i] = { sx, sy }
  end
  return out
end

-- Step: input = { view = {x,y,w,h} window coords, pointer = {x,y}|nil (window), pressed, released,
-- down, moved, wheel, pan = {x,y} (-1..1), zoom_rate (-1..1), zoom_steps, recenter, confirm, close,
-- cycle (-1|0|1) }. Returns { close, toggle_waypoint = idx|nil (1-based), selected = idx|nil,
-- place_waypoint = {x, z}|nil (nearest road point, when snapped), clear_waypoint }.
function M.frame(ctx, input, dt)
  local act = { close = false }
  if not view_state.open then return act end
  view_state.ctx = ctx
  local v = input.view
  local w, h = v.w, v.h
  if w < 2 or h < 2 then
    if input.close then view_state.open = false; act.close = true end
    return act
  end
  view_state.size = { w, h }
  local b = map_bounds(ctx)
  init_view(ctx, w, h)
  local n = #(ctx.events or {})
  local s = scale_for(h)

  -- Pan (keys / stick): a fixed fraction of the visible height per second.
  local px, py = input.pan and input.pan.x or 0.0, input.pan and input.pan.y or 0.0
  local len = math.sqrt(px * px + py * py)
  if len > 0 then
    if len > 1 then px, py = px / len, py / len end
    view_state.centre[1] = view_state.centre[1] + px * (0.9 * h / view_state.ppm) * dt
    view_state.centre[2] = view_state.centre[2] + py * (0.9 * h / view_state.ppm) * dt
  end
  local steps = (input.zoom_steps or 0.0) + (input.zoom_rate or 0.0) * dt * 2.5
  if math.abs(steps) > 1e-6 then zoom_about(steps, 0, 0, w, h, b) end

  local recenter = input.recenter
  local close = input.close
  local toggle = input.confirm
  local clear = false
  local place = nil
  local selected_click = nil

  -- Pointer (overlay coordinates; regions from the last draw).
  view_state.hovered = nil
  view_state.hover_button = nil
  local p = input.pointer and { x = input.pointer.x - v.x, y = input.pointer.y - v.y } or nil
  if p then
    local regions = view_state.regions
    if input.wheel and input.wheel ~= 0 then
      zoom_about(input.wheel > 0 and 1 or -1, p.x - w * 0.5, p.y - h * 0.5, w, h, b)
    end
    local markers = marker_screens(ctx, w, h)
    local best, bestd = nil, (18.0 * s) ^ 2
    for i, m in ipairs(markers) do
      local dx, dy = p.x - m[1], p.y - m[2]
      local d2 = dx * dx + dy * dy
      if d2 <= bestd then best, bestd = i, d2 end
    end
    view_state.hovered = best
    for k, r in pairs(regions.buttons) do
      if p.x >= r[1] and p.x <= r[3] and p.y >= r[2] and p.y <= r[4] then view_state.hover_button = k end
    end
    local in_view = regions.view and p.x >= 0 and p.y >= 0 and p.x <= w and p.y <= h

    if input.pressed then
      view_state.press_marker = best
      view_state.press_button = view_state.hover_button
      view_state.press_view = in_view and best == nil and view_state.hover_button == nil
      view_state.prev = { p.x, p.y }
      view_state.dragging = false
    end
    -- Drag pans when the press started on the map or a marker (never from a button or outside).
    if input.down and view_state.prev and (view_state.press_view or view_state.press_marker) then
      local dx, dy = p.x - view_state.prev[1], p.y - view_state.prev[2]
      if view_state.dragging or math.abs(dx) + math.abs(dy) > 4 then
        view_state.dragging = true
        view_state.centre[1] = view_state.centre[1] - dx / view_state.ppm
        view_state.centre[2] = view_state.centre[2] - dy / view_state.ppm
      end
      view_state.prev = { p.x, p.y }
    end
    if input.released then
      if not view_state.dragging then
        if view_state.press_marker and view_state.press_marker == best then
          view_state.selected = best
          selected_click = best
        elseif view_state.press_view and best == nil and view_state.hover_button == nil then
          local wx, wz = screen_to_world(p.x, p.y, w, h)
          local snap = 40.0 * s / view_state.ppm
          if ctx.roads then
            local ok, nx, nz, dist = pcall(function()
              local x, z, d = ctx.roads:nearest(wx, wz)
              return x, z, d
            end)
            if ok and nx and dist and dist <= snap then place = { x = nx, z = nz } end
          end
        end
        if view_state.press_button and view_state.press_button == view_state.hover_button then
          if view_state.press_button == BTN_WAYPOINT then toggle = true
          elseif view_state.press_button == BTN_CENTER then recenter = true
          elseif view_state.press_button == BTN_BACK then close = true
          elseif view_state.press_button == BTN_CLEAR then clear = true end
        end
      end
      view_state.press_view, view_state.press_marker, view_state.press_button = false, nil, nil
      view_state.dragging = false
    end
    if input.right_clicked then
      clear = true
    end
    view_state.pressed_button = (input.down and view_state.press_button) or nil
  else
    view_state.press_view, view_state.press_marker, view_state.press_button = false, nil, nil
    view_state.dragging = false
  end

  if recenter then view_state.centre = { ctx.player.x, ctx.player.z } end
  if n > 0 and (input.cycle or 0) ~= 0 then
    view_state.selected = ((view_state.selected - 1 + input.cycle) % n) + 1
  end
  if n > 0 then view_state.selected = math.max(1, math.min(view_state.selected, n)) end

  if toggle and n > 0 then act.toggle_waypoint = view_state.selected end
  if selected_click then act.selected = selected_click end
  if place then act.place_waypoint = place end
  if clear then act.clear_waypoint = true end
  if close then
    view_state.open = false
    act.close = true
  end
  return act
end

-- ---- Drawing -------------------------------------------------------------------------------------

local function button(ui, id, x0, y0, x1, y1, s)
  view_state.regions.buttons[id] = { x0, y0, x1, y1 }
  if view_state.pressed_button == id then
    ui.rect_filled(x0, y0, x1 - x0, y1 - y0, D.rgba(206 / 255, 160 / 255, 1, 120 / 255), 4.0 * s)
  elseif view_state.hover_button == id then
    ui.rect_filled(x0, y0, x1 - x0, y1 - y0, D.rgba(206 / 255, 160 / 255, 1, 56 / 255), 4.0 * s)
  end
  if view_state.hover_button == id or view_state.pressed_button == id then
    ui.rect(x0, y0, x1 - x0, y1 - y0, D.rgba(230 / 255, 210 / 255, 1, 220 / 255), 1.2 * s, 4.0 * s)
  end
end

local function key_cap(ui, font, px, x, y, key, s)
  local tw = ui.text_size(font, key, px) or 0
  local w = math.max(tw + 14 * s, 15 * s * 1.55)
  local h = 15 * s * 1.55
  ui.rect_filled(x, y - h * 0.5, w, h, D.rgba(0.2, 0.22, 0.26, 0.9), 3 * s)
  ui.text(font, key, x + w * 0.5, y, px, D.rgba(0.96, 0.97, 0.99, 1), "center", "middle")
  return w
end

local function draw_player(ui, x, y, heading, s)
  local fx, fy = math.sin(heading), math.cos(heading)   -- screen: x east, y south
  local px, py = -fy, fx
  ui.circle_filled(x, y, 20 * s, D.rgba(120 / 255, 230 / 255, 1, 40 / 255))
  local tipx, tipy = x + fx * 13 * s, y + fy * 13 * s
  local ax, ay = x - fx * 6.5 * s + px * 8.5 * s, y - fy * 6.5 * s + py * 8.5 * s
  local bx, by = x - fx * 6.5 * s - px * 8.5 * s, y - fy * 6.5 * s - py * 8.5 * s
  ui.polygon_filled({ tipx, tipy, ax, ay, bx, by }, D.rgba(245 / 255, 252 / 255, 1, 1))
  ui.polyline({ tipx, tipy, ax, ay, bx, by }, D.rgba(120 / 255, 230 / 255, 1, 1), 1.2 * s, true)
end

local function draw_event(ui, x, y, s, selected, hovered)
  local r = 13 * s
  ui.circle_filled(x, y, r + 6 * s, D.rgba(170 / 255, 100 / 255, 1, 46 / 255))
  ui.circle_filled(x, y, r, D.rgba(64 / 255, 26 / 255, 108 / 255, 240 / 255))
  ui.circle(x, y, r, D.rgba(200 / 255, 150 / 255, 1, 1), 2.0 * s)
  if selected or hovered then
    ui.circle(x, y, r + 6 * s, selected and D.rgba(1, 1, 1, 235 / 255) or D.rgba(1, 1, 1, 120 / 255), 1.4 * s)
  end
  local icon = config.icon("event/SPRINT RACE")
  if not (icon and D.icon_id(ui, icon, x, y, 17 * s, { 1, 1, 1, 1 }, 1.0)) then
    ui.line(x - 4 * s, y - 7 * s, x - 4 * s, y + 7 * s, D.rgba(1, 1, 1, 1), 1.6 * s)
    ui.polygon_filled({ x - 4 * s, y - 7 * s, x + 6 * s, y - 3 * s, x - 4 * s, y + 1 * s }, D.rgba(1, 1, 1, 1))
  end
end

local function draw_parking(ui, x, y, s, font)
  local r = 12 * s
  ui.circle_filled(x, y, r + 5 * s, D.rgba(40 / 255, 120 / 255, 1, 40 / 255))
  ui.circle_filled(x, y, r, D.rgba(30 / 255, 110 / 255, 240 / 255, 1))
  ui.circle(x, y, r, D.rgba(200 / 255, 225 / 255, 1, 230 / 255), 1.5 * s)
  ui.text(font, "P", x, y, 17 * s, D.rgba(1, 1, 1, 1), "center", "middle")
end

function M.draw(ui, dt)
  if not view_state.open then return end
  local ctx = view_state.ctx
  if not ctx then return end
  local w, h = ui.viewport()
  if w < 2 or h < 2 then return end
  local s = scale_for(h)
  local cfg = config.get()
  local font = cfg.fonts.label
  view_state.time = view_state.time + (dt or 0.0)
  view_state.regions.buttons = {}
  view_state.regions.view = { 0, 0, w, h }
  init_view(ctx, w, h)

  ui.clip_push(0, 0, w, h)
  ui.rect_filled(0, 0, w, h, D.rgba(21 / 255, 25 / 255, 31 / 255, 1))
  -- Faint 1 km grid.
  local step = 1000.0 * view_state.ppm
  if step >= 12.0 then
    local tlx, tlz = screen_to_world(0, 0, w, h)
    local brx, brz = screen_to_world(w, h, w, h)
    local x0 = math.floor(tlx / 1000.0) * 1000.0
    local z0 = math.floor(tlz / 1000.0) * 1000.0
    local line = D.rgba(120 / 255, 150 / 255, 170 / 255, 14 / 255)
    for gx = x0, brx, 1000.0 do
      local sx = world_to_screen(gx, 0, w, h)
      ui.line(sx, 0, sx, h, line, 1.0)
    end
    for gz = z0, brz, 1000.0 do
      local _, sy = world_to_screen(0, gz, w, h)
      ui.line(0, sy, w, sy, line, 1.0)
    end
  end

  -- Carriageways (no metadata: every edge is a generic street).
  local minX, minZ = screen_to_world(0, 0, w, h)
  local maxX, maxZ = screen_to_world(w, h, w, h)
  for _, line in ipairs(roads.lines(ctx.roads)) do
    if line.maxx >= minX and line.minx <= maxX and line.maxz >= minZ and line.minz <= maxZ then
      local pts = {}
      for i = 1, #line.pts - 1, 2 do
        local sx, sy = world_to_screen(line.pts[i], line.pts[i + 1], w, h)
        pts[#pts + 1] = sx
        pts[#pts + 1] = sy
      end
      ui.polyline(pts, D.rgba(150 / 255, 170 / 255, 190 / 255, 120 / 255), 1.0 * s, false)
    end
  end

  local function screen_path(list)
    local pts = {}
    for _, p in ipairs(list) do
      local sx, sy = world_to_screen(p.x, p.z, w, h)
      pts[#pts + 1] = sx
      pts[#pts + 1] = sy
    end
    return pts
  end

  -- Route preview of the displayed event (hovered, else selected).
  local preview = ctx.preview_route
  if preview and #preview >= 2 and preview ~= ctx.route then
    local pts = screen_path(preview)
    ui.polyline(pts, D.rgba(60 / 255, 210 / 255, 1, 40 / 255), 22 * s, false)
    ui.polyline(pts, D.rgba(70 / 255, 215 / 255, 1, 90 / 255), 12 * s, false)
    ui.polyline(pts, D.rgba(90 / 255, 225 / 255, 1, 230 / 255), 5.5 * s, false)
    ui.polyline(pts, D.rgba(200 / 255, 250 / 255, 1, 1), 2.2 * s, false)
  end

  -- Active guidance route.
  if ctx.route and #ctx.route >= 2 then
    local pts = screen_path(ctx.route)
    ui.polyline(pts, D.rgba(80 / 255, 220 / 255, 1, 60 / 255), 16 * s, false)
    ui.polyline(pts, D.rgba(80 / 255, 220 / 255, 1, 150 / 255), 7 * s, false)
    ui.polyline(pts, D.rgba(160 / 255, 248 / 255, 1, 1), 2.8 * s, false)
  end

  -- Waypoint diamond, parking, events, player.
  if ctx.waypoint then
    local qx, qy = world_to_screen(ctx.waypoint.x, ctx.waypoint.z, w, h)
    local r = 11 * s
    local pts = { qx, qy - r, qx + r, qy, qx, qy + r, qx - r, qy }
    ui.polygon_filled(pts, D.rgba(120 / 255, 240 / 255, 1, 230 / 255))
    ui.polyline(pts, D.rgba(1, 1, 1, 1), 1.5 * s, true)
  end
  for _, pl in ipairs(ctx.places or {}) do
    local qx, qy = world_to_screen(pl.x, pl.z, w, h)
    if qx >= -40 and qx <= w + 40 and qy >= -40 and qy <= h + 40 then draw_parking(ui, qx, qy, s, font) end
  end
  local screens = marker_screens(ctx, w, h)
  for i, m in ipairs(screens) do
    local qx, qy = m[1], m[2]
    draw_event(ui, qx, qy, s, i == view_state.selected, i == view_state.hovered)
  end
  do
    local qx, qy = world_to_screen(ctx.player.x, ctx.player.z, w, h)
    draw_player(ui, qx, qy, ctx.heading or 0.0, s)
  end

  -- Title.
  local left, top = 48 * s, 40 * s
  ui.text(font, "WORLD MAP", left, top, 34 * s, D.rgba(245 / 255, 248 / 255, 252 / 255, 1), "left", "top")

  -- Event panel (right): the displayed event, else the selected one.
  local di = view_state.hovered or view_state.selected
  local ev = ctx.events and ctx.events[di]
  if ev then
    local px = w * 0.7875
    local py = h * 0.4542 - 9 * s
    ui.text(font, ev.type or "", px, py, 30 * s, D.col(cfg.colors.lavender, 1.0), "left", "top")
    ui.text(font, ev.title or "", px, py + 36 * s, 28 * s, D.rgba(246 / 255, 248 / 255, 252 / 255, 1), "left", "top")
    local dx, dz = ev.x - ctx.player.x, ev.z - ctx.player.z
    local dist = math.sqrt(dx * dx + dz * dz)
    ui.text(font, string.format("%.1f km away", dist / 1000.0), px, py + 78 * s, 18 * s, D.rgba(206 / 255, 218 / 255, 232 / 255, 250 / 255), "left", "top")
    local row_y = py + 120 * s
    local key = "Enter"
    local label = ctx.has_waypoint and "Clear waypoint" or "Set waypoint"
    local kw = key_cap(ui, font, 15 * s, px, row_y + 9 * s, key, s)
    ui.text(font, label, px + kw + 12 * s, row_y + 9 * s, 17 * s, D.rgba(240 / 255, 244 / 255, 248 / 255, 245 / 255), "left", "middle")
    local tw = ui.text_size(font, label, 17 * s) or 0
    button(ui, BTN_WAYPOINT, px - 6 * s, row_y - 4 * s, px + kw + 12 * s + tw + 8 * s, row_y + 24 * s, s)
  end

  -- Legend (bottom-left).
  do
    local x = 48 * s
    local ly = h - 80 * s
    ui.circle_filled(x, ly, 8 * s, D.rgba(120 / 255, 230 / 255, 1, 1))
    ui.text(font, "Player", x + 32 * s, ly, 18 * s, D.rgba(235 / 255, 240 / 255, 246 / 255, 240 / 255), "left", "middle")
    ly = ly + 28 * s
    ui.circle_filled(x, ly, 8 * s, D.rgba(170 / 255, 100 / 255, 1, 1))
    ui.text(font, "Race", x + 36 * s, ly, 18 * s, D.rgba(235 / 255, 240 / 255, 246 / 255, 240 / 255), "left", "middle")
    ly = ly + 28 * s
    ui.circle_filled(x, ly, 8 * s, D.rgba(30 / 255, 110 / 255, 240 / 255, 1))
    ui.text(font, "Parking", x + 36 * s, ly, 18 * s, D.rgba(235 / 255, 240 / 255, 246 / 255, 240 / 255), "left", "middle")
  end

  -- Controls (bottom-right): key captions as the native map shows them; the buttons are the Center,
  -- Back and Clear actions (clicking the caption does the same as the key).
  do
    local items = {
      { key = "WASD", label = "Pan" },
      { key = "SCROLL", label = "Zoom" },
      { key = "F", label = "Center", button = BTN_CENTER },
      { key = "ESC", label = "Back", button = BTN_BACK },
    }
    if ctx.has_waypoint then items[#items + 1] = { key = "RMB", label = "Clear", button = BTN_CLEAR } end
    local total = 0
    for _, it in ipairs(items) do
      local tw = ui.text_size(font, it.label, 17 * s) or 0
      local kw = math.max((ui.text_size(font, it.key, 15 * s) or 0) + 14 * s, 15 * s * 1.55)
      it.kw, it.tw = kw, tw
      it.w = kw + 8 * s + tw
      total = total + it.w + 26 * s
    end
    local x = w - 48 * s - total + 26 * s
    local y = h - 40 * s
    for _, it in ipairs(items) do
      local kw = key_cap(ui, font, 15 * s, x, y, it.key, s)
      ui.text(font, it.label, x + kw + 8 * s, y, 17 * s, D.rgba(235 / 255, 240 / 255, 246 / 255, 240 / 255), "left", "middle")
      if it.button then
        button(ui, it.button, x - 6 * s, y - 17 * s, x + it.w + 6 * s, y + 12 * s, s)
      end
      x = x + it.w + 26 * s
    end
  end
  ui.clip_pop()
end

-- The session calls this every frame the map is open, so draw() has the context to draw.
function M.set_context(ctx) view_state.ctx = ctx end

-- Pointer feedback of the last step (for tests).
function M.hovered_event() return view_state.hovered end

return M

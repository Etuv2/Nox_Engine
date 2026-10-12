-- Presentation configuration for the racing HUD, minimap and world map.
--
-- Defaults are the values of the native HudConfig (src/RacingHUD.h). config/hud.json overrides them
-- through apply(decoded): the same keys and the same clamps as RacingHUD::LoadConfig. Reading the
-- file needs a JSON binding (see scripts/ui/README.md, request R1); until then the defaults apply.
-- Colours are arrays {r, g, b, a} of floats 0..1, as in hud.json.

local M = {}

local function defaults()
  return {
    placement = {
      cluster = { anchor = "BR", offset = { 0, 0 }, scale = 1 },
      minimap = { anchor = "BL", offset = { 0, 0 }, scale = 1 },
      event = { anchor = "TC", offset = { 0, 0 }, scale = 1 },
      panel = { anchor = "TR", offset = { 0, 0 }, scale = 1 },
      hint = { anchor = "TL", offset = { 0, 0 }, scale = 1 },
    },
    colors = {
      accent = { 0.35, 0.85, 1.0, 1.0 },   -- cyan markings, route, checkpoints
      needle = { 1.0, 0.15, 0.12, 1.0 },
      redline = { 1.0, 0.12, 0.10, 1.0 },
      speed = { 0.55, 0.80, 1.0, 1.0 },    -- digital speed panel
      lavender = { 0.80, 0.62, 1.0, 1.0 }, -- event title and flag
      text = { 1.0, 1.0, 1.0, 1.0 },
      backing_alpha = 0.35,
      glow = 0.6,
    },
    fonts = { label = "label", digital = "digital", text_scale = 1.0 },
    gauges = {
      rpm_max = 10000.0,
      redline = 8000.0,
      boost_label = "BOOST",
      boost_max = 1.0,
      n2o_label = "N2O",
      speed_units = "km/h",
    },
    minimap = {
      radius_m = 60.0,
      zoom_with_speed = 0.15,
      opacity = 0.75,
      road_px = 1.2,
      radius_px = 42.0,
    },
    -- Mirror of config/hud.json icons.roles: used until the JSON binding (R1) supplies the file.
    icon_roles = {
    ["menu/PAUSED"] = "menus/pause",
    ["menu/RESULTS"] = "events/trophy",
    ["gauge/boost"] = "hud/boost",
    ["gauge/n2o"] = "hud/nitrous",
    ["warning/WRONG WAY"] = "hud/wrong_way",
    ["warning/MISSED CHECKPOINT"] = "hud/missed_checkpoint",
    ["map/checkpoint"] = "navigation/checkpoint",
    ["map/next_checkpoint"] = "navigation/checkpoint",
    ["map/finish"] = "navigation/finish",
    ["map/start"] = "navigation/start",
    ["map/place"] = "navigation/parking",
    ["event/SPRINT RACE"] = "events/sprint",
    ["event/CIRCUIT"] = "events/circuit",
    ["event/DRIFT"] = "events/drift",
    ["event/TIME TRIAL"] = "events/time_trial",
    ["event/DRAG"] = "events/drag",
    ["event/SPEED TRAP"] = "events/speed_trap",
    ["result/finished"] = "events/trophy",
    ["result/record"] = "events/record",
    ["menu/Resume"] = "menus/play",
    ["menu/Restart Event"] = "menus/retry",
    ["menu/Retry"] = "menus/retry",
    ["menu/Quit Event"] = "menus/free_roam",
    ["menu/Free Roam"] = "menus/free_roam",
    ["menu/Reset Car"] = "menus/reset_car",
    ["menu/Exit Game"] = "menus/exit",
    ["key/Enter"] = "controls/enter",
    ["key/X"] = "controls/button_south",
    ["editor/save"] = "menus/save",
    ["editor/play"] = "menus/play",
    ["editor/pause"] = "menus/pause",
    },
  }
end

local current = defaults()
local raw = {}   -- the decoded document last applied (unknown keys survive a save)

local function num(v, fallback)
  if type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge then return v end
  return fallback
end

local function str(v, fallback)
  if type(v) == "string" then return v end
  return fallback
end

local function color(v, fallback)
  if type(v) ~= "table" then return fallback end
  local out = {}
  for i = 1, 4 do out[i] = num(v[i], fallback[i]) end
  return out
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- Merges a decoded config/hud.json document into the current config. Missing or mistyped keys keep
-- their value (the same tolerance as RacingHUD::LoadConfig). Returns the config table.
function M.apply(doc)
  local c = defaults()
  raw = type(doc) == "table" and doc or {}
  if type(doc) == "table" then
    local pl = doc.placement or {}
    for _, name in ipairs({ "cluster", "minimap", "event", "panel", "hint" }) do
      local o = pl[name]
      if type(o) == "table" then
        local p = c.placement[name]
        p.anchor = str(o.anchor, p.anchor)
        if type(o.offset) == "table" and #o.offset >= 2 then
          p.offset = { num(o.offset[1], 0), num(o.offset[2], 0) }
        end
        p.scale = num(o.scale, p.scale)
      end
    end
    local col = doc.colors or {}
    local cc = c.colors
    cc.accent = color(col.accent, cc.accent)
    cc.needle = color(col.needle, cc.needle)
    cc.redline = color(col.redline, cc.redline)
    cc.speed = color(col.speed, cc.speed)
    cc.lavender = color(col.lavender, cc.lavender)
    cc.text = color(col.text, cc.text)
    cc.backing_alpha = num(col.backing_alpha, cc.backing_alpha)
    cc.glow = num(col.glow, cc.glow)
    local fo = doc.fonts or {}
    c.fonts.text_scale = clamp(num(fo.text_scale, c.fonts.text_scale), 0.3, 3.0)
    local g = doc.gauges or {}
    local gg = c.gauges
    gg.rpm_max = clamp(num(g.rpm_max, gg.rpm_max), 1000.0, 30000.0)
    gg.redline = clamp(num(g.redline, gg.redline), 0.0, gg.rpm_max)
    gg.boost_label = str(g.boost_label, gg.boost_label)
    gg.boost_max = math.max(0.01, num(g.boost_max, gg.boost_max))
    gg.n2o_label = str(g.n2o_label, gg.n2o_label)
    gg.speed_units = str(g.speed_units, gg.speed_units)
    local mm = doc.minimap or {}
    local m = c.minimap
    m.radius_m = math.max(5.0, num(mm.radius_m, m.radius_m))
    m.zoom_with_speed = num(mm.zoom_with_speed, m.zoom_with_speed)
    m.opacity = num(mm.opacity, m.opacity)
    m.road_px = num(mm.road_px, m.road_px)
    m.radius_px = clamp(num(mm.radius_px, m.radius_px), 8.0, 200.0)
    if type(doc.icons) == "table" and type(doc.icons.roles) == "table" then
      for k, v in pairs(doc.icons.roles) do
        if type(v) == "string" then c.icon_roles[k] = v end
      end
    end
  end
  current = c
  return current
end

function M.get() return current end

local function copy(v)
  if type(v) ~= "table" then return v end
  local out = {}
  for k, x in pairs(v) do out[k] = copy(x) end
  return out
end

local function rgba(c)
  return { c[1], c[2], c[3], c[4] }
end

-- The config/hud.json document for the current values (the schema of RacingHUD::SaveConfig).
function M.document()
  local c = current
  local d = copy(raw)
  d.placement = {}
  for _, name in ipairs({ "cluster", "minimap", "event", "panel", "hint" }) do
    local p = c.placement[name]
    d.placement[name] = { anchor = p.anchor, offset = { p.offset[1], p.offset[2] }, scale = p.scale }
  end
  d.colors = d.colors or {}
  d.colors.accent = rgba(c.colors.accent)
  d.colors.needle = rgba(c.colors.needle)
  d.colors.redline = rgba(c.colors.redline)
  d.colors.speed = rgba(c.colors.speed)
  d.colors.lavender = rgba(c.colors.lavender)
  d.colors.text = rgba(c.colors.text)
  d.colors.backing_alpha = c.colors.backing_alpha
  d.colors.glow = c.colors.glow
  d.fonts = d.fonts or {}
  d.fonts.text_scale = c.fonts.text_scale
  d.gauges = {
    rpm_max = c.gauges.rpm_max, redline = c.gauges.redline, boost_label = c.gauges.boost_label,
    boost_max = c.gauges.boost_max, n2o_label = c.gauges.n2o_label, speed_units = c.gauges.speed_units,
  }
  d.minimap = {
    radius_m = c.minimap.radius_m, zoom_with_speed = c.minimap.zoom_with_speed, opacity = c.minimap.opacity,
    road_px = c.minimap.road_px, radius_px = c.minimap.radius_px,
  }
  return d
end

-- Icon id for a role, or nil. The icon is drawn with ui.image, which ignores unknown ids.
function M.icon(role)
  return current.icon_roles[role]
end

return M

-- HUD preview: feeds the fixture states (scripts/ui/fixtures.lua) through the presentation modules
-- without the race, so the HUD, minimap and map can be captured in comparable states.
--
-- Enabled only when the project declares it: project.json "presentation": { "preview": true }. The
-- entry script requires this module only in that case (the session wires it; see README). Each
-- fixture is shown for FRAMES_PER_FIXTURE frames in list order, so fixture k covers frames
-- [k*90, k*90+89] of the run (NOX_FINAL_DUMP_FRAMES picks the frames to capture).
--
-- The view is VIEW (overlay rectangle in window coordinates). It must match the window size of the
-- preview run; the draw handler uses ui.viewport() for everything else.

local fixtures = require("ui.fixtures")
local hud = require("ui.hud")
local worldmap = require("ui.worldmap")

local M = {}

local FRAMES_PER_FIXTURE = 90
local VIEW = { x = 0, y = 0, w = 1280, h = 720 }

local frame = 0
local last_logged = nil
local roads_handle = nil
local shown = nil   -- fixture of the last system step (the draw uses the same one)

local function current()
  local list = fixtures.list
  local idx = (frame // FRAMES_PER_FIXTURE) % #list + 1
  return idx, list[idx]
end

-- Road network for the fixtures: the name is the string declared in project.json "roads" (the preview
-- harness declares its copy of the sprint network there).
local ROADS_NAME = "data/sprint_roads.json"
local function roads()
  if roads_handle == nil then
    local ok, h = pcall(function() return nox.roads(ROADS_NAME) end)
    roads_handle = ok and h or false
  end
  return roads_handle or nil
end

nox.system {
  name = "hud_preview",
  order = 100,
  update = function(ctx, dt)
    local idx, fx = current()
    shown = fx
    if last_logged ~= idx then
      last_logged = idx
      print(string.format("[hud-preview] fixture %d %s frame %d", idx, fx.name, frame))
    end
    if fx.kind == "map" then
      local map = fx.map
      local mctx = {
        events = map.events, places = map.places, player = map.player, heading = map.heading,
        route = map.route, preview_route = map.preview_route, has_waypoint = map.has_waypoint,
        waypoint = map.waypoint, selected_event = map.selected_event, roads = roads(),
      }
      if not worldmap.is_open() then worldmap.open(mctx) end
      worldmap.frame(mctx, { view = VIEW, dt = dt }, dt)
    else
      if worldmap.is_open() then worldmap.close() end
      local state = fx.state
      state.roads = roads()
      hud.frame(state, { view = VIEW, selected = state.menu and state.menu.selected or 1 }, dt)
    end
    frame = frame + 1
  end,
}

nox.on_draw(function(ctx, ui, dt)
  local fx = shown
  if fx == nil then return end
  if fx.kind == "map" then
    worldmap.draw(ui, dt)
  else
    hud.draw(ui, dt)
  end
end)

return M

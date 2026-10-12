-- Fixture HUD states for the preview (scripts/ui/preview.lua) and the parity captures. Values follow
-- the nfs_sprint scene (event "sprint_legends_wheels": start [-2, 12], six checkpoints) and the native
-- capture states (free roam, near event, countdown, racing, results, pause, map).
--
-- Each fixture: { name, kind = "hud" | "map", state = <hud state table>, map = <map ctx> (kind map) }.
-- The state fields are the hud contract (scripts/ui/README.md).

local F = {}

local CHECKPOINTS = {
  { -3.8, 120.0 }, { -3.8, 270.0 }, { 75.0, 336.2 }, { 146.2, 270.0 }, { 146.2, 130.0 }, { 75.0, 63.8 },
}

local function base()
  return {
    speed_kmh = 0.0, rpm = 900.0, throttle = 0.0, load = 0.0, nitro = 1.0,
    nitro_active = false, nitro_recharging = false, gear = 1, redline = false, limiter = false,
    car = { x = -2.0, z = -40.0 }, car_y = 0.0, layered = false, heading = 0.0,
    roads = nil, route = {}, markers = {},
    phase = "free_roam",
    event_type = "SPRINT RACE", event_name = "A Legend's Wheels", event_difficulty = "Hard",
    start_key = "Enter", countdown = 0.0, race_time = 0.0, checkpoint = 0, checkpoint_count = 6,
    best_time = -1.0, best_holder = "You", last_time = -1.0, new_record = false, splits = {},
    message = "", message_alpha = 0.0,
    event_marker = nil, checkpoint_marker = nil,
    menu = nil, hint = "CHASE",
  }
end

local function event_marker(x, y, d)
  return { visible = true, x = x, y = y, distance = d }
end

local function menu(title, items, selected)
  return { title = title, items = items, selected = selected or 1 }
end

local function racing_markers(from)
  local out = {}
  for i = from, #CHECKPOINTS do
    local kind = (i == #CHECKPOINTS) and "finish" or ((i == from) and "next_checkpoint" or "checkpoint")
    out[#out + 1] = { x = CHECKPOINTS[i][1], z = CHECKPOINTS[i][2], kind = kind }
  end
  return out
end

-- Route polyline through the checkpoints (fixture only; the real route comes from nox.roads:route).
local function route_from(from, px, pz)
  local out = { { x = px, z = pz } }
  for i = from, #CHECKPOINTS do out[#out + 1] = { x = CHECKPOINTS[i][1], z = CHECKPOINTS[i][2] } end
  return out
end

local map_events = {
  { x = -2.0, z = 12.0, title = "A Legend's Wheels", type = "SPRINT RACE", has_end = true, end_x = 75.0, end_z = 63.8 },
  { x = 60.0, z = -60.0, title = "Palm Blvd Circuit", type = "CIRCUIT" },
}

F.list = {}

do
  local s = base()
  s.event_marker = event_marker(0.47, 0.44, 182.0)
  s.markers = { { x = -2.0, z = 12.0, kind = "event", label = "SPRINT RACE" }, { x = 60.0, z = -60.0, kind = "event", label = "CIRCUIT" } }
  s.speed_kmh = 0.0
  F.list[#F.list + 1] = { name = "free_roam", kind = "hud", state = s }
end

do
  local s = base()
  s.phase = "near_event"
  s.car = { x = -2.0, z = 0.0 }
  s.event_marker = event_marker(0.47, 0.44, 12.0)
  s.hint = "CHASE"
  F.list[#F.list + 1] = { name = "near_event", kind = "hud", state = s }
end

do
  local s = base()
  s.phase = "countdown"
  s.car = { x = -2.0, z = 12.0 }
  s.countdown = 2.4
  s.best_time = 50.26
  s.checkpoint_count = 6
  s.markers = racing_markers(1)
  s.route = route_from(1, -2.0, 12.0)
  F.list[#F.list + 1] = { name = "countdown", kind = "hud", state = s }
end

do
  local s = base()
  s.phase = "racing"
  s.car = { x = -3.8, z = 150.0 }
  s.heading = 0.0
  s.speed_kmh = 142.0
  s.rpm = 6200.0
  s.load = 0.6
  s.gear = 4
  s.nitro = 0.8
  s.throttle = 1.0
  s.race_time = 23.456
  s.checkpoint = 2
  s.checkpoint_count = 6
  s.best_time = 50.26
  s.splits = { -0.42, 1.10 }
  s.markers = racing_markers(2)
  s.route = route_from(2, -3.8, 150.0)
  s.checkpoint_marker = { visible = true, x = 0.5, y = 0.45, distance = 130.0 }
  s.hint = "CHASE"
  F.list[#F.list + 1] = { name = "racing", kind = "hud", state = s }
end

do
  local s = base()
  s.phase = "racing"
  s.car = { x = -3.8, z = 150.0 }
  s.speed_kmh = 150.0
  s.rpm = 8600.0
  s.redline = true
  s.gear = 4
  s.race_time = 24.1
  s.checkpoint = 2
  s.checkpoint_count = 6
  s.best_time = 50.26
  s.message = "WRONG WAY"
  s.message_alpha = 1.0
  s.markers = racing_markers(2)
  s.route = route_from(2, -3.8, 150.0)
  F.list[#F.list + 1] = { name = "racing_wrong_way", kind = "hud", state = s }
end

do
  local s = base()
  s.phase = "finished"
  s.car = { x = 75.0, z = 63.8 }
  s.race_time = 58.12
  s.last_time = 58.12
  s.best_time = 58.12
  s.new_record = true
  s.checkpoint = 6
  s.checkpoint_count = 6
  s.splits = { -0.42, 1.10, -0.9, 0.3, -2.1, 0.0 }
  s.menu = menu("RESULTS", { "Retry", "Free Roam" }, 1)
  F.list[#F.list + 1] = { name = "results", kind = "hud", state = s }
end

do
  local s = base()
  s.phase = "paused"
  s.car = { x = -2.0, z = -40.0 }
  s.menu = menu("PAUSED", { "Resume", "Reset Car", "Exit Game" }, 1)  -- free roam (native capture)
  F.list[#F.list + 1] = { name = "pause", kind = "hud", state = s }
end

-- World map: the event selection and the player's position.
F.map = {
  name = "world_map",
  kind = "map",
  events = map_events,
  places = {},
  player = { x = -2.0, z = -40.0 },
  heading = 0.0,
  route = nil,
  preview_route = nil,
  has_waypoint = false,
  waypoint = nil,
  selected_event = 1,
}

F.list[#F.list + 1] = { name = F.map.name, kind = "map", map = F.map, state = base() }

-- Checkpoint data for tests and the editor (same numbers as the scene).
F.checkpoints = CHECKPOINTS

return F

-- NOX Racing project entry (Lua game, API v1). Replaces the native GameSession for scenes that declare
-- "project": "projects/nox-racing/project.json". Systems run in declared order each frame:
--   race.input (10)    actions sampled once; autopilot or test driver replaces the drive input; pause,
--                      menus, map, camera mode, reset and start (native Tick order)
--   race.vehicle (20)  latched input to ctx.vehicle, the car step (frozen during countdown/menus/map)
--   race.session (30)  checkpoints, countdown, finish, route guidance, declared race state
--   race.camera (40)   chase camera (declared smoothing state on the body node)
--   race.effects (50)  tyre smoke/spray, audio parameters and cues
--   race.hud (60)      hud contract: hud.frame -> actions
-- Authoring: scripts/race/AUTHORING.md. Presentation: scripts/ui/README.md.
local session = require("race.session")
local geom = require("race.geom")
local input = require("input.actions")
local chase = require("camera.chase")
local fx = require("presentation.effects")
local autopilot = require("autopilot.autopilot")
local fixtures = require("autopilot.fixtures")
local hud = require("ui.hud")
local worldmap = require("ui.worldmap")
local config = require("ui.config")
-- Racing editor panels, viewport overlay, scene save hook (editor.model). Required once per project.
require("editor.register")
local editor = require("editor.model")

local VEHICLE_DEF = "projects/nox-racing/data/vehicles/r33.json"
local SAVE_NAME = "speedwall.json"
local TUNING_KEYS = {
    top_speed_kmh = true, drive_accel = true, brake_decel = true, reverse_max_kmh = true, mass = true,
    cg_height = true, tire_grip = true, tire_grip_long = true, rpm_idle = true, rpm_limiter = true,
    gear_ratios = true, suspension_freq = true, suspension_damping = true, bump_amplitude = true,
}

-- Declared state (survives a code reload). Race progress and the chase camera smoothing.
nox.schema("SessionRace", {
    phase = "integer", event = "integer", time = "number", countdown = "number", next_cp = "integer",
    beep = "integer", last_time = "number", new_record = "boolean", wrong_way = "number", paused = "boolean",
}, {
    phase = 0, event = -1, time = 0.0, countdown = 0.0, next_cp = 0, beep = -1, last_time = -1.0,
    new_record = false, wrong_way = 0.0, paused = false,
})
nox.schema("ChaseCamera", {
    init = "boolean", heading = "number", px = "number", py = "number", pz = "number",
    vx = "number", vy = "number", vz = "number", cx = "number", cy = "number", cz = "number",
    fov = "number", mode = "integer",
}, {
    init = false, heading = 0.0, px = 0.0, py = 0.0, pz = 0.0, vx = 0.0, vy = 0.0, vz = 0.0,
    cx = 0.0, cy = 0.0, cz = 0.0, fov = 54.0, mode = 0,
})

-- Per-VM frame state. Rebuilt on every load; the declared parts above carry the race progress.
local F = {
    ctx = nil, car = nil, car_failed = false, state_loaded = false, idle = false,
    drive = nil, btn = nil, tel = nil, teleported = false, frozen = false,
    cam_mode = 0, last_gear = nil, exit_after_finish = false, exit_env = nil,
    autopilot = false, autopilot_max = 150.0, fixture = nil, map_open = false,
    pending_start = nil, cue = nil,
}
local cfg = nil
local rig = nil

local function env(ctx, name)
    if type(ctx.env) ~= "function" then return nil end
    local ok, v = pcall(function() return ctx:env(name) end)
    if ok then return v end
    return nil
end

-- ---------------------------------------------------------------------------------------------
-- Load: configure from the scene's game object (nox.game, request R-S1) and the HUD config.
-- ---------------------------------------------------------------------------------------------

-- Called by nox.on_configure(game, scene) at every load of the world (scene load, Reconfigure, reload).
local function configure_game(game)
    if type(game) ~= "table" then
        F.idle = true
        session.say("[Race] the scene has no game object: the racing project is idle")
        return
    end
    local good, why = session.configure(game, nil)
    if not good then
        F.idle = true
        session.say("[Race] configure failed: " .. tostring(why))
        return
    end
    F.idle = false   -- a reload that now configures clears an earlier idle
    cfg = session.config()
    rig = chase.rig(cfg.camera)
    if nox.json then
        local okh, doc = pcall(nox.json, cfg.hud)
        config.apply(okh and doc or nil)
    end
end

-- Configure from the scene's game object, read once at load through nox.game(). nox.on_configure is not used
-- until the engine fix lands (registering it currently breaks Play). The editor's authored copy is
-- saved through nox.on_save_game (editor.model).
if type(nox.game) == "function" then
    local ok, game = pcall(nox.game)
    configure_game(ok and game or nil)
else
    F.idle = true
    session.say("[Race] nox.game is not available: the racing project is idle")
end
session.save_best = function(best)
    if F.ctx and F.ctx.save_user_json then
        local okv, reason = F.ctx:save_user_json(SAVE_NAME, best)
        if okv == false then session.say("[Race] best times not saved: " .. tostring(reason)) end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------------------------

local function create_car(ctx)
    local def = nox.json(VEHICLE_DEF)
    local hd, tn = {}, {}
    for k, v in pairs(def.controller.params.handling) do hd[k] = v end
    for k, v in pairs(def.tuning) do tn[k] = v end
    -- Scene handling overrides the controller handling and the shared tuning keys.
    for k, v in pairs(cfg.game.handling or {}) do
        hd[k] = v
        if TUNING_KEYS[k] then tn[k] = v end
    end
    local spawn = cfg.game.spawn or {}
    local sx, sz = def.spawn.x, def.spawn.z
    if type(spawn.position) == "table" then
        local x, z = geom.read_point(spawn.position)
        if x then sx, sz = x, z end
    end
    -- The wheel rig needs the hub positions (FL FR RL RR). r33.json has no measured rig yet: the hubs are
    -- derived from the axle and track geometry at the wheel rest height (the native default layout).
    -- Replace with measured values when the rig is measured (parity note in the session report).
    -- The data carries the measured hub positions of the car model (native DriveMode reads them from the glTF);
    -- the derived layout is only the fallback for a definition without them.
    local g = {}
    for k, v in pairs(def.geometry) do g[k] = v end
    if not g.wheel_rest_m then
        local ht = def.geometry.track_m * 0.5
        local fa, ra = def.geometry.front_axle_m, def.geometry.rear_axle_m
        local rest_y = def.geometry.wheel_radius_m[1]
        g.wheel_rest_m = {
            { ht, rest_y, fa }, { -ht, rest_y, fa }, { ht, rest_y, -ra }, { -ht, rest_y, -ra },
        }
    end
    F.top_kmh = (cfg.game.handling or {}).top_speed_kmh or def.tuning.top_speed_kmh
    F.track = def.geometry.track_m
    F.wheelbase = def.geometry.front_axle_m + def.geometry.rear_axle_m
    -- World inputs as native DriveMode::Configure gets them: the district layout obstacles (project data) and
    -- the scene's road network (recover to road, road edge queries).
    local world = { road_network = cfg.road_network }
    if def.world and def.world.obstacles then world.obstacles = def.world.obstacles end
    -- Static collision (stacked roads, tunnels, barriers): the scene's game.collision set (surface mode).
    if type(cfg.game.collision) == "table" and type(cfg.game.collision.files) == "table" then
        world.collision = cfg.game.collision
    end
    local h, why = ctx.vehicle.create({
        definition = VEHICLE_DEF,
        geometry = g,
        world = world,
        tuning = tn,
        spawn = { x = sx, z = sz, heading_deg = spawn.heading_deg or def.spawn.heading_deg },
        controller = {
            module = def.controller.module,
            instruction_budget = def.controller.instruction_budget,
            params = { handling = hd, policy = def.controller.params.policy },
        },
    })
    if not h then
        F.car_failed = true
        session.say("[Race] vehicle create failed: " .. tostring(why))
        return nil
    end
    return h
end

-- Telemetry fields the presentation and camera read that the ctx telemetry does not provide:
-- speed01 (native Speed01: speed over the handling top speed), ax (longitudinal acceleration from the
-- speed change), track and wheelbase (axles from the definition), teleported (set by the session).
local function normalize_telemetry(tel, dt)
    tel.speed01 = geom.clamp(tel.speed_kmh / (F.top_kmh or 250.0), 0.0, 1.0)
    local prev = F.prev_vx
    tel.ax = (prev and dt > 0.0) and (tel.vx - prev) / dt or 0.0
    F.prev_vx = tel.vx
    tel.track = F.track or 1.6
    tel.wheelbase = F.wheelbase or 2.72
    tel.teleported = F.teleported
end

-- The body node carries the declared ChaseCamera state.
local function body_handle(ctx)
    return ctx:find(cfg.body)
end

local function ensure_state(ctx)
    if F.state_loaded then return end
    local h = ctx:find(cfg.player)
    if not h then return end
    session.load_state(ctx, h)
    F.state_loaded = true
end

local function teleport_car(ctx, x, z, heading_deg)
    ctx.vehicle.teleport(F.car, x, z, heading_deg)
    F.teleported = true
    session.set_prev(x, z)
end

-- Creates the car once (race.input calls it first so the creation frame already has telemetry, as the native
-- game steps its car in the frame it is created). Pending start poses apply to the new car.
local function ensure_car(ctx, dt)
    if F.car or F.car_failed then return end
    F.car = create_car(ctx)
    if not F.car then return end
    if F.pending_start then
        local p = F.pending_start
        F.pending_start = nil
        teleport_car(ctx, p.x, p.z, p.heading_deg)
    end
    F.tel = ctx.vehicle.telemetry(F.car)
    normalize_telemetry(F.tel, dt)
end

-- Map context (scripts/ui/README.md, "Map context").
local function map_context(tel)
    local events = {}
    for i, e in ipairs(cfg.events) do
        events[i] = { x = e.x, z = e.z, title = e.name, type = e.type, has_end = false }
    end
    local places = {}
    for i, p in ipairs(cfg.places) do places[i] = { x = p.x, z = p.z } end
    local wp = session.waypoint()
    return {
        events = events, places = places,
        player = { x = tel.x, z = tel.z }, heading = geom.rad(tel.heading_deg),
        route = session.route(), preview_route = nil,
        has_waypoint = wp ~= nil, waypoint = wp,
        selected_event = session.selected(),
        roads = cfg.roads,
    }
end

-- ---------------------------------------------------------------------------------------------
-- Flow: pause, menus, map, camera mode, reset and start (native Tick order).
-- ---------------------------------------------------------------------------------------------

local function start_event_now(ctx, index)
    local out = {}
    session.start_event(index, out)
    if out.teleport then teleport_car(ctx, out.teleport.x, out.teleport.z, out.teleport.heading_deg) end
    if out.cue then F.cue = out.cue end
    autopilot.reset_for_event(out.teleport and out.teleport.y or 0.0)
end

local function reset_car_now(ctx)
    local t = session.reset_target(F.tel.x, F.tel.z, geom.rad(F.tel.heading_deg))
    teleport_car(ctx, t.x, t.z, geom.deg(t.heading))
end

local function activate_menu_item(ctx, item)
    if item == "Resume" then
        session.set_paused(false, false)
    elseif item == "Restart Event" or item == "Retry" then
        session.set_paused(false, false)
        start_event_now(ctx, session.active_index() or session.selected())
    elseif item == "Quit Event" or item == "Free Roam" then
        session.quit_event()
    elseif item == "Reset Car" then
        session.set_paused(false, false)
        reset_car_now(ctx)
    elseif item == "Exit Game" then
        ctx:request_exit()
    end
end

local function run_map(ctx, btn, dt)
    local mctx = map_context(F.tel)
    if not F.map_open then
        if btn.map then
            F.map_open = true
            worldmap.open(mctx)
        end
        return
    end
    local px, py = ctx:pointer_position()
    local mi = {
        view = { x = 0, y = 0, w = 640, h = 360 },
        pointer = px and { x = px, y = py } or nil,
        pressed = ctx:pointer_pressed("left"), released = ctx:pointer_released("left"),
        down = ctx:pointer_down("left"), moved = false, wheel = ctx:pointer_wheel(),
        right_clicked = ctx:pointer_pressed("right"),
        pan = { x = ctx:axis("map_pan_x"), y = ctx:axis("map_pan_y") },
        zoom_rate = ctx:axis("map_zoom_in") - ctx:axis("map_zoom_out"), zoom_steps = ctx:pointer_wheel(),
        recenter = btn.recenter, confirm = btn.confirm, close = btn.back or btn.map,
        cycle = ctx:action_pressed("map_cycle_next") and 1 or (ctx:action_pressed("map_cycle_prev") and -1 or 0),
    }
    local act = worldmap.frame(mctx, mi, dt)
    if act.close then
        F.map_open = false
        worldmap.close()
    end
    if act.selected then session.set_selected(act.selected) end
    if act.toggle_waypoint then
        local ev = cfg.events[act.toggle_waypoint]
        local wp = session.waypoint()
        if ev and wp and wp.x == ev.x and wp.z == ev.z then
            session.clear_waypoint()
            session.show_message("WAYPOINT CLEARED", 2.0)
        elseif ev then
            session.set_waypoint(ev.x, ev.z)
            session.show_message("WAYPOINT SET - " .. ev.name, 2.0)
        end
    end
    if act.place_waypoint then session.set_waypoint(act.place_waypoint.x, act.place_waypoint.z) end
    if act.clear_waypoint then session.clear_waypoint() end
end

-- Returns true when the car is frozen this frame (paused, countdown, menu open, map open).
local function flow(ctx, btn, dt)
    run_map(ctx, btn, dt)
    if F.map_open then return true end
    if btn.pause then
        if session.paused() then session.set_paused(false, false)
        elseif not session.menu() then session.set_paused(true, false) end
    end
    local menu_title = session.menu()
    local menu_open = menu_title ~= nil
    if menu_open then
        local item = session.menu_input(btn)
        if item then activate_menu_item(ctx, item) end
    else
        if btn.camera then F.cam_mode = (F.cam_mode + 1) % 3 end
        if btn.reset then reset_car_now(ctx) end
        if (btn.start and session.phase_name() == "free_roam" and session.near_event() >= 1) then
            start_event_now(ctx, session.near_event())
        end
    end
    local phase = session.state().phase
    return session.paused() or phase == session.COUNTDOWN or (menu_open and phase ~= session.FINISHED)
end

-- Each system runs under pcall: the first error of a system is logged (the engine would otherwise
-- disable it silently); the other systems keep running.
local reported = {}
local function system(t)
    local update = t.update
    t.update = function(ctx, dt)
        local ok, err = pcall(update, ctx, dt)
        if not ok and not reported[t.name] then
            reported[t.name] = true
            nox.log("[race] " .. t.name .. " error: " .. tostring(err))
        end
    end
    nox.system(t)
end

-- ---------------------------------------------------------------------------------------------
-- Editor bridge (R-S-E4, R-S-E6): publish the car and race progress to the editor panels, and apply the
-- editor's session requests. Requests: "play" and "start_event" (payload.index, 1-based event index, starts the
-- event), "teleport" (payload x, z, heading_deg). Runtime tuning (R-S-E5) is not applied yet.
-- ---------------------------------------------------------------------------------------------

local function editor_bridge(ctx, tel)
    editor.publish_car({
        x = tel.x, z = tel.z, heading_deg = tel.heading_deg, speed_kmh = tel.speed_kmh,
        gear = tel.gear, rpm = tel.rpm, nitro = tel.nitro,
    })
    local st = session.state()
    editor.publish_race({ phase_name = session.phase_name(), time = st.time, next_cp = st.next_cp })
    for _, req in ipairs(editor.take_requests()) do
        local p = req.payload or {}
        if req.kind == "play" or req.kind == "start_event" then
            if p.index and cfg.events[p.index] then
                session.set_selected(p.index)
                start_event_now(ctx, p.index)
            end
        elseif req.kind == "teleport" and p.x and p.z then
            teleport_car(ctx, p.x, p.z, p.heading_deg or tel.heading_deg)
        end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Systems
-- ---------------------------------------------------------------------------------------------

system {
    name = "race.input", order = 10,
    update = function(ctx, dt)
        if F.idle then return end
        F.ctx = ctx
        if F.exit_after_finish then
            session.say(autopilot.finish_line())
            ctx:request_exit()
            F.exit_after_finish = false
        end
        ensure_state(ctx)
        ensure_car(ctx, dt)
        -- Until the car exists there is no telemetry to act on.
        if not F.tel then
            F.drive, F.frozen = input.neutral(), true
            return
        end
        local drive, btn = input.sample(ctx)
        local teleport_req = nil
        if F.fixture and F.tel then
            -- Test drivers: the drive input is theirs and no buttons are pressed.
            local view = {
                x = F.tel.x, z = F.tel.z, y = F.tel.y, heading = geom.rad(F.tel.heading_deg),
                speed_kmh = F.tel.speed_kmh, speed_ms = F.tel.vx, tel = F.tel,
            }
            if F.fixture == "maneuvers" then drive = fixtures.maneuvers(view, dt)
            else drive = fixtures.gearbox(view, dt) end
            btn = { confirm = false, start = false, back = false, pause = false, reset = false,
                camera = false, map = false, recenter = false, up = false, down = false }
        elseif F.autopilot and F.tel and session.state() then
            local st = session.state()
            local view = {
                x = F.tel.x, z = F.tel.z, y = F.tel.y, heading = geom.rad(F.tel.heading_deg),
                speed_kmh = F.tel.speed_kmh, speed_ms = F.tel.vx, airborne = F.tel.airborne,
                route = session.route(), phase = st.phase, near = session.near_event(),
                selected = session.selected(), active = session.active_event(), next_cp = st.next_cp,
                events = cfg.events, roads = cfg.roads,
                finished_menu = session.menu() ~= nil and st.phase == session.FINISHED,
                menu_sel = select(3, session.menu()) or 1, max_kmh = F.autopilot_max,
            }
            drive, btn, teleport_req = autopilot.step(view, dt)
        end
        F.drive, F.btn = drive, btn
        if teleport_req then teleport_car(ctx, teleport_req.x, teleport_req.z, geom.deg(teleport_req.heading)) end
        F.frozen = flow(ctx, btn, dt)
        if F.cue then fx.cue(ctx, F.cue); F.cue = nil end
        session.store_state(ctx, body_handle(ctx))
    end,
}

system {
    name = "race.vehicle", order = 20,
    update = function(ctx, dt)
        if F.idle or F.car_failed then return end
        ensure_car(ctx, dt)
        if not F.car then return end
        local drive = F.drive or input.neutral()
        -- Finished: the car coasts to a stop (native: brake 0.6, no throttle).
        if session.state().phase == session.FINISHED then
            drive = { throttle = 0.0, brake = 0.6, steer = 0.0, handbrake = false, nitrous = false }
        end
        ctx.vehicle.set_input(F.car, {
            throttle = drive.throttle, brake = drive.brake, steer = drive.steer,
            handbrake = drive.handbrake, nitrous = drive.nitrous,
        })
        -- The service clamps the frame dt itself (0.05 s); the call accepts up to 1 s.
        if session.paused() or F.map_open then
            ctx.vehicle.step(0.0, true)
        else
            ctx.vehicle.step(math.min(dt, 1.0), F.frozen)
        end
        F.tel = ctx.vehicle.telemetry(F.car)
        normalize_telemetry(F.tel, dt)
    end,
}

system {
    name = "race.session", order = 30,
    update = function(ctx, dt)
        if F.idle or not F.tel or not F.car then return end
        local tel = F.tel
        local px, pz = tel.x, tel.z
        local prev_x, prev_z = session.prev()
        if prev_x == nil or tel.teleported then prev_x, prev_z = px, pz end
        local out = {}
        -- Native: no race update while paused or while the map is open.
        if not session.paused() and not F.map_open then
            session.update_race(dt, tel, px, pz, prev_x, prev_z, tel.y, out)
        end
        session.set_prev(px, pz)
        editor_bridge(ctx, tel)
        if out.cue then fx.cue(ctx, out.cue) end
        if out.finished and F.exit_env then F.exit_after_finish = true end
        F.teleported = false
    end,
}

system {
    name = "race.camera", order = 40,
    update = function(ctx, dt)
        if F.idle or not F.tel or not F.car then return end
        local h = body_handle(ctx)
        local st = (h and ctx:get(h, "ChaseCamera")) or chase.fresh()
        st.mode = F.cam_mode
        local tel = F.tel
        local teleported = tel.teleported
        local ex, ey, ez, tx, ty, tz, fov = chase.update(ctx, tel, dt, rig, st, nil, teleported, 0.0)
        chase.apply(ctx, ex, ey, ez, tx, ty, tz, fov, teleported)
        if h then ctx:add(h, "ChaseCamera", st) end
    end,
}

system {
    name = "race.effects", order = 50,
    update = function(ctx, dt)
        if F.idle or not F.tel or not F.car then return end
        local tel = F.tel
        local throttle = F.drive and F.drive.throttle or 0.0
        fx.tyres(ctx, tel, dt)
        local g = config.get()
        local rpm_max = (g and g.gauges and g.gauges.rpm_max) or 7800.0
        fx.audio(ctx, tel, throttle, session.paused(), rpm_max)
        fx.shift_cue(ctx, tel.gear, F.last_gear)
        F.last_gear = tel.gear
    end,
}

system {
    name = "race.hud", order = 60,
    update = function(ctx, dt)
        if F.idle or not F.tel then return end
        local hs = session.hud_state(F.tel, dt)
        local px, py = ctx:pointer_position()
        local hi = {
            view = { x = 0, y = 0, w = 640, h = 360 },
            pointer = px and { x = px, y = py } or nil,
            moved = false,
            pressed = ctx:pointer_pressed("left"), released = ctx:pointer_released("left"),
            down = ctx:pointer_down("left"), wheel = ctx:pointer_wheel(),
            selected = select(3, session.menu()) or 1,
        }
        local act = hud.frame(hs, hi, dt)
        if act.menu_select then session.set_menu_sel(act.menu_select) end
        if act.menu_wheel then session.step_menu(act.menu_wheel) end
        if act.menu_click then
            session.set_menu_sel(act.menu_click)
            local item = session.menu_activate_item(act.menu_click)
            if item then activate_menu_item(ctx, item) end
        end
        if act.start_click and session.phase_name() == "free_roam" and session.near_event() >= 1 then
            start_event_now(ctx, session.near_event())
        end
    end,
}

-- ---------------------------------------------------------------------------------------------
-- Draw and lifecycle
-- ---------------------------------------------------------------------------------------------

nox.on_draw(function(ctx, ui, dt)
    if F.idle then return end
    hud.draw(ui, dt)
    if F.map_open then worldmap.draw(ui, dt) end
end)

nox.on("begin_play", function(ctx, payload)
    F.ctx = ctx
    if F.idle then return end
    F.exit_env = env(ctx, "NOX_EXIT_ON_FINISH")
    F.autopilot = env(ctx, "NOX_DRIVE_AUTOPILOT") == "1"
    local script = env(ctx, "NOX_DRIVE_SCRIPT")
    F.fixture = (script == "maneuvers" or script == "gearbox") and script or nil
    local maxk = env(ctx, "NOX_AUTOPILOT_MAXKMH")
    F.autopilot_max = (maxk and tonumber(maxk)) or 150.0
    F.cam_mode = 0
    F.last_gear = nil
    -- Best times: the user save replaces the seed when present.
    local saved = ctx:load_user_json(SAVE_NAME)
    if type(saved) == "table" then session.set_best(saved) end
    session.quit_event()
    session.set_paused(false, false)
    local idx = tonumber(env(ctx, "NOX_AUTOPILOT_EVENT") or "")
    if idx and cfg.events[idx + 1] then
        session.set_selected(idx + 1)
        session.say(string.format("[Traversal] Event %d '%s'", idx, cfg.events[idx + 1].name))
        local out = {}
        session.start_event(idx + 1, out)
        F.pending_start = out.teleport
        autopilot.reset_for_event(out.teleport and out.teleport.y or 0.0)
    end
end)

nox.on("end_play", function(ctx, payload)
    if F.idle then return end
    ctx:audio_set({ active = false })
    ctx:fx_clear()
    session.quit_event()
    session.set_paused(false, false)
    F.car = nil
    F.tel = nil
    editor.publish_car(nil)
    editor.publish_race(nil)
    F.state_loaded = false
    F.map_open = false
end)

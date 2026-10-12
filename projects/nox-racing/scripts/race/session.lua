-- Race session: free roam, event start prompt, countdown, ordered checkpoints, finish and results,
-- pause and results menus, waypoint and route guidance, places, best times. Port of GameSession
-- (src/GameSession.cpp). Race progress lives in the declared schema SessionRace on the player node so it
-- survives a code reload; everything else here is rebuilt from the scene's "game" object at load.
--
-- The HUD and map are presentation (scripts/ui): this module builds their state and applies the actions
-- they return (docs/ui in scripts/ui/README.md).
local geom = require("race.geom")

local M = {}

-- Phase numbers are stored in the declared state; the names are the HUD contract strings.
M.FREE, M.COUNTDOWN, M.RACING, M.FINISHED = 0, 1, 2, 3
local PHASE_NAME = { [0] = "free_roam", "countdown", "racing", "finished" }
M.PHASE_NAME = PHASE_NAME

local DEFAULT_SPAWN = { x = -2.0, z = -40.0, heading_deg = 0.0 }
local SAVE_NAME = "speedwall.json"

-- Runtime (rebuilt at load, never declared).
local cfg = nil          -- configured authoring data (see AUTHORING.md)
local R = nil            -- declared race state (mirror of SessionRace)
local rt = {}            -- non-declared runtime: messages, route, menu, best times, edges

local function new_runtime()
    return {
        msg = "", msg_timer = 0.0,
        prev_x = nil, prev_z = nil,
        near = -1, at_place = -1, selected = 1,
        route = {}, route_timer = 0.0, route_keep = 0,
        splits = {}, gate_cache = {},
        menu_title = nil, menu_items = {}, menu_sel = 1,
        paused = false,
        waypoint = nil,
        best = {},
        last_gear = nil,
        incidents = 0,
        exit_requested = false,
    }
end

local function default_state()
    return { phase = M.FREE, event = -1, time = 0.0, countdown = 0.0, next_cp = 0, beep = -1,
        last_time = -1.0, new_record = false, wrong_way = 0.0, paused = false }
end

-- Logging: native text at the start of the line. nox.log_raw is requested (R-S5); fall back to nox.log.
local function say(line)
    if nox.log_raw then nox.log_raw(line) else nox.log(line) end
end
M.say = say

-- ---------------------------------------------------------------------------------------------
-- Configuration from the scene "game" object
-- ---------------------------------------------------------------------------------------------

local function read_events(list)
    local out = {}
    if type(list) ~= "table" then return out end
    for _, e in ipairs(list) do
        if type(e) == "table" then
            local sx, sz, sy, shas = geom.read_point(e.start)
            local ev = {
                id = e.id or "sprint_01",
                type = e.type or "SPRINT RACE",
                name = e.name or "A Legend's Wheels",
                difficulty = e.difficulty or "Hard",
                x = sx or 0.0, z = sz or 0.0, y = sy or 0.0, has_y = shas and true or false,
                heading_deg = e.start_heading_deg or 0.0,
                trigger = e.trigger_radius or 9.0,
                countdown = e.countdown or 3.0,
                checkpoints = {},
            }
            for _, c in ipairs(e.checkpoints or {}) do
                local x, z, y, hy = geom.read_point(c.pos or c)
                if x then
                    local cp = { x = x, z = z, y = y or 0.0, has_y = hy, width = (type(c) == "table" and c.width) or 18.0 }
                    if type(c) == "table" and c.heading_deg ~= nil then cp.heading_deg = c.heading_deg end
                    ev.checkpoints[#ev.checkpoints + 1] = cp
                end
            end
            if #ev.checkpoints == 0 then
                say("[Race] event '" .. ev.id .. "' has no checkpoints and is skipped")
            else
                out[#out + 1] = ev
            end
        end
    end
    return out
end

local function read_places(list)
    local out = {}
    if type(list) ~= "table" then return out end
    for _, p in ipairs(list) do
        local x, z, y, hy = geom.read_point(p.pos)
        if x then
            out[#out + 1] = { id = p.id or "", type = p.type or "PARKING AREA", name = p.name or "",
                x = x, z = z, y = y or 0.0, has_y = hy, radius = p.radius or 40.0 }
        end
    end
    return out
end

-- Configures the session from nox.game(). Returns false (with a reason) when the scene has no game
-- object or the road network cannot be read; the caller then idles.
function M.configure(game, saved_best)
    if type(game) ~= "table" then return false, "scene has no game object" end
    cfg = {
        game = game,
        player = game.player_node or "PlayerCar",
        body = game.body_node or "PlayerCarBody",
        spawn = game.spawn or DEFAULT_SPAWN,
        camera = game.camera or {},
        events = read_events(game.events),
        places = read_places(game.places),
        height_tol = (type(game.collision) == "table" and game.collision.height_tolerance) or 4.0,
        pause_on_focus_loss = game.pause_on_focus_loss ~= false,
        hud = game.hud or "config/hud.json",
        roads = nil,
    }
    -- Road network: inline in the scene, or a logical file (game.road_network.file). An inline table goes through
    -- nox.roads.from_table; a file is declared in project.json "roads" (as "../../" + the logical path, because the
    -- declared names resolve from the project folder) and loaded through nox.roads(name): nox.json caps JSON at 1 MiB,
    -- and the Shuto network is 17 MB. cfg.road_network is the scene spec itself; the vehicle world reads a file
    -- through AssetIO (RoadNetwork::Load), so the same spec serves both sides.
    cfg.road_network = nil
    if type(game.road_network) == "table" then
        local rn = game.road_network
        cfg.road_network = rn
        if type(rn.file) == "string" then
            if nox.roads then
                local ok, road = pcall(nox.roads, "../../" .. rn.file)
                if ok and road then cfg.roads = road end
                if not cfg.roads then say("[Race] road network '" .. rn.file .. "' is not declared in project.json (roads)") end
            end
        elseif nox.roads and nox.roads.from_table then
            cfg.roads = nox.roads.from_table(rn)
        else
            say("[Race] road_network present but nox.roads.from_table is not available (R-S2)")
        end
    end
    rt = new_runtime()
    R = default_state()
    -- Best times: the user save (passed in by main), else the seed copy of config/speedwall.json.
    local best = saved_best
    if type(best) ~= "table" and nox.json then
        local ok, seed = pcall(nox.json, "config/speedwall.json")
        if ok and type(seed) == "table" then best = seed end
    end
    rt.best = type(best) == "table" and best or {}
    return true
end

function M.config() return cfg end
function M.set_best(best) if type(best) == "table" then rt.best = best end end
function M.best_table() return rt.best end
function M.runtime() return rt end

-- Save hook: main sets M.save_best = function(best) ... end (it closes over the current ctx).
M.save_best = nil

-- ---------------------------------------------------------------------------------------------
-- Declared state
-- ---------------------------------------------------------------------------------------------

-- Stored declared state wins; without it the in-memory state (set by begin_play or the defaults) stays.
function M.load_state(ctx, h)
    local s = ctx:get(h, "SessionRace")
    if s then R = s end
    R = R or default_state()
    return R
end

function M.state() return R end

-- Writes the declared state back (called once at the end of the race system each frame).
function M.store_state(ctx, h)
    if R and h then ctx:add(h, "SessionRace", R) end
end

-- ---------------------------------------------------------------------------------------------
-- Gate geometry and routing (nox.roads)
-- ---------------------------------------------------------------------------------------------

local function gate_heading(ev, i)
    local cp = ev.checkpoints[i]
    if cp.heading_deg ~= nil then return geom.rad(cp.heading_deg) end
    local px, pz
    if i == 1 then px, pz = ev.x, ev.z else px, pz = ev.checkpoints[i - 1].x, ev.checkpoints[i - 1].z end
    local key = string.format("%.4f,%.4f,%.4f,%.4f", px, pz, cp.x, cp.z)
    if rt.gate_cache[key] then return rt.gate_cache[key] end
    local dx, dz = cp.x - px, cp.z - pz
    local roads = cfg.roads
    if roads then
        local path = roads:route(px, pz, cp.x, cp.z)
        if path and #path >= 2 then
            local a, b = path[#path - 1], path[#path]
            local ddx, ddz = b.x - a.x, b.z - a.z
            if math.sqrt(ddx * ddx + ddz * ddz) > 1e-3 then dx, dz = ddx, ddz end
        end
    end
    if math.sqrt(dx * dx + dz * dz) < 1e-3 then dx, dz = 0.0, 1.0 end
    local h = math.atan(dx, dz)
    rt.gate_cache[key] = h
    return h
end
M.gate_heading = gate_heading

-- Route polyline from (x, z) to (tx, tz), or {} without a network. Hysteresis: while the car stays on
-- the current line (within 4 m) toward the same target, trim the line instead of re-projecting.
local function refresh_route(dt, px, pz, tx, tz, finished)
    -- Float32 timer (native m_routeTimer): decides the refresh frame.
    rt.route_timer = geom.f32(rt.route_timer - dt)
    if rt.route_timer > 0.0 then return end
    rt.route_timer = geom.f32(0.2)
    if finished or not cfg.roads then rt.route = {}; return end
    local r = rt.route
    if #r >= 2 and math.sqrt((r[#r].x - tx) ^ 2 + (r[#r].z - tz) ^ 2) < 1.0 and rt.route_keep < 10 then
        local best, seg, cx, cz = 1e30, 1, r[1].x, r[1].z
        for i = 1, #r - 1 do
            local ax, az, bx, bz = r[i].x, r[i].z, r[i + 1].x, r[i + 1].z
            local abx, abz = bx - ax, bz - az
            local den = abx * abx + abz * abz
            local t = den > 1e-6 and geom.clamp(((px - ax) * abx + (pz - az) * abz) / den, 0.0, 1.0) or 0.0
            local qx, qz = ax + abx * t, az + abz * t
            local d = math.sqrt((px - qx) ^ 2 + (pz - qz) ^ 2)
            if d < best then best, seg, cx, cz = d, i, qx, qz end
        end
        if best < 4.0 then
            local trimmed = { { x = cx, z = cz } }
            for i = seg + 1, #r do trimmed[#trimmed + 1] = r[i] end
            if #trimmed >= 2 then
                rt.route = trimmed
                rt.route_keep = rt.route_keep + 1
                return
            end
        end
    end
    rt.route_keep = 0
    local path = cfg.roads:route(px, pz, tx, tz)
    rt.route = path or {}
end

local function route_target()
    if rt.waypoint and R.phase == M.FREE then return rt.waypoint.x, rt.waypoint.z end
    if rt.active and (R.phase == M.RACING or R.phase == M.COUNTDOWN) then
        local cp = rt.active.checkpoints[math.min(R.next_cp + 1, #rt.active.checkpoints)]
        return cp.x, cp.z
    end
    local ev = cfg.events[rt.selected]
    if ev then return ev.x, ev.z end
    return nil
end

-- ---------------------------------------------------------------------------------------------
-- Height gate (stacked roads). Needs the car height: nil telemetry height disables the gate.
-- ---------------------------------------------------------------------------------------------

local function at_height(has_y, y, car_y)
    if not has_y or car_y == nil then return true end
    return math.abs(car_y - y) <= cfg.height_tol
end

-- ---------------------------------------------------------------------------------------------
-- Messages, menus, best times
-- ---------------------------------------------------------------------------------------------

local function show_message(text, seconds)
    rt.msg = text
    rt.msg_timer = seconds or 2.0
end
M.show_message = show_message

local function best_of(id)
    local b = rt.best[id]
    if type(b) == "table" and type(b.time) == "number" then return b.time end
    return -1.0
end
M.best_time = best_of

local function set_menu(title, items)
    rt.menu_title = title
    rt.menu_items = items
    rt.menu_sel = 1
end

-- ---------------------------------------------------------------------------------------------
-- Event start, quit, reset, pause
-- ---------------------------------------------------------------------------------------------

-- Actions the owner (main) performs: teleport the car, play cues. Returned to keep this module free of
-- the ctx of the current callback.
function M.start_event(index, out)
    local ev = cfg.events[index]
    if not ev then return end
    rt.active = ev
    rt.active_index = index
    R.phase = M.COUNTDOWN
    R.event = index
    R.countdown = geom.f32(math.max(ev.countdown, 1.0))
    R.beep = -1
    R.time = 0.0
    R.next_cp = 0
    rt.splits = {}
    R.new_record = false
    R.wrong_way = 0.0
    rt.menu_title = nil
    rt.paused = false
    rt.selected = index
    out.teleport = { x = ev.x, z = ev.z, y = ev.has_y and ev.y or nil, heading_deg = ev.heading_deg }
    out.cue = "select"
    say(string.format("[Race] Event '%s' countdown", ev.name))
end

function M.quit_event()
    R.phase = M.FREE
    R.event = -1
    rt.active = nil
    rt.active_index = nil
    rt.menu_title = nil
    rt.paused = false
    rt.route = {}
end

-- Reset: back on the road at the last cleared gate (racing) or the nearest road point (free roam).
function M.reset_target(px, pz, heading)
    if R.phase == M.RACING and rt.active then
        local ev = rt.active
        local from_x, from_z, from_y, from_hy
        if R.next_cp == 0 then
            from_x, from_z, from_y, from_hy = ev.x, ev.z, ev.y, ev.has_y
        else
            local cp = ev.checkpoints[R.next_cp]
            from_x, from_z, from_y, from_hy = cp.x, cp.z, cp.y, cp.has_y
        end
        local heading_out = R.next_cp == 0 and geom.rad(ev.heading_deg) or gate_heading(ev, R.next_cp)
        return { x = from_x, z = from_z, y = from_hy and from_y or nil, heading = heading_out }
    end
    if cfg.roads then
        local x, z = cfg.roads:nearest(px, pz)
        if x then return { x = x, z = z, heading = heading } end
    end
    return { x = px, z = pz, heading = heading }
end

-- Pause: menu items follow the native list.
function M.set_paused(paused, standalone)
    if paused then
        rt.paused = true
        local items = { "Resume" }
        if R.phase == M.COUNTDOWN or R.phase == M.RACING then
            items[#items + 1] = "Restart Event"
            items[#items + 1] = "Quit Event"
        end
        items[#items + 1] = "Reset Car"
        if standalone then items[#items + 1] = "Exit Game" end
        set_menu("PAUSED", items)
    else
        rt.paused = false
        if rt.menu_title == "PAUSED" then rt.menu_title = nil end
    end
    R.paused = rt.paused
end

-- ---------------------------------------------------------------------------------------------
-- Free roam: places and event markers
-- ---------------------------------------------------------------------------------------------

local function free_roam(px, pz, speed_kmh, car_y, out)
    if rt.waypoint and math.sqrt((px - rt.waypoint.x) ^ 2 + (pz - rt.waypoint.z) ^ 2) < 30.0 then
        rt.waypoint = nil
        show_message("WAYPOINT REACHED", 2.5)
    end
    local at = -1
    for i, pl in ipairs(cfg.places) do
        if math.sqrt((px - pl.x) ^ 2 + (pz - pl.z) ^ 2) < pl.radius and at_height(pl.has_y, pl.y, car_y) then
            at = i - 1
            break
        end
    end
    if at ~= rt.at_place then
        rt.at_place = at
        if at >= 0 and speed_kmh < 80.0 then
            local pl = cfg.places[at + 1]
            local nearby = 0
            for _, e in ipairs(cfg.events) do
                if math.sqrt((e.x - pl.x) ^ 2 + (e.z - pl.z) ^ 2) < 1500.0 then nearby = nearby + 1 end
            end
            local text = pl.name .. " " .. pl.type .. " - REST / MEET / PHOTO"
            if nearby > 0 then text = text .. " - " .. nearby .. " EVENT(S) NEARBY" end
            show_message(text, 3.5)
            out.refuel = true
            say(string.format("[Place] Arrived at %s (%s)", pl.name, pl.id))
        end
    end
    rt.near = -1
    for i, e in ipairs(cfg.events) do
        if math.sqrt((px - e.x) ^ 2 + (pz - e.z) ^ 2) < e.trigger and speed_kmh < 60.0 and at_height(e.has_y, e.y, car_y) then
            rt.near = i
            R.event = -1
            rt.selected = i
            break
        end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Race logic (after the vehicle step). tel = telemetry of this frame, prev = last frame position.
-- Returns a table of requests for the caller: cue names, finished flag, log lines are written here.
-- ---------------------------------------------------------------------------------------------

local function swept_gate_hit(px0, pz0, px1, pz1, cp, ev, i)
    local h = gate_heading(ev, i)
    local dx, dz = math.sin(h), math.cos(h)
    local sx, sz = dz, -dx
    local hw = cp.width * 0.5
    local g0x, g0z = cp.x - sx * hw, cp.z - sz * hw
    local g1x, g1z = cp.x + sx * hw, cp.z + sz * hw
    if not geom.segments_intersect(px0, pz0, px1, pz1, g0x, g0z, g1x, g1z) then return false end
    if (px1 - px0) * dx + (pz1 - pz0) * dz <= 0.0 then return false end
    return true
end

function M.update_race(dt, tel, px, pz, prev_x, prev_z, car_y, out)
    local speed = tel.speed_kmh
    -- Float32 arithmetic on the timers: the native fields are float, and a double countdown drains one
    -- frame early (3.0 s takes 181 frames in float32, 180 in double).
    rt.msg_timer = math.max(0.0, geom.f32(rt.msg_timer - dt))
    if R.phase == M.FREE then
        free_roam(px, pz, speed, car_y, out)
    elseif R.phase == M.COUNTDOWN then
        R.countdown = geom.f32(R.countdown - dt)
        local beep = math.ceil(R.countdown)
        if beep ~= R.beep and beep > 0 then
            out.cue = "tick"
            R.beep = beep
        end
        if R.countdown <= 0.0 then
            R.phase = M.RACING
            R.time = 0.0
            out.cue = "start"
            show_message("GO", 0.8)
        end
    elseif R.phase == M.RACING then
        R.time = geom.f32(R.time + dt)
        local ev = rt.active
        local count = #ev.checkpoints
        for i = R.next_cp + 1, count do
            local cp = ev.checkpoints[i]
            if swept_gate_hit(prev_x, prev_z, px, pz, cp, ev, i) and at_height(cp.has_y, cp.y, car_y) then
                if i == R.next_cp + 1 then
                    rt.splits[#rt.splits + 1] = R.time
                    R.next_cp = R.next_cp + 1
                    say(string.format("[Race] Checkpoint %d/%d at %g s", R.next_cp, count, R.time))
                    if R.next_cp >= count then
                        R.phase = M.FINISHED
                        R.last_time = R.time
                        local best = best_of(ev.id)
                        R.new_record = best < 0.0 or R.time < best
                        if R.new_record then
                            rt.best[ev.id] = { time = R.time, splits = rt.splits }
                            if M.save_best then M.save_best(rt.best) end
                            out.cue = "record"
                        else
                            out.cue = "complete"
                        end
                        say(string.format("[Race] Finished in %g s%s", R.time, R.new_record and " (new record)" or ""))
                        set_menu("RESULTS", { "Retry", "Free Roam" })
                        if R.new_record then out.finish_record = true end
                        out.finished = true
                    else
                        out.cue = "marker"
                    end
                else
                    show_message("MISSED CHECKPOINT - FOLLOW THE ROUTE", 2.5)
                    say(string.format("[Race] Missed checkpoint %d (crossed %d)", R.next_cp + 1, i))
                    out.cue = "fail"
                end
                break
            end
        end
        if R.phase == M.RACING and #rt.route >= 2 and speed > 20.0 then
            local rx, rz = rt.route[2].x - rt.route[1].x, rt.route[2].z - rt.route[1].z
            local rl = math.sqrt(rx * rx + rz * rz) + 1e-4
            local vx, vz = px - prev_x, pz - prev_z
            local vl = math.sqrt(vx * vx + vz * vz)
            if vl > 1e-4 and (vx * rx + vz * rz) / (vl * rl) < -0.6 then
                R.wrong_way = geom.f32(R.wrong_way + dt)
            else
                R.wrong_way = math.max(0.0, geom.f32(R.wrong_way - dt * 2.0))
            end
            if R.wrong_way > 2.0 and rt.msg_timer <= 0.0 then show_message("WRONG WAY", 1.5) end
        end
    end
    local tx, tz = route_target()
    refresh_route(dt, px, pz, tx or px, tz or pz, R.phase == M.FINISHED)
end

-- Restores the mirror of the declared state after a reload (route, messages and menus rebuild).
function M.route() return rt.route end
function M.phase_name() return PHASE_NAME[R.phase] end
function M.clear_route() rt.route = {} end
function M.set_waypoint(x, z) rt.waypoint = { x = x, z = z } end
function M.clear_waypoint() rt.waypoint = nil end
function M.waypoint() return rt.waypoint end
function M.menu() return rt.menu_title, rt.menu_items, rt.menu_sel end
function M.set_menu_sel(i) rt.menu_sel = i end
function M.message() return rt.msg, rt.msg_timer end
function M.near_event() return rt.near end
function M.selected() return rt.selected end
function M.set_selected(i) rt.selected = i end
function M.splits() return rt.splits end
function M.at_place() return rt.at_place end
function M.active_index() return rt.active_index end
function M.active_event() return rt.active end
function M.paused() return rt.paused end
function M.set_exit_requested() rt.exit_requested = true end
function M.exit_requested() return rt.exit_requested end
function M.incidents() return rt.incidents end
function M.add_incident() rt.incidents = rt.incidents + 1 end
function M.last_gear() return rt.last_gear end
function M.set_last_gear(g) rt.last_gear = g end
function M.menu_activate_item(index) return rt.menu_items[index] end
function M.set_prev(x, z) rt.prev_x, rt.prev_z = x, z end
function M.prev() return rt.prev_x, rt.prev_z end
function M.set_menu_title(t) rt.menu_title = t end

-- Mouse wheel over an open menu: steps the selection (+1 next, -1 previous, wrapping).
function M.step_menu(d)
    local n = #rt.menu_items
    if n == 0 then return end
    rt.menu_sel = (rt.menu_sel - 1 + (d < 0 and -1 or 1)) % n + 1
end

-- Menu input (keys/pad through actions): returns the activated item name or nil.
function M.menu_input(btn)
    if not rt.menu_title or #rt.menu_items == 0 then return nil end
    local n = #rt.menu_items
    if btn.up then rt.menu_sel = (rt.menu_sel - 2) % n + 1 end
    if btn.down then rt.menu_sel = rt.menu_sel % n + 1 end
    if rt.menu_title == "PAUSED" and btn.back then return "Resume" end
    if btn.confirm then return rt.menu_items[rt.menu_sel] end
    return nil
end

-- Draw state for the HUD (scripts/ui/README.md, "HUD state").
function M.hud_state(tel, dt)
    local st = {
        speed_kmh = tel.speed_kmh, rpm = tel.rpm, load = tel.load or 0.0, nitro = tel.nitro,
        nitro_active = tel.nitro_active, nitro_recharging = (tel.nitro < 0.999) and not tel.nitro_active,
        gear = tel.gear, redline = tel.rpm >= (cfg.redline or 7800.0), limiter = tel.limiter,
        car = { x = tel.x, z = tel.z }, heading = geom.rad(tel.heading_deg),
        layered = false, roads = cfg.roads,
        route = rt.route,
        phase = PHASE_NAME[R.phase],
        start_key = "Enter",
        race_time = R.time, countdown = R.countdown,
        checkpoint = R.next_cp, checkpoint_count = rt.active and #rt.active.checkpoints or 0,
        splits = rt.splits,
        message = rt.msg, message_alpha = math.min(rt.msg_timer * 2.0, 1.0),
        hint = "",
    }
    if rt.paused then st.phase = "paused" end
    if rt.near >= 0 and R.phase == M.FREE then st.phase = "near_event" end
    local ev = rt.active or cfg.events[rt.selected]
    if ev then
        st.event_type, st.event_name, st.event_difficulty = ev.type, ev.name, ev.difficulty
        st.best_time = best_of(ev.id)
    end
    st.best_holder = "You"
    st.last_time = R.last_time
    st.new_record = R.new_record
    if rt.menu_title then
        st.menu = { title = rt.menu_title, items = rt.menu_items, selected = rt.menu_sel }
    end
    local markers = {}
    for _, e in ipairs(cfg.events) do
        markers[#markers + 1] = { x = e.x, z = e.z, kind = "event", label = e.type }
    end
    for _, pl in ipairs(cfg.places) do markers[#markers + 1] = { x = pl.x, z = pl.z, kind = "place", label = pl.name } end
    if rt.waypoint then markers[#markers + 1] = { x = rt.waypoint.x, z = rt.waypoint.z, kind = "waypoint", label = "" } end
    if rt.active and (R.phase == M.RACING or R.phase == M.COUNTDOWN) then
        for i, cp in ipairs(rt.active.checkpoints) do
            local kind = i == #rt.active.checkpoints and "finish" or (i == R.next_cp + 1 and "next_checkpoint" or "checkpoint")
            markers[#markers + 1] = { x = cp.x, z = cp.z, kind = kind, label = "" }
        end
    end
    st.markers = markers
    st.dt = dt
    return st
end

return M

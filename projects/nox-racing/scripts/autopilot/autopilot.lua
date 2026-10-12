-- Autopilot (NOX_DRIVE_AUTOPILOT=1): scripted driver for unattended validation. Port of the autopilot
-- block in GameSession::ReadInput. It follows the route, starts the event, retries once from the results
-- menu, then returns to free roam. Stuck and fall detection report "[Traversal]" lines and recover.
--
-- The log text is the native text at the start of the line (the session's say() writes it raw).
-- Gaps (documented in the session report): stuck recovery teleports to the nearest centre line point
-- (native: lane position + ground check); the autopilot state is not declared (it restarts on reload).
local geom = require("race.geom")
local session = require("race.session")

local M = {}

local st = {
    timer = 0.0, runs = 0, stuck = 0.0, fall_timer = 0.0, last_y = 0.0, incidents = 0,
    results_for = 0.0, log_timer = 0.0, prev_start = false, prev_confirm = false,
    maneuver_time = 0.0,
}

function M.incidents() return st.incidents end
function M.reset_for_event(y)
    st.stuck = 0.0
    st.fall_timer = 0.0
    st.last_y = y or 0.0
end

local function say(line) session.say(line) end

-- Look-ahead point and steering from the route (route = array of {x, z}).
local function look_ahead_target(route, px, pz, speed_ms)
    local want = geom.clamp(speed_ms * 0.9, 10.0, 30.0)
    local n = #route
    if n < 2 then return px, pz end
    local tx, tz = route[n].x, route[n].z
    for i = 2, n do
        local ax, az = route[i - 1].x, route[i - 1].z
        local bx, bz = route[i].x, route[i].z
        local seg = math.sqrt((bx - ax) ^ 2 + (bz - az) ^ 2)
        if want <= seg then
            local f = want / math.max(seg, 1e-3)
            return ax + (bx - ax) * f, az + (bz - az) * f
        end
        want = want - seg
    end
    return tx, tz
end

-- view: { x, z, heading (rad), speed_kmh, speed_ms, y, route, phase, near (1-based or -1),
--         selected (1-based), active (event table or nil), next_cp (0-based), events, roads,
--         finished_menu (bool), menu_sel (1-based), max_kmh (number), dt }
-- Returns drive = {throttle, brake, steer, handbrake=false, nitrous}, btn = {start, down, confirm} (pressed
-- edges), teleport = {x, z, heading} or nil.
function M.step(view, dt)
    local drive = { throttle = 0.0, brake = 0.0, steer = 0.0, handbrake = false, nitrous = false }
    local btn = { start = false, down = false, confirm = false }
    local teleport = nil
    st.timer = st.timer + dt
    local px, pz = view.x, view.z
    local kmh = view.speed_kmh
    local route = view.route or {}
    local tx, tz = look_ahead_target(route, px, pz, view.speed_ms)
    local dx, dz = tx - px, tz - pz
    local diff = geom.wrap(math.atan(dx, dz) - view.heading)
    drive.steer = geom.clamp(-diff * 2.5, -1.0, 1.0)
    local phase = view.phase

    -- Traversal diagnostics: report and recover when stuck or after falling off a level.
    if phase == session.RACING then
        st.stuck = kmh < 3.0 and st.stuck + dt or 0.0
        local y = view.y or 0.0
        st.fall_timer = st.fall_timer + dt
        if st.fall_timer > 1.0 then
            if st.last_y - y > 4.0 then
                say(string.format("[Traversal] FELL from y=%.2f to y=%.2f at (%.1f,%.1f)", st.last_y, y, px, pz))
                st.incidents = st.incidents + 1
            end
            st.last_y = y
            st.fall_timer = 0.0
        end
        if st.stuck > 3.0 then
            say(string.format("[Traversal] STUCK at (%.1f,%.2f,%.1f) heading %.1f", px, y, pz, geom.deg(view.heading)))
            st.incidents = st.incidents + 1
            st.stuck = 0.0
            -- Skip ~40 m ahead along the route so one run reports every problem spot.
            local skip_x, skip_z = px, pz
            local want = 40.0
            for i = 2, #route do
                local ax, az = route[i - 1].x, route[i - 1].z
                local bx, bz = route[i].x, route[i].z
                local seg = math.sqrt((bx - ax) ^ 2 + (bz - az) ^ 2)
                skip_x, skip_z = bx, bz
                if want <= seg then
                    local f = want / math.max(seg, 1e-3)
                    skip_x, skip_z = ax + (bx - ax) * f, az + (bz - az) * f
                    break
                end
                want = want - seg
            end
            local nx, nz = skip_x, skip_z
            if view.roads then
                local rx, rz = view.roads:nearest(skip_x, skip_z)
                if rx then nx, nz = rx, rz end
            end
            teleport = { x = nx, z = nz, heading = view.heading }
        end
    end

    st.log_timer = st.log_timer + dt
    if st.log_timer > 2.0 then
        st.log_timer = 0.0
        say(string.format("[Autopilot] pos=(%.1f,%.1f) y=%.2f air=%d v=%.0f near=%d phase=%d route=%d",
            px, pz, view.y or 0.0, view.airborne and 1 or 0, kmh, view.near >= 1 and view.near - 1 or -1, phase, #route))
    end

    drive.throttle = math.abs(diff) < 0.35 and 1.0 or 0.35
    if math.abs(diff) > 0.5 and kmh > 70.0 then drive.throttle = 0.0; drive.brake = 1.0 end
    drive.nitrous = math.abs(diff) < 0.1 and kmh > 60.0

    -- Approach the free-roam event marker slowly enough to trigger the start prompt.
    local sel = view.events and view.events[view.selected]
    if phase == session.FREE and sel then
        local dist = math.sqrt((sel.x - px) ^ 2 + (sel.z - pz) ^ 2)
        local limit = 12.0 + dist * 1.2
        if kmh > limit then drive.throttle = 0.0; drive.brake = 0.8; drive.nitrous = false end
    end
    -- Brake before sharp route corners in races.
    if phase == session.RACING and math.abs(diff) > 0.25 and kmh > 75.0 then
        drive.throttle = 0.0; drive.brake = 0.7
    end

    if #route >= 2 then
        -- Distance to the first real corner (more than about 25 degrees) along the route.
        local to_corner = 1e9
        local along = math.sqrt((route[2].x - px) ^ 2 + (route[2].z - pz) ^ 2)
        for i = 2, #route - 1 do
            if along >= 200.0 then break end
            local d0x, d0z = route[i].x - route[i - 1].x, route[i].z - route[i - 1].z
            local d1x, d1z = route[i + 1].x - route[i].x, route[i + 1].z - route[i].z
            local l0 = math.sqrt(d0x * d0x + d0z * d0z)
            local l1 = math.sqrt(d1x * d1x + d1z * d1z)
            if l0 > 1e-3 and l1 > 1e-3 and (d0x * d1x + d0z * d1z) / (l0 * l1) < 0.9 then
                to_corner = along
                break
            end
            along = along + l1
        end
        -- The turn after the current gate counts as a corner at the route end.
        local act = view.active
        if phase == session.RACING and act and view.next_cp + 2 <= #act.checkpoints then
            local gate = act.checkpoints[view.next_cp + 1]
            local after = act.checkpoints[view.next_cp + 2]
            local n = #route
            local in_x, in_z = route[n].x - route[n - 1].x, route[n].z - route[n - 1].z
            local out_x, out_z = after.x - gate.x, after.z - gate.z
            local li = math.sqrt(in_x * in_x + in_z * in_z)
            local lo = math.sqrt(out_x * out_x + out_z * out_z)
            if li > 1e-3 and lo > 1e-3 and (in_x * out_x + in_z * out_z) / (li * lo) < 0.85 then
                local to_end = 0.0
                for i = 2, #route do
                    to_end = to_end + math.sqrt((route[i].x - route[i - 1].x) ^ 2 + (route[i].z - route[i - 1].z) ^ 2)
                end
                to_corner = math.min(to_corner, to_end)
            end
        end
        if to_corner < 45.0 and kmh > 30.0 + to_corner * 1.1 then
            drive.throttle = 0.0; drive.brake = 1.0; drive.nitrous = false
        end
        -- Gentle curves and diverges: limit speed by the route's total turn over the next ~80 m.
        local turn, dist = 0.0, 0.0
        for i = 2, #route - 1 do
            if dist >= 80.0 then break end
            local d0x, d0z = route[i].x - route[i - 1].x, route[i].z - route[i - 1].z
            local d1x, d1z = route[i + 1].x - route[i].x, route[i + 1].z - route[i].z
            local l0 = math.sqrt(d0x * d0x + d0z * d0z)
            local l1 = math.sqrt(d1x * d1x + d1z * d1z)
            if l0 >= 1e-3 and l1 >= 1e-3 then
                turn = turn + math.acos(geom.clamp((d0x * d1x + d0z * d1z) / (l0 * l1), -1.0, 1.0))
                dist = dist + l1
            end
        end
        local vmax = geom.clamp(150.0 - geom.deg(turn) * 2.2, 45.0, view.max_kmh or 150.0)
        if kmh > vmax + 5.0 then drive.throttle = 0.0; drive.brake = 0.6; drive.nitrous = false
        elseif kmh > vmax then drive.throttle = 0.0; drive.nitrous = false end
    end

    -- Free roam: start the event when the car is at rest in the trigger (once per run).
    if phase == session.FREE and view.near >= 1 and st.runs < 2 then
        drive.throttle = 0.0
        drive.brake = kmh > 3.0 and 1.0 or 0.0
        local level = kmh < 5.0 and (st.timer % 0.5) < 0.25
        btn.start = level and not st.prev_start
        if level and not st.prev_start and st.runs == 0 then st.runs = st.runs + 1 end
        st.prev_start = level
    else
        st.prev_start = false
    end

    -- Results: the driver reads the results screen for 3 s, then retries once and leaves to free roam.
    st.results_for = (phase == session.FINISHED and view.finished_menu) and st.results_for + dt or 0.0
    if phase == session.FINISHED and view.finished_menu and st.results_for > 3.0 then
        local tick = (st.timer % 1.0) < 0.5
        if st.runs >= 2 and view.menu_sel == 1 then
            btn.down = tick and not st.prev_down
            st.prev_down = tick
        else
            local confirm = tick
            btn.confirm = confirm and not st.prev_confirm
            if confirm and not st.prev_confirm and view.menu_sel == 1 then st.runs = st.runs + 1 end
            st.prev_confirm = confirm
        end
    end
    return drive, btn, teleport
end

-- NOX_EXIT_ON_FINISH: the session writes the traversal summary once the race finished.
function M.finish_line()
    return string.format("[Traversal] Finished; incidents=%d", st.incidents)
end

return M

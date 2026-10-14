-- Chase camera: port of DriveMode::UpdateCamera (src/DriveMode.cpp). Modes: 0 chase, 1 far, 2 bumper
-- (cockpit-like), 3 free (the camera is not moved by the game). Smoothing state (heading, spring position
-- and velocity, fov, previous car position) is a plain table `st` that the caller stores in the declared
-- schema ChaseCamera, so it survives a reload.
local geom = require("race.geom")

local M = {}

local DEFAULTS = {
    distance = 4.8, height = 1.72, look_ahead = 10.0, look_height = 0.45,
    heading_lag = 5.0, velocity_follow = 0.45, stiffness = 9.0, pull_back = 0.6,
    fov = 54.0, fov_speed_gain = 5.0, fov_nitro = 4.0,
}

-- Rig values from the scene's game.camera, defaults where a key is missing.
function M.rig(camera_json)
    local r = {}
    for k, v in pairs(DEFAULTS) do
        local got = type(camera_json) == "table" and camera_json[k]
        r[k] = type(got) == "number" and got or v
    end
    return r
end

-- Fresh smoothing state (also used after a teleport).
function M.fresh()
    return { init = false, heading = 0.0, px = 0.0, py = 0.0, pz = 0.0, vx = 0.0, vy = 0.0, vz = 0.0,
        cx = 0.0, cy = 0.0, cz = 0.0, fov = 54.0, mode = 0 }
end

local function wrap(a)
    while a > math.pi do a = a - 2.0 * math.pi end
    while a < -math.pi do a = a + 2.0 * math.pi end
    return a
end

local function approach(cur, target, step)
    if cur < target then return math.min(cur + step, target) end
    return math.max(cur - step, target)
end

-- tel: ctx.vehicle.telemetry(h). Returns the eye and target (for the caller to apply) and fov.
-- ctx: used for nox camera calls. teleported: true for one frame after a discontinuous move.
function M.update(ctx, tel, dt, rig, st, aspect, teleported, orbit_deg)
    local heading = math.rad(tel.heading_deg)
    local fwd_x, fwd_z = math.sin(heading), math.cos(heading)
    local left_x, left_z = math.cos(heading), -math.sin(heading)
    -- World velocity from the body frame (VehicleService: fwd*vx + left*vy).
    local vel_x = fwd_x * tel.vx + left_x * tel.vy
    local vel_z = fwd_z * tel.vx + left_z * tel.vy
    local speed = math.sqrt(vel_x * vel_x + vel_z * vel_z)
    local speed01 = tel.speed01
    local car_x, car_y, car_z = tel.x, tel.y or 0.0, tel.z
    if teleported then st.init = false end

    -- Follow direction: heading blended with the velocity direction (drifts show the flank).
    local follow = heading
    if speed > 4.0 and tel.vx > 0.0 then
        local vh = math.atan(vel_x, vel_z)
        local w = geom.clamp((speed - 4.0) / 8.0, 0.0, 1.0)
        follow = heading + wrap(vh - heading) * rig.velocity_follow * w
    end
    follow = follow + math.rad(orbit_deg or 0.0)
    if not st.init then st.heading = follow end
    local lag = (st.mode == 2) and 30.0 or rig.heading_lag
    st.heading = wrap(st.heading + wrap(follow - st.heading) * (1.0 - math.exp(-dt * lag)))
    local cam_fwd_x, cam_fwd_y, cam_fwd_z = math.sin(st.heading), 0.0, math.cos(st.heading)

    local distance, height, look_ahead, look_height = rig.distance, rig.height, rig.look_ahead, rig.look_height
    if st.mode == 1 then
        distance = distance * 1.5; height = height * 1.45; look_ahead = look_ahead * 1.1; look_height = look_height * 1.4
    end
    distance = distance + speed01 * rig.pull_back + geom.clamp(tel.ax * 0.025, -0.12, 0.3)

    local dx = car_x - cam_fwd_x * distance
    local dy = car_y + height
    local dz = car_z - cam_fwd_z * distance
    local tx = car_x + cam_fwd_x * look_ahead
    local ty = car_y + look_height
    local tz = car_z + cam_fwd_z * look_ahead
    if st.mode == 2 then
        dx = car_x + fwd_x * 1.9; dy = car_y + 0.75; dz = car_z + fwd_z * 1.9
        tx = dx + fwd_x * 10.0; ty = dy; tz = dz + fwd_z * 10.0
    end

    if not st.init or st.mode == 2 then
        st.px, st.py, st.pz = dx, dy, dz
        st.vx, st.vy, st.vz = 0.0, 0.0, 0.0
        st.init = true
    else
        -- Critically damped spring on the car-relative offset.
        local doy_x, doy_y, doy_z = dx - car_x, dy - car_y, dz - car_z
        local cof_x, cof_y, cof_z = st.px - st.cx, st.py - st.cy, st.pz - st.cz
        local om = rig.stiffness
        local xx, xy, xz = cof_x - doy_x, cof_y - doy_y, cof_z - doy_z
        local e = math.exp(-om * dt)
        local tmx, tmy, tmz = (st.vx + om * xx) * dt, (st.vy + om * xy) * dt, (st.vz + om * xz) * dt
        st.vx = (st.vx - om * tmx) * e
        st.vy = (st.vy - om * tmy) * e
        st.vz = (st.vz - om * tmz) * e
        local ox = doy_x + (xx + tmx) * e
        local oy = doy_y + (xy + tmy) * e
        local oz = doy_z + (xz + tmz) * e
        oy = doy_y + (oy - doy_y) * 0.5
        st.px, st.py, st.pz = car_x + ox, car_y + oy, car_z + oz
    end
    st.cx, st.cy, st.cz = car_x, car_y, car_z

    local ex, ey, ez = st.px, st.py, st.pz
    if speed01 > 0.6 then
        local a = (speed01 - 0.6) * 0.012
        local t = ctx:time()
        ex = ex + math.sin(t * 31.0) * a
        ey = ey + math.sin(t * 47.0 + 1.3) * a
    end
    local target_fov = rig.fov + speed01 * rig.fov_speed_gain + (((tel.channels.nitro_active or 0.0) > 0.5) and rig.fov_nitro or 0.0)
    st.fov = approach(st.fov, target_fov, dt * 2.5)

    if st.mode == 3 then return nil end
    return ex, ey, ez, tx, ty, tz, st.fov
end

-- Applies the pose to the module camera (camera_look_at refuses a zero look vector; a pose at the
-- target is skipped).
function M.apply(ctx, ex, ey, ez, tx, ty, tz, fov, teleported)
    if not ex then return end
    local d = (tx - ex) ^ 2 + (ty - ey) ^ 2 + (tz - ez) ^ 2
    if d > 1e-6 then ctx:camera_look_at(ex, ey, ez, tx, ty, tz) end
    ctx:camera_set_fov(fov)
    if teleported then ctx:camera_cut() end
end

return M

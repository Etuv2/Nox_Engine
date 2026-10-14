-- Presentation decisions for the racing game: tyre smoke and spray (VehicleFX emitters through ctx),
-- procedural engine parameters (ctx:audio_set) and cues (ctx:audio_cue). Port of GameSession::EmitTyreEffects
-- and DriveAudio's cue mapping. The policy is here; the emitters and the DSP are engine services.
--
-- Tyres use the per-wheel telemetry (tel.wheels: contact, velocity, wheelspin, slip, grounded; FL FR RL RR). The
-- axle approximation from the car pose remains as the fallback for telemetry without wheels.
local M = {}

local CUE = {
    select = "select", back = "back", countdown = "tick", go = "start", checkpoint = "marker",
    finish = "complete", record = "record", missed = "fail", shift = "gear_shift",
}
M.CUE = CUE

local function clamp(x, lo, hi)
    if x < lo then return lo end
    if x > hi then return hi end
    return x
end

-- Emits smoke and spray at the four tyres from the wheel states (contact point, contact velocity, wheelspin,
-- slip, grounded). Smoke needs slip energy; spray needs speed and a grounded tyre.
function M.tyres(ctx, tel, dt)
    local wheels = tel.wheels
    if type(wheels) ~= "table" or #wheels < 4 then return M.axle_tyres(ctx, tel, dt) end
    if tel.teleported then ctx:fx_clear() end
    local speed = tel.speed_kmh
    for i = 1, 4 do
        local w = wheels[i]
        local rear = i >= 3
        if w.grounded then
            local c, v = w.contact, w.velocity
            local spin = w.wheelspin or 0.0
            local slip_smoke = clamp((w.slip - 0.45) / 0.55, 0.0, 1.0) * clamp(speed / 25.0 + spin, 0.0, 1.0)
            local smoke = math.max(spin, slip_smoke) * (rear and 1.0 or 0.35)
            if smoke > 0.03 then
                local sx, sz = v[1] * 0.3, v[2] * 0.3
                ctx:fx_smoke(c[1], c[2] + 0.18, c[3], sx, 0.3, sz, smoke, dt, c[2])
            end
            local spray = clamp((speed - 20.0) / 90.0, 0.0, 1.0) * (rear and 1.0 or 0.45)
            if spray > 0.02 then
                ctx:fx_spray(c[1], c[2] + 0.05, c[3], v[1] * 0.75, 0.0, v[2] * 0.75, spray, dt, c[2])
            end
        end
    end
end

-- Fallback without per-wheel telemetry: front and rear axles placed from the heading, the track and the wheelbase.
function M.axle_tyres(ctx, tel, dt)
    if tel.teleported then ctx:fx_clear() end
    local h = math.rad(tel.heading_deg)
    local fx, fz = math.sin(h), math.cos(h)
    local lx, lz = math.cos(h), -math.sin(h)
    local half_track = (tel.track or 1.6) * 0.5
    local half_base = (tel.wheelbase or 2.72) * 0.5
    local speed = tel.speed_kmh
    local y = (tel.y or 0.0)
    local vx = fx * tel.vx + lx * tel.vy
    local vz = fz * tel.vx + lz * tel.vy
    for axle = 0, 1 do
        local rear = axle == 1
        local along = rear and -half_base or half_base
        local spin = rear and (tel.rear_spin or 0.0) or 0.0
        local slip01 = tel.slip or 0.0
        -- Smoke needs slip energy: wheelspin, a sliding rear in a drift, or locked wheels.
        local slip_smoke = clamp((slip01 - 0.45) / 0.55, 0.0, 1.0) * clamp(speed / 25.0 + spin, 0.0, 1.0)
        local smoke = math.max(spin, slip_smoke) * (rear and 1.0 or 0.35)
        for side = -1, 1, 2 do
            local px = tel.x + lx * half_track * side + fx * along
            local pz = tel.z + lz * half_track * side + fz * along
            if smoke > 0.03 then
                local sx = vx * 0.3 - fx * (spin * 1.8 + 0.3)
                local sz = vz * 0.3 - fz * (spin * 1.8 + 0.3)
                ctx:fx_smoke(px - fx * 0.45, y + 0.18, pz - fz * 0.45, sx, 0.3, sz, smoke, dt, y)
            end
            local spray = clamp((speed - 20.0) / 90.0, 0.0, 1.0) * (rear and 1.0 or 0.45)
            if spray > 0.02 then
                ctx:fx_spray(px, y + 0.05, pz, vx * 0.75, 0.0, vz * 0.75, spray, dt, y)
            end
        end
    end
end

-- Procedural engine state for this frame. drive = the sampled input (throttle).
function M.audio(ctx, tel, throttle, paused, rpm_max)
    ctx:audio_set({
        active = true,
        paused = paused and true or false,
        rpm = tel.rpm,
        rpm_max = rpm_max or 7800.0,
        throttle = throttle,
        speed01 = tel.speed01,
        slip01 = tel.slip,
        boost = (tel.channels.nitro_active or 0.0) > 0.5,
        limiter = tel.limiter and true or false,
    })
end

function M.cue(ctx, key)
    local name = CUE[key] or key
    ctx:audio_cue(name)
end

-- Gear shift cue: the controller's shift event is not in telemetry, so a change of one step between
-- forward gears counts as a shift (documented gap, see AUTHORING.md / the report).
function M.shift_cue(ctx, gear, last_gear)
    if last_gear and gear ~= last_gear and gear >= 1 and last_gear >= 1 and math.abs(gear - last_gear) == 1 then
        M.cue(ctx, "shift")
    end
end

return M

-- Test drivers (NOX_DRIVE_SCRIPT=maneuvers | gearbox): timed handling tests for validation recordings and
-- parity runs. Port of the two blocks in GameSession::ReadInput. Each returns the drive input for the frame
-- and writes the same "[Telemetry]" / "[Gearbox]" lines every 0.25 s.
--
-- Gaps: the Lua telemetry has no rear wheel state beyond rear_spin, no VehicleFX particle count (fx=0 is
-- printed), and drift/beta come from the telemetry proxies (see the report).
local geom = require("race.geom")
local session = require("race.session")

local M = {}

local function say(line) session.say(line) end
-- Clocks are float32 like the native fields (m_scriptTime, m_phaseTime): a double sum can cross a phase threshold
-- one frame apart from the native one.
local function wrap(a) return geom.wrap(a) end
local function clamp(x, lo, hi) return geom.clamp(x, lo, hi) end

local mv = { t = 0.0, phase = 0, phase_time = 0.0, stuck = 0.0, log = 0.0 }
local gb = { t = 0.0, phase = 0, phase_time = 0.0, log = 0.0 }

local function drive(throttle, brake, steer, handbrake)
    return { throttle = throttle, brake = brake, steer = steer, handbrake = handbrake and true or false, nitrous = false }
end

-- view: { x, z, y, heading (rad), speed_kmh, speed_ms, tel (telemetry) }, dt.
function M.maneuvers(view, dt)
    local tel = view.tel
    mv.t = geom.f32(mv.t + dt)
    local t = mv.t
    local px, py, pz = view.x, view.y or 0.0, view.z
    local head = view.heading
    local kmh = view.speed_kmh
    local function hold_line(desired, lateral)
        local want = desired + clamp(lateral * 0.12, -0.4, 0.4)
        return clamp(-wrap(want - head) * 2.6 / (1.0 + kmh / 140.0), -1.0, 1.0)
    end
    local d = drive(0.0, 0.0, 0.0, false)
    if mv.phase >= 2 and mv.phase < 12 and kmh < 2.0 and mv.phase ~= 3 then mv.stuck = geom.f32(mv.stuck + dt)
    elseif kmh > 6.0 then mv.stuck = 0.0 end
    if mv.stuck > 1.0 then
        d.brake = 1.0; d.throttle = 0.0; d.steer = 0.8
        if mv.stuck > 2.4 then mv.stuck = 0.0 end
    else
        local p = mv.phase
        if p == 0 then
            if t > 0.5 then mv.phase = 1 end
        elseif p == 1 then
            d.throttle = 1.0; d.brake = 1.0
            if t > 2.5 then mv.phase = 2 end
        elseif p == 2 then
            d.throttle = 1.0; d.steer = hold_line(0.0, -2.0 - px)
            if pz > 15.0 then mv.phase = 3 end
        elseif p == 3 then
            d.brake = 1.0; d.steer = hold_line(0.0, -2.0 - px)
            if kmh < 30.0 then mv.phase = 4 end
        elseif p == 4 then
            d.throttle = 0.65
            local target_x = -2.0 - 2.4 * math.sin(math.max(pz - 45.0, 0.0) / 11.0)
            d.steer = hold_line(0.0, target_x - px)
            if pz > 130.0 then mv.phase = 5 end
        elseif p == 5 then
            d.throttle = kmh < 72.0 and 1.0 or 0.0
            d.brake = kmh > 82.0 and 0.6 or 0.0
            d.steer = hold_line(0.0, -2.0 - px)
            if pz > 176.0 then mv.phase = 6 end
            mv.phase_time = 0.0
        elseif p == 6 then
            d.throttle = 0.6; d.steer = -1.0; d.handbrake = true
            mv.phase_time = geom.f32(mv.phase_time + dt)
            if mv.phase_time > 0.35 then mv.phase = 7; mv.phase_time = 0.0 end
        elseif p == 7 then
            d.throttle = 1.0
            mv.phase_time = geom.f32(mv.phase_time + dt)
            local rel = wrap(math.pi * 0.5 - head)
            d.steer = rel > 0.6 and -0.6 or 0.0
            if math.abs(rel) < 0.15 or mv.phase_time > 3.5 then mv.phase = 8 end
        elseif p == 8 then
            d.throttle = kmh < (px > 80.0 and 74.0 or 100.0) and 0.9 or 0.0
            d.brake = kmh > (px > 80.0 and 84.0 or 120.0) and 0.7 or 0.0
            d.steer = hold_line(math.pi * 0.5, pz - 202.0)
            if px > 129.0 then mv.phase = 9; mv.phase_time = 0.0 end
        elseif p == 9 then
            d.brake = 1.0; d.steer = -0.8
            mv.phase_time = geom.f32(mv.phase_time + dt)
            if mv.phase_time > 0.25 then mv.phase = 10; mv.phase_time = 0.0 end
        elseif p == 10 then
            d.throttle = 1.0
            mv.phase_time = geom.f32(mv.phase_time + dt)
            local rel = wrap(math.pi - head)
            d.steer = rel > 0.3 and -0.8 or hold_line(math.pi, px - 146.0)
            if math.abs(wrap(math.pi - head)) < 0.3 or mv.phase_time > 3.5 then mv.phase = 11 end
        elseif p == 11 then
            d.throttle = 0.7; d.steer = hold_line(math.pi, px - 146.0)
            mv.phase_time = geom.f32(mv.phase_time + dt)
            if mv.phase_time > 3.0 then mv.phase = 12 end
        else
            d.brake = 1.0
        end
    end
    mv.log = geom.f32(mv.log + dt)
    if mv.log >= 0.25 then
        mv.log = 0.0
        say(string.format("[Telemetry] ph=%d t=%5.2f v=%6.1f km/h gear=%d rpm=%5.0f drift=%d beta=%5.1f spin=%.2f slip=%.2f pos=(%.1f,%.1f) head=%.1f fx=%d",
            mv.phase, t, kmh, tel.gear, tel.rpm, (tel.drift or 0.0) > 0.5 and 1 or 0, geom.deg(tel.beta or 0.0),
            tel.rear_spin or 0.0, tel.slip or 0.0, px, pz, geom.deg(head), 0))
    end
    return d
end

function M.gearbox(view, dt)
    local tel = view.tel
    gb.t = geom.f32(gb.t + dt)
    gb.phase_time = geom.f32(gb.phase_time + dt)
    local px, pz = view.x, view.z
    local kmh = view.speed_kmh
    local head = view.heading
    local keep = clamp(-wrap(clamp((-3.8 - px) * 0.12, -0.3, 0.3) - head) * 2.6 / (1.0 + kmh / 140.0), -1.0, 1.0)
    local function next_phase(p) gb.phase = p; gb.phase_time = 0.0 end
    local d = drive(0.0, 0.0, 0.0, false)
    local p = gb.phase
    if p == 0 then
        if gb.phase_time > 0.5 then next_phase(1) end
    elseif p == 1 then
        d.throttle = 1.0; d.steer = keep
        if pz > 250.0 or gb.phase_time > 30.0 then next_phase(2) end
    elseif p == 2 then
        d.brake = 1.0; d.steer = keep
        if kmh < 0.5 and gb.phase_time > 0.5 then next_phase(3) end
    elseif p == 3 then
        d.brake = 1.0
        if gb.phase_time > 4.0 then next_phase(4) end
    elseif p == 4 then
        d.throttle = 1.0
        if gb.phase_time > 4.0 then next_phase(5) end
    else
        d.brake = 1.0
    end
    gb.log = geom.f32(gb.log + dt)
    if gb.log >= 0.25 then
        gb.log = 0.0
        say(string.format("[Gearbox] ph=%d t=%5.2f thr=%.1f brk=%.1f v=%6.1f km/h vx=%6.2f gear=%d rpm=%5.0f y=%.1f",
            gb.phase, gb.t, d.throttle, d.brake, kmh, tel.vx, tel.gear, tel.rpm, view.y or 0.0))
    end
    return d
end

function M.reset()
    mv.t, mv.phase, mv.phase_time, mv.stuck, mv.log = 0.0, 0, 0.0, 0.0, 0.0
    gb.t, gb.phase, gb.phase_time, gb.log = 0.0, 0, 0.0, 0.0
end

return M

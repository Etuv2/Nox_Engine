-- Direction (reverse) selection and automatic gear shifts. The mechanical ratio/rpm math stays native:
-- the controller returns the gear and the shift timers, the backend evaluates the engine from them.
local util = require("vehicle.util")
local M = {}

function M.new()
    return { shift_timer = 0.0, shift_cooldown = 0.0, direction_hold = 0.0, shift_event = false }
end

-- Pedal-driven direction (NFS style). Hold the brake at a standstill to engage reverse; in reverse the
-- throttle brakes and holding it at a standstill selects first again. Only a deliberate hold at (near)
-- zero speed changes direction. Returns the selected gear.
function M.direction(st, pol, hd, h, gear, vx, inp)
    local stopped = math.abs(vx) < 1.0
    local want_reverse = gear >= 1 and inp.brake > 0.1 and inp.throttle < 0.1
    local want_forward = gear == -1 and inp.throttle > 0.1 and inp.brake < 0.1
    if stopped and (want_reverse or want_forward) then
        st.direction_hold = st.direction_hold + h
        if st.direction_hold >= hd.reverse_engage_delay then
            gear = want_reverse and -1 or 1
            st.direction_hold = 0.0
            st.shift_cooldown = pol.reverse_cooldown_s
            st.shift_event = true
        end
    else
        st.direction_hold = 0.0
    end
    -- Rolling forward under its own speed while in reverse (or after a teleport): back to a forward gear.
    if gear == -1 and vx > 3.0 then gear = 1 end
    local count = #hd.gear_ratios
    if gear == 0 or gear > count then gear = util.clamp(gear, 1, count) end
    -- At a standstill a forward gear is always first (e.g. after stopping hard against a wall).
    if gear > 1 and stopped and st.shift_timer <= 0.0 then gear = 1 end
    return gear
end

-- Automatic up/down shifts (forward gears), from PostStep. Returns the gear and whether a shift timer was
-- running before this sub-step (the backend's gearboxShifting flag).
--   up        at the redline under throttle (not while the rear tyres spin up)
--   kickdown  floored throttle while the lower gear still has room below ~80% of the redline
--   braking   step down as speed falls, keeping the engine in its band for the exit
--   lugging   part throttle / coasting below the lugging rpm
-- A short lockout after every shift prevents hunting between two gears.
function M.post_step(st, pol, hd, tel, inp, gear)
    local h = tel.h
    local shifting = st.shift_timer > 0.0
    st.shift_timer = math.max(0.0, st.shift_timer - h)
    st.shift_cooldown = math.max(0.0, st.shift_cooldown - h)
    local gears = #hd.gear_ratios
    local function engine_rpm(g)
        local rpm_for = math.abs(tel.wheel_speed) / tel.rear_wheel_radius * 60.0 / (2.0 * math.pi)
            * hd.gear_ratios[g] * tel.final_drive
        return hd.rpm_idle + rpm_for
    end
    if gear >= 1 and not shifting and st.shift_cooldown <= 0.0 then
        local rpm_now = engine_rpm(gear)
        local lower_rpm = gear > 1 and engine_rpm(gear - 1) or 1e9
        local redline = hd.rpm_redline
        local up = gear < gears and inp.throttle > 0.1 and tel.vx > 3.0 and tel.rear_spin < 0.5
            and (rpm_now >= redline or tel.limiter)
        local kickdown = inp.throttle > 0.9 and lower_rpm < redline * 0.8
        local braking = inp.brake > 0.1 and tel.vx > 0.3 and lower_rpm < redline * 0.7
        local lugging = inp.throttle < 0.9 and rpm_now < pol.lugging_rpm and lower_rpm < redline * 0.75
        if up then
            gear = gear + 1
            st.shift_timer = hd.shift_time
            st.shift_cooldown = pol.upshift_cooldown_s
            st.shift_event = true
        elseif gear > 1 and (kickdown or braking or lugging) then
            gear = gear - 1
            st.shift_timer = hd.shift_time * pol.downshift_time_scale
            st.shift_cooldown = pol.downshift_cooldown_s
            st.shift_event = true
        end
    end
    return gear, shifting
end

function M.teleport(st)
    st.shift_timer = 0.0
    st.shift_cooldown = 0.0
    st.direction_hold = 0.0
end

return M

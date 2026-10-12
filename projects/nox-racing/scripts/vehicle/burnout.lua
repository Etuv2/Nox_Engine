-- Pedals, burnout and brake split. The backend applies the forces; this decides what the pedals mean.
-- Burnout: both pedals held near standstill. The front brakes hold the car, the rear spins up (smoke),
-- the rear drive is cut and the car is braked toward rest in the velocity phase.
local util = require("vehicle.util")
local M = {}

function M.new()
    return { burnout = false }
end

-- Called in Control after the direction decision (the brake pedal depends on the selected gear).
-- Fills the pedal and brake fields of the actuation table `out`.
function M.pedals(st, pol, tel, inp, gear, out)
    st.burnout = inp.throttle > 0.5 and inp.brake > 0.5 and math.abs(tel.vx) < 3.0
    out.drive_throttle = inp.throttle
    out.reverse_drive = inp.brake > 0.1 and inp.brake or 0.0
    out.brake_command = gear == -1 and inp.throttle or inp.brake
    out.brake_at_standstill = st.burnout
    out.brake_front_share = pol.brake_front_share
    out.brake_rear_share = st.burnout and 0.0 or pol.brake_rear_share
    out.rear_drive_cut = st.burnout
    out.rear_spin_floor = st.burnout and inp.throttle or 0.0
    out.rear_lock = inp.handbrake
    out.hold_at_rest = inp.throttle == 0.0 and inp.brake == 0.0
end

-- Burnout hold on the integrated velocity (before the drift swing). Takes and returns vx.
function M.velocity(st, h, vx)
    if st.burnout then
        return util.approach(vx, 0.0, h * 8.0)
    end
    return vx
end

function M.teleport(st)
    st.burnout = false
end

return M

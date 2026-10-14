-- Nitrous tank: activation, drain, recharge after a cooldown, thrust and top-speed effects, refuel.
-- Tuning comes from the handling profile (projects/nox-racing/data/vehicles/r33.json "handling").
local M = {}

function M.new()
    return { nitro = 1.0, cooldown = 0.0, active = false }
end

-- Called once per sub-step in Control (before the drift and direction decisions).
function M.step(st, hd, h, vx, inp)
    st.active = (inp.aux.nitrous or 0.0) > 0.5 and st.nitro > 0.0 and inp.throttle > 0.05 and vx > 1.0
    if st.active then
        st.nitro = math.max(0.0, st.nitro - hd.nitro_drain * h)
        st.cooldown = hd.nitro_recharge_delay
    else
        st.cooldown = math.max(0.0, st.cooldown - h)
        if st.cooldown <= 0.0 then
            st.nitro = math.min(1.0, st.nitro + hd.nitro_recharge * h)
        end
    end
end

-- Force and drag modifiers for the native drive evaluation.
function M.drive_scale(st, hd)
    if st.active then return hd.nitro_thrust end
    return 1.0
end

function M.drag_scale(st, hd)
    if st.active then return 1.0 / (hd.nitro_top_speed * hd.nitro_top_speed) end
    return 1.0
end

function M.refuel(st)
    st.nitro = 1.0
    st.cooldown = 0.0
end

function M.hold(st)
    st.active = false
end

return M

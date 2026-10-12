-- Drift state machine, steering with countersteer assist, drift assist yaw terms, slip-rate damping,
-- anti-spin, grip stability, yaw damping and the arcade velocity swing (NFS 2015 style).
local util = require("vehicle.util")
local approach, clamp, mix, radians = util.approach, util.clamp, util.mix, util.radians
local M = {}

function M.new()
    return {
        drift = 0.0, drift_sign = 1.0, prev_beta = 0.0,
        brake_tap_timer = 0.0, drift_exit_timer = 0.0, steer = 0.0,
    }
end

-- Entry and exit of the drift state (Control). The direction is latched at entry from the steering.
function M.update(st, pol, hd, h, tel, inp)
    local speed, beta = tel.speed, tel.beta
    -- Brake tap: a press shorter than the tap length arms drift entry for the window after release.
    if inp.brake > pol.brake_threshold then
        st.brake_tap_timer = math.max(st.brake_tap_timer, 0.0) + h
    elseif st.brake_tap_timer > 0.0 then
        st.brake_tap_timer = st.brake_tap_timer < pol.brake_tap_press_s and -pol.brake_tap_window_s or 0.0
    else
        st.brake_tap_timer = math.min(0.0, st.brake_tap_timer + h)
    end
    local brake_tapped = st.brake_tap_timer < 0.0 and st.brake_tap_timer > -pol.brake_tap_window_s
    local steer_dir = inp.steer >= 0.0 and 1.0 or -1.0
    local power_over = math.abs(beta) > pol.drift_power_over_beta and inp.throttle > pol.drift_power_over_throttle
        and beta * steer_dir > 0.0
    local wants_drift = speed > pol.drift_entry_speed and math.abs(inp.steer) > pol.drift_entry_steer
        and (inp.handbrake or (brake_tapped and inp.throttle > 0.3) or power_over)
    if wants_drift then
        if st.drift <= 0.0 then st.drift_sign = steer_dir end
        st.drift = math.min(1.0, st.drift + h * pol.drift_entry_rate)
    end
    if st.drift > 0.0 then
        local drift_dir = st.drift_sign
        local straightening = inp.steer * -drift_dir < -0.6        -- countersteering hard out of it
        local small = math.abs(beta) < radians(pol.drift_small_angle_deg)
        local released = inp.steer * drift_dir < 0.15 and math.abs(beta) < radians(pol.drift_released_angle_deg)
        if small or straightening or released or speed < 7.0 or inp.throttle < 0.05 then
            st.drift_exit_timer = st.drift_exit_timer + h
        else
            st.drift_exit_timer = 0.0
        end
        if st.drift_exit_timer > pol.drift_exit_time and not inp.handbrake then
            st.drift = math.max(0.0, st.drift - h * pol.drift_exit_rate)
        end
    end
end

-- Current drift direction: the latched sign while drifting, else the side of the slip.
function M.direction(st, beta)
    if st.drift > 0.0 then return st.drift_sign end
    return beta >= 0.0 and 1.0 or -1.0
end

-- Steering: speed-sensitive lock, smoothed; the countersteer assist points the front wheels toward the
-- velocity. Returns the front wheel angle (rad, left positive).
function M.steering(st, pol, hd, h, tel, inp)
    local speed, beta = tel.speed, tel.beta
    local rate = hd.steer_rate_hz
    if math.abs(inp.steer) <= math.abs(st.steer) then rate = rate * 1.6 end
    st.steer = approach(st.steer, inp.steer, h * rate)
    local lock = radians(mix(hd.steer_lock_low, hd.steer_lock_high, (tel.speed01) ^ 0.5))
    local delta = -st.steer * lock
    local assist = hd.countersteer_assist * (1.0 - 0.5 * st.drift)
    if speed > 4.0 then
        delta = delta + assist * clamp(beta, -0.6, 0.6) * 0.8
    end
    local limit = radians(hd.steer_lock_low + 10.0)
    return clamp(delta, -limit, limit)
end

-- Yaw terms added after the native force evaluation. Takes the yaw acceleration and ax from the backend
-- and returns the absolute values after the additions.
function M.yaw_assist(st, pol, hd, h, tel, inp, yaw_acc, ax)
    local speed, beta, yaw_rate, vx = tel.speed, tel.beta, tel.yaw_rate, tel.vx
    local delta, wheelbase = tel.steer_angle, tel.wheelbase
    local drift_dir = M.direction(st, beta)
    local drift = st.drift
    local ay = yaw_acc
    local ax_out = ax

    -- Drift assist: hold the drift angle selected by the steering, keep speed through the drift.
    if drift > 0.01 and speed > 6.0 then
        local toward = clamp(-inp.steer * -drift_dir, -1.0, 1.0)  -- + = steering into the drift
        local target = drift_dir * radians(hd.max_drift_angle) * clamp(0.1 + 0.9 * toward, 0.0, 1.0)
        ay = ay + -hd.drift_assist * drift * pol.drift_hold_gain * (target - beta)
        -- Steering released or countersteered: kill the rotation so the car straightens crisply.
        if toward < 0.15 then
            ay = ay - pol.drift_straighten_gain * drift * (yaw_rate - vx * math.tan(delta) / wheelbase)
        end
        ax_out = ax_out + drift * hd.drift_assist * inp.throttle * pol.drift_throttle_gain * math.cos(beta)
    end
    -- Slip-rate damping: no pendulum overshoot when the rear steps out (strongest in drifts).
    local beta_rate = (beta - st.prev_beta) / h
    st.prev_beta = beta
    if speed > 5.0 then
        ay = ay + (0.6 + pol.drift_rate_gain * drift * hd.drift_assist) * clamp(beta_rate, -6.0, 6.0)
    end
    -- Never spin out (drift or grip): strong correction toward the velocity past the limit.
    local limit = radians(hd.max_drift_angle + 12.0)
    if speed > 5.0 and math.abs(beta) > limit then
        ay = ay + pol.anti_spin_gain * (math.abs(beta) - limit) * (beta > 0.0 and 1.0 or -1.0) - 2.0 * yaw_rate
    end
    -- Grip-mode stability (arcade ESC): catch unintended oversteer outside drift mode.
    if speed > 5.0 then
        ay = ay + pol.esc_gain * (1.0 - drift) * clamp(beta, -0.5, 0.5)
            * clamp((math.abs(beta) - 0.03) / 0.08, 0.0, 1.0)
    end
    -- Yaw damping in grip driving when the steering is centred.
    if drift < 0.2 and math.abs(inp.steer) < 0.1 then
        ay = ay - hd.yaw_damping * 4.0 * (yaw_rate - vx * math.tan(delta) / wheelbase)
    end
    return ay, ax_out
end

-- Arcade velocity swing (NFS 2015): while drifting the velocity turns toward the nose at a fixed rate,
-- speed preserved. Takes and returns vx, vy.
function M.swing(st, pol, hd, h, tel, inp, vx, vy)
    if st.drift > 0.01 and vx > 6.0 then
        local sp = math.sqrt(vx * vx + vy * vy)
        local b = math.atan(vy, vx)
        local steer_into = clamp(inp.steer * st.drift_sign, 0.0, 1.0)
        local rate = hd.drift_assist * st.drift * (1.1 + 0.9 * inp.throttle) * (0.25 + 0.75 * steer_into)
        local step = math.min(math.abs(b), rate * h)
        local nb = b - (b < 0.0 and -step or step)
        vx = sp * math.cos(nb)
        vy = sp * math.sin(nb)
    end
    return vx, vy
end

function M.teleport(st)
    st.steer = 0.0
    st.drift = 0.0
end

return M

-- NFS-style vehicle controller (project policy for the R33 prototype). The engine calls the phases in
-- this order each fixed sub-step (docs/lua_api.md "Vehicles"):
--   control(tel, inp, act)      nitrous, drift entry, steering, direction, pedals and brakes
--   yaw_assist(tel, inp, io)    drift assist, slip damping, anti-spin, ESC, yaw damping
--   velocity(tel, inp, io)      burnout hold, arcade drift swing
--   post_step(tel, inp, io)     automatic shifts (the rpm math stays native)
-- The formulas live in the slice modules (nitrous, drift, gearbox, burnout); the numbers come from
-- params.handling (the scene's handling profile) and params.policy (projects/nox-racing/data/vehicles).
local nitrous = require("vehicle.nitrous")
local gearbox = require("vehicle.gearbox")
local drift = require("vehicle.drift")
local burnout = require("vehicle.burnout")
local util = require("vehicle.util")

local C = {}
C.__index = C

function C.new(params)
    assert(type(params.handling) == "table", "params.handling is required")
    assert(type(params.policy) == "table", "params.policy is required")
    local self = setmetatable({}, C)
    self.hd = params.handling
    self.pol = params.policy
    self.nitro_st = nitrous.new()
    self.gear_st = gearbox.new()
    self.drift_st = drift.new()
    self.burn_st = burnout.new()
    self.debug = {
        nitro = 1.0, nitro_cooldown = 0.0, nitro_active = false,
        drift = 0.0, drift_sign = 1.0, shift_timer = 0.0, shift_event = false,
    }
    return self
end

-- Debug state read by the engine after the phases that change it (facade HUD and telemetry).
local function refresh_debug(self)
    local d = self.debug
    d.nitro = self.nitro_st.nitro
    d.nitro_cooldown = self.nitro_st.cooldown
    d.nitro_active = self.nitro_st.active
    d.drift = self.drift_st.drift
    d.drift_sign = self.drift_st.drift_sign
    d.shift_timer = self.gear_st.shift_timer
    -- One-shot: a shift or a direction change since the last report.
    d.shift_event = self.gear_st.shift_event
    self.gear_st.shift_event = false
end

function C:on_teleport()
    drift.teleport(self.drift_st)
    gearbox.teleport(self.gear_st)
    burnout.teleport(self.burn_st)
    refresh_debug(self)
end

function C:on_respawn()
    nitrous.refuel(self.nitro_st)
    refresh_debug(self)
end

function C:on_hold()
    nitrous.hold(self.nitro_st)
    refresh_debug(self)
end

-- Declared controller state (docs/lua_api.md "Vehicles", state hooks). A reload exports it from the running VM and
-- restores it into the candidate VM, so the car continues with the same nitrous, drift, gear and assist values.
-- Every slice table holds plain data only.
function C:on_refuel()
    nitrous.refuel(self.nitro_st)
    refresh_debug(self)
end

function C:state()
    return { nitro = self.nitro_st, gear = self.gear_st, drift = self.drift_st, burn = self.burn_st }
end

function C:restore(t)
    for _, key in ipairs({ "nitro", "gear", "drift", "burn" }) do
        assert(type(t[key]) == "table", "controller state '" .. key .. "' missing")
    end
    local targets = { nitro = self.nitro_st, gear = self.gear_st, drift = self.drift_st, burn = self.burn_st }
    for key, dst in pairs(targets) do
        for k in pairs(dst) do dst[k] = nil end
        for k, v in pairs(t[key]) do dst[k] = v end
    end
    refresh_debug(self)
end

function C:control(tel, inp, act)
    local h = tel.h
    nitrous.step(self.nitro_st, self.hd, h, tel.vx, inp)
    drift.update(self.drift_st, self.pol, self.hd, h, tel, inp)
    local delta = drift.steering(self.drift_st, self.pol, self.hd, h, tel, inp)
    local gear = gearbox.direction(self.gear_st, self.pol, self.hd, h, tel.gear, tel.vx, inp)

    act.gear = gear
    act.steer_angle = delta
    act.nitro_drive_scale = nitrous.drive_scale(self.nitro_st, self.hd)
    act.drive_shift_cut = self.gear_st.shift_timer > 0.0
    act.drag_scale = nitrous.drag_scale(self.nitro_st, self.hd)
    act.rear_grip_scale = util.mix(1.0, self.hd.drift_grip_scale, self.drift_st.drift)
    burnout.pedals(self.burn_st, self.pol, tel, inp, gear, act)
    refresh_debug(self)
end

function C:yaw_assist(tel, inp, io)
    local ay, ax = drift.yaw_assist(self.drift_st, self.pol, self.hd, tel.h, tel, inp, io.yaw_acc, io.ax)
    io.yaw_acc = ay
    io.ax = ax
end

function C:velocity(tel, inp, io)
    local vx = burnout.velocity(self.burn_st, tel.h, io.vx)
    local vx2, vy2 = drift.swing(self.drift_st, self.pol, self.hd, tel.h, tel, inp, vx, io.vy)
    io.vx = vx2
    io.vy = vy2
end

function C:post_step(tel, inp, io)
    local gear, shifting = gearbox.post_step(self.gear_st, self.pol, self.hd, tel, inp, io.gear)
    io.gear = gear
    io.gear_shifting = shifting
    refresh_debug(self)
end

return C

-- Editor panel "Drive" (Lua port of DrawDriveTools in src/GameEditorPanels.cpp). Handling, nitrous, spawn and the
-- chase camera. Authored values live in the scene's "game" object (game.handling, game.spawn, game.camera);
-- the vehicle definition (data/vehicles/r33.json) supplies the values a scene does not override.
-- While playing, edits change the runtime values only (the session applies them to the running car through
-- editor.model.take_runtime_changes); "Apply to authored" keeps them in the scene.
--
-- Native slider ranges are kept (they were the DragFloat ranges of the native panel).

local model = require("editor.model")

-- { key (Lua controller/handling key), label, min, max }
local HANDLING = {
    { "top_speed_kmh", "Top speed (km/h)", 60.0, 400.0 },
    { "drive_accel", "Acceleration (m/s2)", 2.0, 30.0 },
    { "brake_decel", "Braking (m/s2)", 4.0, 40.0 },
    { "steer_lock_low", "Steer lock low (deg)", 10.0, 50.0 },
    { "steer_lock_high", "Steer lock high (deg)", 1.0, 20.0 },
    { "steer_rate_hz", "Steer rate (Hz)", 1.0, 30.0 },
    { "tire_grip", "Grip", 0.5, 20.0 },
    { "drift_grip_scale", "Drift grip", 0.1, 10.0 },
    { "rpm_redline", "Redline (rpm)", 4000.0, 9800.0 },
    { "rpm_limiter", "Limiter (rpm)", 4500.0, 10000.0 },
}

local NITROUS = {
    { "nitro_thrust", "Thrust x", 1.0, 3.0 },
    { "nitro_top_speed", "Top speed x", 1.0, 1.5 },
    { "nitro_drain", "Drain /s", 0.02, 1.0 },
    { "nitro_recharge", "Recharge /s", 0.0, 1.0 },
    { "nitro_recharge_delay", "Recharge delay (s)", 0.0, 10.0 },
}

local CAMERA = {
    { "distance", "Distance (m)", 2.0, 12.0 },
    { "height", "Height (m)", 0.4, 5.0 },
    { "look_ahead", "Look ahead (m)", 0.0, 20.0 },
    { "look_height", "Look height (m)", 0.0, 3.0 },
    { "fov", "FOV vertical (deg)", 25.0, 90.0 },
    { "fov_speed_gain", "FOV speed gain", 0.0, 30.0 },
    { "fov_nitro", "FOV nitrous", 0.0, 20.0 },
    { "pull_back", "Pull back (m)", 0.0, 4.0 },
    { "stiffness", "Stiffness", 1.0, 30.0 },
    { "heading_lag", "Heading lag", 0.5, 20.0 },
}

local function rows(ed, group, list)
    for _, r in ipairs(list) do
        local key, label, lo, hi = r[1], r[2], r[3], r[4]
        local cur = (group == "handling") and model.handling(key) or model.camera(key)
        if type(cur) == "number" then
            local nv = ed.slider(label .. "##" .. group .. "_" .. key, cur, lo, hi)
            if model.changed(nv, cur) then model.set_tuning(group, key, nv) end
        else
            ed.text(label .. ": no value (not in the vehicle definition)")
        end
    end
end

local function draw_spawn(ed)
    if model.playing then
        ed.text("Spawn is read when Play starts. Stop to change it.")
        return
    end
    local x, z, hd = model.spawn()
    local nx = ed.input_float("Spawn x##spx", x)
    local nz = ed.input_float("Spawn z##spz", z)
    if model.changed(nx, x) or model.changed(nz, z) then model.set_spawn(nx, nz, nil) end
    local nh = ed.slider("Spawn heading (deg)##sph", hd, -180.0, 180.0)
    if model.changed(nh, hd) then model.set_spawn(nx, nz, nh) end
    if model.car_xz() and ed.button("Spawn = car##spcar") then
        local cx, cz, ch = model.car_xz()
        model.set_spawn(cx, cz, ch or hd, "Set spawn")
    end
end

nox.editor_panel {
    id = "lua.drive",
    title = "Drive (Lua)",
    draw = function(ctx, ed)
        model.bind(ed)
        if not model.game then
            ed.text(model.status)
            return
        end
        if model.status ~= "" then ed.text(model.status) end
        if model.playing then
            ed.text("RUNTIME: edits change the running car only. Stop discards them.")
            if ed.button("Apply to authored##apply") then model.apply_runtime_to_authored() end
        else
            ed.text("Authored: edits go to the scene (Save writes them).")
        end
        if model.car then
            ed.text(string.format("Live: %.0f km/h   gear %s   %s rpm   N2O %s",
                model.car.speed_kmh or 0.0, tostring(model.car.gear), tostring(math.floor(model.car.rpm or 0)),
                tostring(model.car.nitro and math.floor(model.car.nitro * 100) .. "%" or "-")))
        end
        ed.separator()

        ed.tree("HANDLING##hand", function(e) rows(e, "handling", HANDLING) end)
        ed.tree("NITROUS##nos", function(e) rows(e, "handling", NITROUS) end)
        ed.tree("Drift yaw gain##dyg", function(e)
            e.text("Native 'Drift yaw gain' has no Lua controller key yet (request R-E3).")
        end)
        ed.tree("SPAWN##spawn", function(e) draw_spawn(e) end)
        ed.tree("CHASE CAMERA##cam", function(e) rows(e, "camera", CAMERA) end)
        ed.tree("HEADLIGHTS##hl", function(e)
            e.text("Not ported: light intensity writes need a native component binding (request R-E4).")
        end)
    end,
}

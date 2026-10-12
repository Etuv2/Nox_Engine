-- Camera, effects and save-data demo (docs/lua_api.md "Camera", "Effects", "Save data"). Generic API only.
-- The camera orbits the arena centre; holding accelerate emits tyre-style smoke from the rear; the session count
-- is stored in the user directory and read back on the next Play.

local SAVE_NAME = "sample-arena/stats.json"
local demo = { angle = 0.0, sessions = 0 }

nox.on("begin_play", function(ctx)
  local stats, reason = ctx:load_user_json(SAVE_NAME)
  if stats then
    demo.sessions = stats.sessions or 0
  end
  ctx:log("sessions so far: " .. tostring(demo.sessions), reason or "")
end)

nox.on("end_play", function(ctx)
  ctx:audio_set({ active = false })
  demo.sessions = demo.sessions + 1
  local ok, reason = ctx:save_user_json(SAVE_NAME, { sessions = demo.sessions })
  if not ok then ctx:log("could not save stats: " .. tostring(reason)) end
end)

nox.system{
  name = "orbit_camera",
  order = 10,
  update = function(ctx, dt)
    demo.angle = demo.angle + dt * 0.25
    local radius = 12.0
    ctx:camera_look_at(math.cos(demo.angle) * radius, 5.0, math.sin(demo.angle) * radius, 0.0, 1.0, 0.0)
    ctx:camera_set_fov(60.0)
  end,
}

nox.system{
  name = "exhaust",
  order = 20,
  update = function(ctx, dt)
    if ctx:action_down("accelerate") then
      ctx:fx_smoke(0.0, 0.3, -2.0, -1.0, 0.5, 0.0, 0.8, dt)
    end
  end,
}

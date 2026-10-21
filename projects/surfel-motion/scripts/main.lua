-- Surfel GI motion test: moves the "Mover" node sideways on a sine path each frame (test scene only).
-- The camera is held at the scene's authored pose (the project would otherwise leave the default camera).
local amplitude = 0.7
local omega = 2.0 * math.pi * 0.25

nox.system({
    name = "surfel_camera",
    order = 5,
    update = function(ctx, dt)
        ctx:camera_look_at(0.0, 1.25, 4.25, 0.0, 1.2, -1.0)
        ctx:camera_set_fov(55.0)
    end,
})

nox.system({
    name = "surfel_mover",
    order = 10,
    update = function(ctx, dt)
        local mover = ctx:find("Mover")
        if not mover then return end
        local t = ctx:time()
        ctx:move(mover, amplitude * math.sin(omega * t), 0.75, -0.2)
    end,
})

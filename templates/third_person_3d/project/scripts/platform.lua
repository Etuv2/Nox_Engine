-- Moving platform: a model with a StaticCollision component (scenes/main.json, "Moving Platform"). Its collider follows
-- the node every frame, and a character standing on it is carried along by the engine (docs/physics.md, "Moving
-- platforms"). The script only moves the node: a slide back and forth along X.

local PLATFORM = "Moving Platform"
local AMPLITUDE = 3.0     -- metres either side of the start position
local PERIOD = 8.0        -- seconds for one full swing

local elapsed = 0.0

nox.on("begin_play", function(ctx)
  elapsed = 0.0
end)

nox.system{
  name = "moving_platform",
  order = 2,
  update = function(ctx, dt)
    local platform = ctx:find(PLATFORM)
    if not platform then return end
    elapsed = elapsed + dt
    local x = AMPLITUDE * math.sin(2.0 * math.pi * elapsed / PERIOD)
    ctx:component(platform, "Transform").position = { x = x, y = 0.75, z = -6.0 }
  end,
}

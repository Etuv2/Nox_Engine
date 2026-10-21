-- Animator demo (docs/animation.md). Lua drives the sharks' Animator parameters every frame: "speed" sweeps the
-- 1D blend between circling and swimming, and "circling" toggles a smoothstep crossfade into the circling state.
-- Marker events ("splash", at the middle of each swim cycle) arrive through nox.on("anim.event").
-- Sharks are named Shark_00 .. Shark_19; scenes may use any subset of them. A shark whose Animator has no graph
-- (the perf scene's legacy sharks) only plays the swim clip.

local SHARK_COUNT = 20
local SWIM_CLIP = "Shark Rig|swimming_skeletal.3"

local t = 0
local splashes = 0
local lastState = nil

nox.on("begin_play", function(ctx)
  for i = 0, SHARK_COUNT - 1 do
    local h = ctx:find(string.format("Shark_%02d", i))
    if h then
      local anim = ctx:component(h, "Animator")
      if anim and anim.graph == "" then
        anim:play(SWIM_CLIP)
      end
    end
  end
end)

nox.system{
  name = "shark_driver",
  update = function(ctx, dt)
    t = t + dt
    local speed = 0.5 + 0.5 * math.sin(t * 0.8)
    local circling = (math.floor(t / 4) % 2) == 1
    for i = 0, SHARK_COUNT - 1 do
      local h = ctx:find(string.format("Shark_%02d", i))
      if h then
        local anim = ctx:component(h, "Animator")
        if anim and anim.graph ~= "" then
          anim:set_float("speed", speed)
          anim:set_bool("circling", circling)
          if i == 0 and anim.state ~= lastState then
            lastState = anim.state
            ctx:log("Shark_00 state", lastState, "at play time", string.format("%.2f", t))
          end
        end
      end
    end
  end,
}

nox.on("anim.event", function(ctx, e)
  splashes = splashes + 1
  if splashes <= 6 or splashes % 50 == 0 then
    ctx:log("anim.event", e.name, "clip", e.clip, "phase", string.format("%.2f", e.phase), "total", splashes)
  end
end)

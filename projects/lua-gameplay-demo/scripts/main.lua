-- Lua gameplay runtime demo (docs/lua_api.md, "Lifecycle, timers and coroutines" and "Scenes and saves").
-- Every line it prints goes to the engine log with the [Lua] prefix.

-- A behaviour with on_start and on_destroy. The coin is created at runtime with ctx:instantiate.
nox.behavior{
  name = "coin",
  on_start = function(ctx, e)
    ctx:log("coin on_start (once), entity", ctx:name(e))
  end,
  update = function(ctx, e, dt)
    coinFrames = (coinFrames or 0) + 1
  end,
  on_destroy = function(ctx, e)
    ctx:log("coin on_destroy after", coinFrames or 0, "update frames")
  end,
}

nox.on("begin_play", function(ctx, payload)
  ctx:log("begin_play at frame", ctx:frame(), "payload origin", payload and payload.origin or "none")

  -- A repeating timer that cancels itself after three ticks (0.5 s apart on the play clock).
  local ticks = 0
  local heartbeat
  heartbeat = ctx:every(0.5, function(c)
    ticks = ticks + 1
    c:log("heartbeat", ticks, "play time", string.format("%.2f", c:time()), "frame", c:frame())
    if ticks == 3 then heartbeat:cancel() end
  end)

  -- A one-shot timer.
  ctx:after(1.0, function(c)
    c:log("one-shot after 1.0 s at frame", c:frame())
  end)

  -- A coroutine: wait, create a coin, wait a while, destroy it, then switch scene with a payload.
  ctx:start_coroutine(function(c)
    c:wait(0.5)
    local coin = c:instantiate("projects/lua-gameplay-demo/prefabs/coin.prefab.json",
      { name = "Coin", position = { 0, 1.5, 0 } })
    c:attach(coin, "coin")
    c:log("coin instantiated at frame", c:frame())
    c:wait_frames(120)
    c:destroy(coin)
    c:log("coin destroyed at frame", c:frame())
    c:wait(0.5)
    c:log("loading scenes/lua_gameplay_demo_next.json with a payload")
    c:load_scene("scenes/lua_gameplay_demo_next.json",
      { origin = "lua-gameplay-demo", level = 2, tags = { "intro", "arena" } })
  end)
end)

nox.on("end_play", function(ctx)
  ctx:log("end_play at frame", ctx:frame())
end)

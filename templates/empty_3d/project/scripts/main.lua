-- {{NAME}}: Empty 3D template (docs/lua_api.md, "Lifecycle").
-- The entry script runs once for each configuration of the scene. begin_play runs at Play.

nox.on("begin_play", function(ctx, payload)
  ctx:log("{{NAME}}: begin_play at frame", ctx:frame())
end)

nox.on("end_play", function(ctx)
  ctx:log("{{NAME}}: end_play at frame", ctx:frame())
end)

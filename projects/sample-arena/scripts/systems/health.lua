-- Every two seconds of play (120 frames at 60 Hz) the player loses one health point.
-- Reaching zero emits "defeated" once; the event is delivered after all systems ran.
nox.system({
    name = "health",
    order = 20,
    update = function(ctx, dt)
        if ctx:frame() % 120 ~= 0 then return end
        local player = ctx:find("Player")
        if not player then return end
        local health = ctx:get(player, "Health")
        if not health or health.current <= 0 then return end
        ctx:set_field(player, "Health", "current", health.current - 1)
        if health.current - 1 == 0 then
            ctx:emit("defeated", { entity = player })
        end
    end,
})
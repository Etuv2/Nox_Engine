-- Moves the "Mover" node on a circle each frame. Positions are set through the host (ctx:move).
local Menu = require("menu")

nox.system({
    name = "mover",
    order = 10,
    update = function(ctx, dt)
        if Menu.blocking() then return end
        local mover = ctx:find("Mover")
        if not mover then return end
        local t = ctx:time()
        ctx:move(mover, 3.0 * math.cos(t), 2.0, 3.0 * math.sin(t))
    end,
})
-- Reacts to "defeated" (delivered at the end of the frame) and records the quest outcome.
nox.on("defeated", function(ctx, payload)
    local player = payload.entity
    if not ctx:is_valid(player) then return end
    ctx:set_field(player, "Quest", "state", "failed")
    ctx:set_field(player, "Quest", "progress", 1.0)
    ctx:log("quest failed: player defeated")
end)
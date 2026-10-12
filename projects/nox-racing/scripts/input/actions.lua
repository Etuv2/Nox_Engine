-- Input sampling: one latch per frame from the action map (projects/nox-racing/input.json). Every system
-- of the frame reads the same sample. Driving values come from axes/buttons; menu and map keys are edges.
local M = {}

local function axis01(ctx, name)
    local v = ctx:axis(name)
    if v ~= v then return 0.0 end
    return math.max(0.0, math.min(1.0, v))
end

-- Returns drive = {throttle, brake, steer, handbrake, nitrous} and btn = {confirm, start, back, pause,
-- reset, camera, map, recenter, up, down} (pressed edges for this frame; up/down are menu steps).
function M.sample(ctx)
    local drive = {
        throttle = axis01(ctx, "throttle"),
        brake = axis01(ctx, "brake"),
        steer = math.max(-1.0, math.min(1.0, ctx:axis("steer"))),
        handbrake = ctx:action_down("handbrake"),
        nitrous = ctx:action_down("nitrous"),
    }
    local btn = {
        confirm = ctx:action_pressed("confirm"),
        start = ctx:action_pressed("start_event"),
        back = ctx:action_pressed("back"),
        pause = ctx:action_pressed("pause"),
        reset = ctx:action_pressed("reset"),
        camera = ctx:action_pressed("camera"),
        map = ctx:action_pressed("map"),
        recenter = ctx:action_pressed("recenter"),
        up = ctx:action_pressed("menu_up"),
        down = ctx:action_pressed("menu_down"),
    }
    return drive, btn
end

-- Neutral drive input (no throttle, no brake, centred steering).
function M.neutral()
    return { throttle = 0.0, brake = 0.0, steer = 0.0, handbrake = false, nitrous = false }
end

return M

-- Viewport authoring overlay (Lua port of DrawRacingAuthoringOverlay, handles only): spawn (green), event starts
-- (lavender), checkpoints (cyan; the finish yellow) and road-network nodes (blue). Drag a handle on the ground
-- plane; the engine records one undo step per drag. The overlay is on by default (model.overlay_on).
--
-- Positions: the engine owns a handle's position while it is dragged, and it is the authored point after an undo
-- or redo. This file keeps the authored copy (editor.model) in step with the handles in both directions:
--   handle moved by the engine (drag, undo, redo)  -> model takes the handle position
--   model moved by a panel edit                    -> ed.handle_set moves the handle
-- Gate lines, routes, road edges and labels are not drawn yet (no line or text binding in ed; request R-E2).

local model = require("editor.model")

local TOL = 1e-3
local last = {}   -- handle id -> { x, z } as of the previous frame

-- One handle. write(x, z) stores a position the engine moved; the model's current point is (x, z).
local function point(ed, id, x, y, z, opts, write)
    local dragging, hx, hy, hz = ed.handle(id, x, y, z, opts)
    local prev = last[id]
    if prev == nil then
        if math.abs(hx - x) > TOL or math.abs(hz - z) > TOL then
            ed.handle_set(id, x, y, z)   -- stale point from an earlier entry with this id
            hx, hz = x, z
        end
    elseif math.abs(hx - prev.x) > TOL or math.abs(hz - prev.z) > TOL then
        write(hx, hz)                    -- the engine moved it (drag, undo or redo)
        x, z = hx, hz
    elseif math.abs(x - prev.x) > TOL or math.abs(z - prev.z) > TOL then
        ed.handle_set(id, x, y, z)       -- a panel edit moved the model point
        hx, hz = x, z
    end
    last[id] = { x = hx, z = hz }
    return dragging
end

local COL = {
    road = 0x6E78AAFF,           -- IM_COL32(120, 170, 255, 110)
    spawn = 0xFF5AFF8C,          -- IM_COL32(90, 255, 140)
    start_on = 0xFFCDA0FF, start_off = 0x96CDA0FF,   -- lavender (selected / other events)
    cp_on = 0xFF50DCFF, cp_off = 0x8C50DCFF,         -- cyan
    finish_on = 0xFFFFE65A, finish_off = 0x8CFFE65A, -- yellow
}

local function draw_roads(ed)
    local net = model.road_network()
    if type(net) ~= "table" or type(net.nodes) ~= "table" then return end
    if net.file then return end   -- file-backed networks are not edited as handles
    for k, n in ipairs(net.nodes) do
        local x, z = model.read_xz(n.pos)
        if x then
            point(ed, "road:" .. tostring(n.id), x, 0.05, z, { radius = 5, color = COL.road },
                function(hx, hz) model.poke_road_node(k, hx, hz) end)
        end
    end
end

local function draw_spawn(ed)
    local x, z = model.spawn()
    point(ed, "spawn", x, 0.1, z, { radius = 6, color = COL.spawn }, function(hx, hz) model.poke_spawn(hx, hz) end)
end

local function draw_events(ed)
    local events = model.events()
    for i, e in ipairs(events) do
        local selected = (i == model.selected)
        local sx, sz = model.read_xz(e.start)
        if sx then
            point(ed, "ev:" .. i .. ":start", sx, 0.1, sz, { radius = 6, color = selected and COL.start_on or COL.start_off },
                function(hx, hz) model.poke_event_start(i, hx, hz) end)
        end
        local cps = e.checkpoints or {}
        for j, cp in ipairs(cps) do
            local cx, cz = model.read_xz(cp.pos or cp)
            if cx then
                local finish = j == #cps
                local color
                if finish then color = selected and COL.finish_on or COL.finish_off
                else color = selected and COL.cp_on or COL.cp_off end
                point(ed, "ev:" .. i .. ":cp:" .. j, cx, 0.1, cz, { radius = 5, color = color },
                    function(hx, hz) model.poke_checkpoint(i, j, hx, hz) end)
            end
        end
    end
end

nox.editor_viewport(function(ctx, ed)
    model.bind(ed)
    if not model.loaded or not model.game or not model.overlay_on then return end
    draw_roads(ed)
    draw_spawn(ed)
    draw_events(ed)
end)

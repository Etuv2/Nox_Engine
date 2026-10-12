-- Editor panel "Race & Routes" (Lua port of DrawRaceTools in src/GameEditorPanels.cpp): events, ordered
-- checkpoints, test buttons, the speedwall (save data) and the road network. Event data is read-only while
-- playing (native semantics). Every edit is one undo step through editor.model.

local model = require("editor.model")

local function changed(new, old) return model.changed(new, old) end

local function text_field(ed, label, value)
    return ed.input_text(label, value or "")
end

-- Checkpoint row (editable). i = event index, j = checkpoint index.
local function draw_checkpoint(ed, i, j, cp, count)
    local x, z = model.read_xz(cp.pos or cp)
    x, z = x or 0.0, z or 0.0
    local key = i .. "_" .. j
    ed.text(string.format("CP %d%s", j, j == count and " (finish)" or ""))
    ed.same_line()
    local nx = ed.input_float("x##cpx" .. key, x)
    ed.same_line()
    local nz = ed.input_float("z##cpz" .. key, z)
    if changed(nx, x) or changed(nz, z) then model.set_checkpoint_xz(i, j, nx, nz) end
    ed.same_line()
    local w = cp.width or 18.0
    local nw = ed.slider("width##cpw" .. key, w, 4.0, 60.0)
    if changed(nw, w) then model.set_checkpoint_field(i, j, "width", nw) end
    local auto = cp.heading_deg == nil
    local na = ed.checkbox("auto##cpa" .. key, auto)
    if na ~= auto then
        if na then model.set_checkpoint_field(i, j, "heading_deg", nil)
        else model.set_checkpoint_field(i, j, "heading_deg", cp.heading_deg or 0.0) end
    end
    if not na then
        local h = cp.heading_deg or 0.0
        local nh = ed.slider("heading##cph" .. key, h, -180.0, 180.0)
        if changed(nh, h) then model.set_checkpoint_field(i, j, "heading_deg", nh) end
    end
    ed.same_line()
    if ed.button("up##cpu" .. key) and j > 1 then model.move_checkpoint(i, j, -1) end
    ed.same_line()
    if ed.button("dn##cpd" .. key) and j < count then model.move_checkpoint(i, j, 1) end
    ed.same_line()
    if ed.button("dup##cpp" .. key) then model.duplicate_checkpoint(i, j) end
    ed.same_line()
    if ed.button("del##cpx" .. key) and count > 1 then model.delete_checkpoint(i, j) end
end

local function draw_event(ed, i, e, editable)
    local key = tostring(i)
    if not editable then
        ed.text(string.format("id %s   type %s   name %s   difficulty %s", tostring(e.id), tostring(e.type), tostring(e.name), tostring(e.difficulty)))
        local x, z = model.read_xz(e.start)
        ed.text(string.format("start (%.2f, %.2f)   heading %.0f   trigger %.1f m   countdown %.1f s",
            x or 0.0, z or 0.0, e.start_heading_deg or 0.0, e.trigger_radius or 9.0, e.countdown or 3.0))
        return
    end
    local id = text_field(ed, "Id##evid" .. key, e.id)
    if id ~= e.id then model.set_event_field(i, "id", id) end
    local tp = text_field(ed, "Type##evty" .. key, e.type)
    if tp ~= e.type then model.set_event_field(i, "type", tp) end
    local nm = text_field(ed, "Name##evnm" .. key, e.name)
    if nm ~= e.name then model.set_event_field(i, "name", nm) end
    local df = text_field(ed, "Difficulty##evdf" .. key, e.difficulty)
    if df ~= e.difficulty then model.set_event_field(i, "difficulty", df) end

    local x, z = model.read_xz(e.start)
    x, z = x or 0.0, z or 0.0
    local nx = ed.input_float("Start x##evsx" .. key, x)
    local nz = ed.input_float("Start z##evsz" .. key, z)
    if changed(nx, x) or changed(nz, z) then model.set_event_start(i, nx, nz) end
    local sh = e.start_heading_deg or 0.0
    local nsh = ed.slider("Start heading##evsh" .. key, sh, -180.0, 180.0)
    if changed(nsh, sh) then model.set_event_field(i, "start_heading_deg", nsh) end
    local tr = e.trigger_radius or 9.0
    local ntr = ed.slider("Trigger radius (m)##evtr" .. key, tr, 2.0, 40.0)
    if changed(ntr, tr) then model.set_event_field(i, "trigger_radius", ntr) end
    local cd = e.countdown or 3.0
    local ncd = ed.slider("Countdown (s)##evcd" .. key, cd, 1.0, 10.0)
    if changed(ncd, cd) then model.set_event_field(i, "countdown", ncd) end

    if model.car_xz() then
        if ed.button("Start = car position##evcar" .. key) then model.set_event_start_from_car() end
        ed.same_line()
    end
    if ed.button("Delete event##evdel" .. key) then
        model.delete_event()
        return
    end

    ed.separator()
    ed.text("CHECKPOINTS (ordered; the last one is the finish)")
    local cps = e.checkpoints or {}
    for j, cp in ipairs(cps) do
        draw_checkpoint(ed, i, j, cp, #cps)
    end
    if model.car_xz() then
        if ed.button("+ Checkpoint at car##cpadd" .. key) then model.add_checkpoint_at_car() end
    else
        ed.text("(+ Checkpoint at car needs the car: Play the scene)")
    end
end

local function draw_roads(ed, editable)
    local net = model.road_network()
    if type(net) ~= "table" then
        ed.text("No road_network in the scene: routes use no road graph.")
        return
    end
    local hw = net.road_half_width or 7.5
    local sw = net.sidewalk_width or 4.25
    local nhw = ed.slider("Road half width (m)##rhw", hw, 2.0, 15.0)
    if editable and changed(nhw, hw) then model.set_road_width("road_half_width", nhw) end
    local nsw = ed.slider("Sidewalk width (m)##rsw", sw, 0.0, 10.0)
    if editable and changed(nsw, sw) then model.set_road_width("sidewalk_width", nsw) end
    for k, n in ipairs(net.nodes or {}) do
        local x, z = model.read_xz(n.pos)
        x, z = x or 0.0, z or 0.0
        ed.text(tostring(n.id))
        ed.same_line()
        local ax = ed.input_float("x##rnx" .. k, x)
        ed.same_line()
        local az = ed.input_float("z##rnz" .. k, z)
        if editable and (changed(ax, x) or changed(az, z)) then model.set_road_node(k, ax, az) end
    end
    ed.text(string.format("%d nodes, %d streets. Edits change the authored network; routes and collision read it at the next load.",
        #(net.nodes or {}), #(net.edges or {})))
end

local function draw_speedwall(ctx, ed, events, editable)
    if ed.button("Refresh best times##swr") then model.load_best(ctx) end
    ed.same_line()
    if editable and ed.button("Clear best times##swc") then model.clear_best(ctx) end
    for _, e in ipairs(events) do
        ed.text(string.format("%s   %s", tostring(e.id), model.fmt_time(model.best_time(e.id))))
    end
end

nox.editor_panel {
    id = "lua.race",
    title = "Race & Routes (Lua)",
    draw = function(ctx, ed)
        model.bind(ed)
        if not model.game then
            ed.text(model.status)
            return
        end
        local editable = not model.playing
        model.overlay_on = ed.checkbox("Show authoring handles (viewport)##ovl", model.overlay_on)
        if model.status ~= "" then ed.text(model.status) end
        ed.text(model.playing and "Playing: event data is read-only (Stop to edit). Test buttons below." or "Stopped: edits go to the scene (Save writes them).")
        if not model.has_engine_undo() then
            if ed.button("Undo##lu") then model.undo_local() end
            ed.same_line()
            if ed.button("Redo##lr") then model.redo_local() end
        end
        ed.separator()

        local events = model.events()
        ed.text("EVENTS")
        for i, e in ipairs(events) do
            local mark = (i == model.selected) and "> " or "  "
            if ed.button(string.format("%s%s  (%s)##evsel%d", mark, tostring(e.name), tostring(e.id), i)) then
                model.selected = i
            end
        end
        if editable then
            if ed.button("+ Event##evadd") then model.add_event() end
            ed.same_line()
            if ed.button("Duplicate##evdup") then model.duplicate_event() end
        end

        local selected, si = model.selected_event()
        if selected then
            ed.separator()
            ed.text(string.format("Selected: event %d of %d", si, #events))
            draw_event(ed, si, selected, editable)
        end

        ed.separator()
        ed.text("TEST")
        if selected then
            if not model.playing then
                if ed.button("Play this event##evplay") then
                    model.request("play", { index = model.selected })
                end
            else
                if ed.button("Restart event##evrst") then
                    model.request("start_event", { index = model.selected })
                end
                ed.same_line()
                local sx, sz = model.read_xz(selected.start)
                if sx and ed.button("Teleport car to start##evtp") then
                    model.request("teleport", { x = sx, z = sz, heading_deg = selected.start_heading_deg or 0.0 })
                end
            end
        end
        if model.race then
            ed.text(string.format("Phase: %s   time %s   next CP %d",
                tostring(model.race.phase_name or "free roam"), model.fmt_time(model.race.time), (model.race.next_cp or 0) + 1))
        end

        ed.separator()
        ed.tree("Speedwall (save data)##sw", function(e) draw_speedwall(ctx, e, events, editable) end)
        ed.tree("Road network##roads", function(e) draw_roads(e, editable) end)
        ed.separator()
        ed.text("Viewport handles: spawn (green), event starts (lavender), checkpoints (cyan, finish yellow), road nodes (blue).")
    end,
}

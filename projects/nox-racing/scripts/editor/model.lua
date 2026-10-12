-- Authoring model for the racing editor panels (Lua port of GameEditorPanels.cpp: Race & Routes, Drive, the
-- authoring overlay). The model owns a copy of the scene's "game" object (scripts/race/AUTHORING.md), taken with
-- nox.game() at the first editor callback (nox.on_configure is not used: it breaks the project load in the current
-- build, see requests_for_lead.md). Every edit lands in that copy, and nox.on_save_game returns it, so the editor
-- Save writes it into the scene.
--
-- Edits go through M.edit(label, key, fn): one undo step per edit (consecutive edits with the same key within
-- 30 frames merge, e.g. typing in a field). Undo uses the engine's stack (ed.undo) when the binding provides it;
-- otherwise the panel's own Undo/Redo buttons use a local stack (see docs/nfs2015/... request R-E1).
--
-- Runtime values: while playing, Drive edits change the runtime overrides only (M.runtime). The project's session
-- takes them with M.take_runtime_changes() and applies them to the running car. Stop discards them; "Apply to
-- authored" copies them into the authored game object.
--
-- Requests to the session (play, start an event, teleport) are queued here; the project's main module takes them
-- with M.take_requests(). Nothing in this file touches the world directly: editor code cannot write world state.

local chase_ok, chase = pcall(require, "camera.chase")

local M = {}

local VEHICLE_DEF = "projects/nox-racing/data/vehicles/r33.json"
local SPEEDWALL = "speedwall.json"

M.frame = 0
M.loaded = false          -- true once the scene's game object was read (nox.game)
M.game = nil              -- authored game object (copy); nil when the scene has none
M.status = ""
M.overlay_on = true       -- viewport authoring handles (on by default)
M.playing = false
M.ed = nil                -- the `ed` table of the current callback (set by M.bind)
M.car = nil               -- car telemetry while playing: { x, z, heading_deg, speed_kmh, ... } (published by the session)
M.race = nil              -- race progress published by the session (see publish_race)
M.best = nil              -- speedwall best times (save data), loaded on request
M.selected = 1            -- selected event (1-based)
M.runtime = { handling = {}, camera = {} }

local runtime_changes = {}   -- key -> value for the running car, consumed by take_runtime_changes
local requests = {}          -- queued session requests, consumed by take_requests
local undo_stack, redo_stack = {}, {}  -- local fallback stack: records { label, before, after }
local last_edit = { key = nil, frame = -100, rec = nil }
local defs_cache = nil
local generation = 0      -- bumped on every load: undo records of an older world do nothing

-- ---------------------------------------------------------------------------------------------
-- Small helpers
-- ---------------------------------------------------------------------------------------------

local function copy(v)
    if type(v) ~= "table" then return v end
    local out = {}
    for k, x in pairs(v) do out[k] = copy(x) end
    return out
end
M.copy = copy

local function same(a, b)
    if type(a) ~= type(b) then return false end
    if type(a) ~= "table" then return a == b end
    for k, v in pairs(a) do if not same(v, b[k]) then return false end end
    for k in pairs(b) do if a[k] == nil then return false end end
    return true
end

-- Widgets return float32 values, so compare with a tolerance (a change is a real edit).
function M.changed(new, old)
    if type(new) ~= "number" or type(old) ~= "number" then return new ~= old end
    return math.abs(new - old) > 1e-3 * math.max(1.0, math.abs(old))
end

-- Point forms accepted by the session: [x, z], [x, y, z] (y in the middle) and {x=, z=, y=}.
function M.read_xz(p)
    if type(p) ~= "table" then return nil end
    if p.x ~= nil then return p.x, p.z, p.y end
    if #p >= 3 then return p[1], p[3], p[2] end
    if #p == 2 then return p[1], p[2], nil end
    return nil
end

local function write_xz(p, x, z)
    if p.x ~= nil then p.x, p.z = x, z
    elseif #p >= 3 then p[1], p[3] = x, z
    else p[1], p[2] = x, z end
end

local function fmt_time(t)
    if type(t) ~= "number" or t < 0 then return "--:--.--" end
    local m = math.floor(t / 60)
    return string.format("%d:%05.2f", m, t - m * 60)
end
M.fmt_time = fmt_time

-- ---------------------------------------------------------------------------------------------
-- Vehicle definition (defaults shown where the scene has no override)
-- ---------------------------------------------------------------------------------------------

function M.definition()
    if defs_cache ~= nil then return defs_cache end
    defs_cache = false
    if type(nox) == "table" and type(nox.json) == "function" then
        local ok, def = pcall(nox.json, VEHICLE_DEF)
        if ok and type(def) == "table" then defs_cache = def end
    end
    return defs_cache
end

-- Shipped handling for one key (controller params first, then the shared tuning keys).
local function default_handling(key)
    local def = M.definition()
    if not def then return nil end
    local hp = def.controller and def.controller.params and def.controller.params.handling
    if hp and hp[key] ~= nil then return hp[key] end
    if def.tuning and def.tuning[key] ~= nil then return def.tuning[key] end
    return nil
end

local function default_camera(key)
    if chase_ok and chase then return chase.rig(nil)[key] end
    return nil
end

-- ---------------------------------------------------------------------------------------------
-- Loading and the save hook
-- ---------------------------------------------------------------------------------------------

-- Reads the authored copy (once per project world; the editor callbacks call M.ensure_loaded).
function M.load(game)
    generation = generation + 1
    M.loaded = true
    M.game = type(game) == "table" and copy(game) or nil
    undo_stack, redo_stack = {}, {}
    last_edit = { key = nil, frame = -100, rec = nil }
    runtime_changes = {}
    M.runtime = { handling = {}, camera = {} }
    M.status = M.game and "" or "This scene has no \"game\" section (no authored race data)."
end

-- Takes the scene's game object on first use (nox.game() returns a copy of the scene's data).
function M.ensure_loaded()
    if M.loaded then return end
    local game = nil
    if type(nox) == "table" and type(nox.game) == "function" then
        local ok, value = pcall(nox.game)
        if ok then game = value end
    end
    M.load(game)
end

-- The scene's game object for SaveGameJson (nox.on_save_game). Fails when the scene has none.
function M.document()
    M.ensure_loaded()
    if not M.game then error("the scene has no game object to save") end
    return M.game
end

if type(nox) == "table" and type(nox.on_save_game) == "function" then
    nox.on_save_game(function() return M.document() end)
end

-- ---------------------------------------------------------------------------------------------
-- Per-callback state (called at the top of every panel and viewport callback)
-- ---------------------------------------------------------------------------------------------

function M.bind(ed)
    M.ed = ed
    M.ensure_loaded()
    M.frame = M.frame + 1
    local playing = false
    if ed and type(ed.playing) == "function" then
        local ok, v = pcall(ed.playing)
        playing = ok and v == true
    end
    if playing and not M.playing then
        -- Play started: runtime overrides start empty (the running car reads the authored values).
        M.runtime = { handling = {}, camera = {} }
        runtime_changes = {}
    elseif not playing and M.playing then
        -- Stop: runtime edits are discarded, the car returns to the authored data.
        M.runtime = { handling = {}, camera = {} }
        runtime_changes = {}
        M.car = nil
    end
    M.playing = playing
end

-- The engine undo sink when the binding has one (ed.undo(label, undo_fn, redo_fn)), else nil.
local function engine_undo()
    local ed = M.ed
    if ed and type(ed.undo) == "function" then return ed.undo end
    return nil
end
function M.has_engine_undo() return engine_undo() ~= nil end

-- ---------------------------------------------------------------------------------------------
-- Edits and undo
-- ---------------------------------------------------------------------------------------------

local function apply_record(rec, which)
    if rec.gen ~= generation then return end
    M.game = copy(rec[which])
end

-- Runs fn(game) on the authored copy as one undo step when it changed the data. key (optional) merges
-- consecutive edits of the same widget.
function M.edit(label, key, fn)
    if not M.game then
        M.status = "no authored game object: nothing to edit"
        return false
    end
    local before = copy(M.game)
    fn(M.game)
    if same(before, M.game) then return false end
    local coalesce = key ~= nil and last_edit.key == key and M.frame - last_edit.frame <= 30 and last_edit.rec ~= nil
    if coalesce then
        last_edit.rec.after = copy(M.game)
        last_edit.frame = M.frame
        return true
    end
    local rec = { label = label, before = before, after = copy(M.game), gen = generation }
    local undo = engine_undo()
    if undo then
        undo(label, function() apply_record(rec, "before") end, function() apply_record(rec, "after") end)
    else
        undo_stack[#undo_stack + 1] = rec
        redo_stack = {}
    end
    last_edit = { key = key, frame = M.frame, rec = rec }
    return true
end

-- Local undo/redo (panel buttons). With the engine stack available the engine's Ctrl+Z/Ctrl+Y do this.
function M.undo_local()
    local rec = table.remove(undo_stack)
    if not rec then return false end
    apply_record(rec, "before")
    redo_stack[#redo_stack + 1] = rec
    last_edit = { key = nil, frame = -100, rec = nil }
    return true
end

function M.redo_local()
    local rec = table.remove(redo_stack)
    if not rec then return false end
    apply_record(rec, "after")
    undo_stack[#undo_stack + 1] = rec
    last_edit = { key = nil, frame = -100, rec = nil }
    return true
end

function M.undo_depth() return #undo_stack end
function M.redo_depth() return #redo_stack end

-- ---------------------------------------------------------------------------------------------
-- Events and checkpoints (game.events)
-- ---------------------------------------------------------------------------------------------

function M.events()
    return (M.game and M.game.events) or {}
end

function M.event_count() return #M.events() end

function M.selected_event()
    local evs = M.events()
    if #evs == 0 then return nil end
    if M.selected < 1 then M.selected = 1 end
    if M.selected > #evs then M.selected = #evs end
    return evs[M.selected], M.selected
end

-- The car position (free roam or race) when the session publishes telemetry, else nil.
function M.car_xz()
    local c = M.car
    if not c or type(c.x) ~= "number" then return nil end
    return c.x, c.z, c.heading_deg
end

-- Forward point for a new event when no car position is known: 20 m ahead of the spawn.
local function spawn_ahead(dist)
    local sx, sz, hd = M.spawn()
    local rad = math.rad(hd or 0)
    return sx + math.sin(rad) * dist, sz + math.cos(rad) * dist, hd or 0
end

function M.add_event()
    local label = "Add event"
    local ok = M.edit(label, nil, function(g)
        g.events = g.events or {}
        local n = #g.events + 1
        local x, z, hd = M.car_xz()
        local fwd = 120.0
        if not x then
            x, z, hd = spawn_ahead(20.0)
            fwd = 140.0 - 20.0
        end
        local r = math.rad(hd or 0)
        local fx, fz = x + math.sin(r) * fwd, z + math.cos(r) * fwd
        g.events[n] = {
            id = "event_" .. n, type = "SPRINT RACE", name = "New Sprint", difficulty = "Hard",
            start = { x, z }, start_heading_deg = hd or 0, trigger_radius = 9.0, countdown = 3.0,
            checkpoints = { { pos = { fx, fz }, width = 18.0 } },
        }
    end)
    if ok then M.selected = #M.events() end
    return ok
end

function M.duplicate_event()
    local _, i = M.selected_event()
    if not i then return false end
    local ok = M.edit("Duplicate event", nil, function(g)
        local src = g.events[i]
        local e = copy(src)
        e.id = tostring(e.id or "event") .. "_copy"
        e.name = tostring(e.name or "event") .. " (copy)"
        local x, z = M.read_xz(e.start)
        if x then write_xz(e.start, x + 6.0, z) end
        table.insert(g.events, i + 1, e)
    end)
    if ok then M.selected = i + 1 end
    return ok
end

function M.delete_event()
    local _, i = M.selected_event()
    if not i then return false end
    local ok = M.edit("Delete event", nil, function(g) table.remove(g.events, i) end)
    if ok then M.selected = 1 end
    return ok
end

-- Sets one event field (plain values: id, type, name, difficulty, start_heading_deg, trigger_radius, countdown).
function M.set_event_field(i, field, value)
    return M.edit("Event " .. field, "ev:" .. i .. ":" .. field, function(g)
        g.events[i][field] = value
    end)
end

-- Start point of event i (x, z) keeps the authored point form.
function M.set_event_start(i, x, z, label)
    return M.edit(label or "Move event start", "ev:" .. i .. ":start", function(g)
        local e = g.events[i]
        if type(e.start) ~= "table" then e.start = { x, z } else write_xz(e.start, x, z) end
    end)
end

function M.set_event_start_from_car()
    local _, i = M.selected_event()
    local x, z, hd = M.car_xz()
    if not i or not x then return false end
    local ok = M.edit("Set event start", nil, function(g)
        g.events[i].start = { x, z }
        g.events[i].start_heading_deg = hd or 0
    end)
    return ok
end

-- Checkpoint j of event i: pos {x, z}, width, optional heading_deg (nil = automatic).
function M.checkpoint(i, j)
    local e = M.events()[i]
    return e and e.checkpoints and e.checkpoints[j] or nil
end

function M.checkpoint_xz(i, j)
    local c = M.checkpoint(i, j)
    if not c then return nil end
    return M.read_xz(c.pos or c)
end

function M.set_checkpoint_xz(i, j, x, z, label)
    return M.edit(label or "Move checkpoint", "ev:" .. i .. ":cp:" .. j, function(g)
        local c = g.events[i].checkpoints[j]
        if c.pos == nil then c.pos = { x, z } else write_xz(c.pos, x, z) end
    end)
end

function M.set_checkpoint_field(i, j, field, value)
    return M.edit("Checkpoint " .. field, "ev:" .. i .. ":cp:" .. j .. ":" .. field, function(g)
        local c = g.events[i].checkpoints[j]
        if value == nil then c[field] = nil else c[field] = value end
    end)
end

function M.add_checkpoint_at_car()
    local _, i = M.selected_event()
    local x, z = M.car_xz()
    if not i or not x then return false end
    return M.edit("Add checkpoint", nil, function(g)
        local cps = g.events[i].checkpoints
        cps[#cps + 1] = { pos = { x, z }, width = 18.0 }
    end)
end

-- Duplicate checkpoint j (inserted after it, moved 10 m along +z like the native panel).
function M.duplicate_checkpoint(i, j)
    return M.edit("Duplicate checkpoint", nil, function(g)
        local cps = g.events[i].checkpoints
        local c = copy(cps[j])
        local x, z = M.read_xz(c.pos or c)
        if c.pos then write_xz(c.pos, x, z + 10.0) else c = { pos = { x, z + 10.0 }, width = c.width } end
        table.insert(cps, j + 1, c)
    end)
end

function M.move_checkpoint(i, j, dir)
    local k = j + dir
    return M.edit("Reorder checkpoint", nil, function(g)
        local cps = g.events[i].checkpoints
        if k < 1 or k > #cps then return end
        cps[j], cps[k] = cps[k], cps[j]
    end)
end

function M.delete_checkpoint(i, j)
    return M.edit("Delete checkpoint", nil, function(g)
        local cps = g.events[i].checkpoints
        if #cps > 1 then table.remove(cps, j) end
    end)
end

-- Direct point writes for the viewport handles. The engine owns the drag and its undo step (one per drag), so
-- these do not push undo records. They keep the authored copy in step with the handle positions.
function M.poke_event_start(i, x, z)
    local e = M.events()[i]
    if not e or not M.game then return end
    if type(e.start) ~= "table" then e.start = { x, z } else write_xz(e.start, x, z) end
end

function M.poke_checkpoint(i, j, x, z)
    local c = M.checkpoint(i, j)
    if not c or not M.game then return end
    if c.pos == nil then c.pos = { x, z } else write_xz(c.pos, x, z) end
end

function M.poke_spawn(x, z)
    if not M.game then return end
    M.game.spawn = M.game.spawn or {}
    M.game.spawn.position = { x, z }
end

function M.poke_road_node(k, x, z)
    local net = M.road_network()
    local n = net and net.nodes and net.nodes[k]
    if not n then return end
    if n.pos == nil then n.pos = { x, z } else write_xz(n.pos, x, z) end
end

-- ---------------------------------------------------------------------------------------------
-- Spawn and handling / camera (game.spawn, game.handling, game.camera)
-- ---------------------------------------------------------------------------------------------

-- Effective spawn: authored game.spawn, else the vehicle definition.
function M.spawn()
    local def = M.definition() or {}
    local dsp = def.spawn or {}
    local sp = M.game and M.game.spawn or {}
    local x, z = M.read_xz(sp.position)
    if not x then x, z = dsp.x or 0.0, dsp.z or 0.0 end
    local hd = sp.heading_deg
    if type(hd) ~= "number" then hd = dsp.heading_deg or 0.0 end
    return x, z, hd
end

function M.set_spawn(x, z, heading_deg, label)
    return M.edit(label or "Set spawn", "spawn", function(g)
        g.spawn = g.spawn or {}
        g.spawn.position = { x, z }
        if heading_deg ~= nil then g.spawn.heading_deg = heading_deg end
    end)
end

-- Handling value: runtime override (while playing) > authored override > vehicle definition.
function M.handling(key)
    if M.playing and M.runtime.handling[key] ~= nil then return M.runtime.handling[key] end
    local g = M.game and M.game.handling
    if type(g) == "table" and g[key] ~= nil then return g[key] end
    return default_handling(key)
end

function M.camera(key)
    if M.playing and M.runtime.camera[key] ~= nil then return M.runtime.camera[key] end
    local g = M.game and M.game.camera
    if type(g) == "table" and g[key] ~= nil then return g[key] end
    return default_camera(key)
end

-- Changes a handling or camera value: the runtime override while playing, else the authored data.
function M.set_tuning(group, key, value)
    if M.playing then
        M.runtime[group][key] = value
        runtime_changes[#runtime_changes + 1] = { group = group, key = key, value = value }
        return true
    end
    return M.edit("Set " .. group .. " " .. key, group .. ":" .. key, function(g)
        g[group] = g[group] or {}
        g[group][key] = value
    end)
end

-- "Apply to authored": copies the runtime overrides into the scene data (kept until Stop).
function M.apply_runtime_to_authored()
    local n = 0
    for _, group in ipairs({ "handling", "camera" }) do
        for key, value in pairs(M.runtime[group]) do n = n + 1 end
    end
    if n == 0 then
        M.status = "no runtime changes to apply"
        return false
    end
    local ok = M.edit("Apply runtime tuning", nil, function(g)
        for _, group in ipairs({ "handling", "camera" }) do
            g[group] = g[group] or {}
            for key, value in pairs(M.runtime[group]) do g[group][key] = value end
        end
    end)
    M.status = ok and ("applied " .. n .. " runtime values to the authored scene") or "nothing changed"
    return ok
end

-- Session side: the runtime values to apply to the running car since the last call.
function M.take_runtime_changes()
    local out = runtime_changes
    runtime_changes = {}
    return out
end

-- ---------------------------------------------------------------------------------------------
-- Road network (game.road_network)
-- ---------------------------------------------------------------------------------------------

function M.road_network()
    return M.game and M.game.road_network or nil
end

function M.set_road_width(key, value)
    return M.edit("Road " .. key, "road:" .. key, function(g)
        g.road_network = g.road_network or {}
        g.road_network[key] = value
    end)
end

function M.set_road_node(index, x, z, label)
    return M.edit(label or "Move road node", "node:" .. index, function(g)
        local n = g.road_network.nodes[index]
        if n.pos == nil then n.pos = { x, z } else write_xz(n.pos, x, z) end
    end)
end

-- ---------------------------------------------------------------------------------------------
-- Session requests and car telemetry
-- ---------------------------------------------------------------------------------------------

-- kinds: "play" (Play this event: Play, then start the event), "start_event" (index), "teleport" ({x, z, heading_deg}).
function M.request(kind, payload)
    requests[#requests + 1] = { kind = kind, payload = payload }
end

function M.take_requests()
    local out = requests
    requests = {}
    return out
end

-- The session calls this every frame while playing (nil when stopped).
function M.publish_car(tel)
    M.car = tel
end

-- Race progress for the panel: { phase_name, time, next_cp } (nil when not racing or not published).
function M.publish_race(race)
    M.race = race
end

-- Speedwall (save data, user directory): loaded through ctx:load_user_json, cleared with ctx:save_user_json.
function M.load_best(ctx)
    if not ctx or type(ctx.load_user_json) ~= "function" then
        M.status = "speedwall: no user-data binding in this editor context"
        return false
    end
    local ok, value, reason = pcall(ctx.load_user_json, ctx, SPEEDWALL)
    if not ok then
        M.status = "speedwall: " .. tostring(value)
        return false
    end
    if value == nil then
        M.best = {}
        M.status = reason and ("speedwall: " .. tostring(reason)) or "speedwall: no best times yet"
        return reason == nil
    end
    M.best = type(value) == "table" and value or {}
    M.status = "speedwall loaded"
    return true
end

function M.best_time(id)
    local b = M.best and M.best[id]
    if type(b) == "table" and type(b.time) == "number" then return b.time end
    return nil
end

function M.clear_best(ctx)
    if not ctx or type(ctx.save_user_json) ~= "function" then
        M.status = "speedwall: no user-data binding in this editor context"
        return false
    end
    local ok, saved, reason = pcall(ctx.save_user_json, ctx, SPEEDWALL, {})
    if not ok or saved ~= true then
        M.status = "speedwall clear failed: " .. tostring(ok and reason or saved)
        return false
    end
    M.best = {}
    M.status = "best times cleared (speedwall.json in the user directory)"
    return true
end

-- Test helpers for the headless check: the internal state without the engine.
function M._state() return { undo = undo_stack, redo = redo_stack, runtime_changes = runtime_changes, requests = requests } end

return M

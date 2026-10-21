-- Menus of the sample arena (docs/lua_api.md "UI widgets and menus"): a title screen, a pause menu, options and a
-- Controls page. Escape (ctx:ui_back_pressed) opens and closes the pause menu. The mouse, Enter or Space, the arrow
-- keys and the pad drive the widgets. While a menu is open the gameplay systems return early (Menu.blocking()).
-- Controls rebinds the project's actions with input.listen_next / input.rebind and saves them per player (input.save_bindings).
-- Nothing here is engine-specific: the widgets and the event/input API are the same ones any project can use.

local Menu = { screen = "title", from = "title", volume = 80, showHud = true, quit = false, shown = nil,
               listen = nil, controlsMsg = "" }

local PANEL = { bg = 0xD80A0C10, border = 0xFF3A4A5A, rounding = 14 }
local BTN = { bg = 0xE6202C3A, hover = 0xF0325068, active = 0xF0162330, focus = 0xFF5FD1FF,
              text = 0xFFEEF3F8, rounding = 8, size = 24, font = "label" }
local SLIDER = { bg = 0xE6202C3A, hover = 0xF0325068, accent = 0xFF5FD1FF, track = 0xFF3A4756,
                 knob = 0xFFFFFFFF, focus = 0xFF5FD1FF, rounding = 8, size = 24, font = "label", step = 5 }
local BTN_ROW = { bg = BTN.bg, hover = BTN.hover, active = BTN.active, focus = BTN.focus, text = BTN.text,
                  rounding = BTN.rounding, size = 20, font = "label" }
local ACCENT = 0xFF5FD1FF
local DIM = 0xFFB8C4D0
local WIDTH = 380
local ROW = 54

function Menu.blocking()
    return Menu.screen ~= "game"
end

local function prettyName(action)
    local s = action:gsub("_", " ")
    return s:sub(1, 1):upper() .. s:sub(2)
end

-- Binding labels without the role suffix ("A (negative)" -> "A"): the row says which side it is by position.
local function bindingText(b)
    local parts = {}
    for _, label in ipairs(b.labels) do
        parts[#parts + 1] = (label:gsub(" %(%a+%)", ""))
    end
    if #parts == 0 then return "unbound" end
    return table.concat(parts, " / ")
end

local function isAxis(action)
    local b = input.bindings(action)
    return b ~= nil and (b.pad_axis ~= nil or #b.keys_negative > 0 or #b.keys_positive > 0)
end

-- The next press that listen_result reported: put it on the action it was asked for.
local function bindCaptured(action, hit)
    local spec
    if isAxis(action) then
        if hit.pad_axis then
            -- The prompt says "push the stick right": a negative raw reading is the inverted direction.
            spec = { pad_axis = hit.pad_axis, invert = hit.sign < 0 }
        elseif hit.key then
            spec = { key = hit.key, role = "positive" }
        else
            Menu.controlsMsg = "Steering takes a stick or a key"
            return
        end
    else
        if hit.key then spec = hit.key
        elseif hit.pad_button then spec = { pad_button = hit.pad_button }
        elseif hit.mouse then spec = { mouse = hit.mouse }
        else
            Menu.controlsMsg = "Buttons take a key, a pad button or a mouse button"
            return
        end
    end
    local ok, conflicts = input.rebind(action, spec)
    if ok == nil then
        Menu.controlsMsg = "Could not bind: " .. tostring(conflicts)
        return
    end
    input.save_bindings()
    Menu.controlsMsg = prettyName(action) .. " is now " .. input.binding_name(hit)
    if conflicts and #conflicts > 0 then
        Menu.controlsMsg = Menu.controlsMsg .. " (also used by " .. table.concat(conflicts, ", ") .. ")"
    end
end

-- Escape: the arena pauses, the pause menu resumes, options go back where they were opened from.
nox.system({
    name = "menu",
    order = -20,
    update = function(ctx, dt)
        if Menu.quit then ctx:request_exit() end
        audio.bus_volume("master", Menu.volume / 100)
        local back = ctx:ui_back_pressed()
        if Menu.listen then
            -- While a rebind waits for input, Escape cancels it and is not bound (it is dropped with the capture).
            if back then
                input.listen_cancel()
                Menu.listen = nil
                Menu.controlsMsg = "Rebind cancelled"
                back = false
            else
                local hit = input.listen_result()
                if hit then
                    bindCaptured(Menu.listen, hit)
                    Menu.listen = nil
                end
            end
        end
        if back then
            if Menu.screen == "game" then Menu.screen = "pause"
            elseif Menu.screen == "pause" then Menu.screen = "game"
            elseif Menu.screen == "options" then Menu.screen = Menu.from
            elseif Menu.screen == "controls" then Menu.screen = "options" end
        end
        if Menu.shown ~= Menu.screen then
            Menu.shown = Menu.screen
            ctx:log("menu: " .. Menu.screen)
        end
    end,
})

nox.on_draw(function(ctx, ui, dt)
    if not Menu.blocking() then return end
    local w, h = ui.viewport()
    ui.menu()
    ui.rect_filled(0, 0, w, h, 0x90000000)
    local x = (w - WIDTH) * 0.5
    local top = h * 0.5 - 170

    if Menu.screen == "title" then
        ui.panel({ x = x - 30, y = top - 30, w = WIDTH + 60, h = 380 }, PANEL)
        ui.label({ x = x, y = top, w = WIDTH, h = 64 }, "NOX ARENA",
            { font = "label", size = 46, color = ACCENT, align = "center" })
        local col = ui.column(x, top + 90, WIDTH, 12)
        if ui.button("title.start", col:next(ROW), "Start", BTN) then Menu.screen = "game" end
        if ui.button("title.options", col:next(ROW), "Options", BTN) then
            Menu.from = "title"
            Menu.screen = "options"
        end
        if ui.button("title.quit", col:next(ROW), "Quit", BTN) then Menu.quit = true end
        ui.label({ x = x, y = top + 300, w = WIDTH, h = 30 },
            "Enter or A picks   Escape pauses", { size = 16, color = DIM, align = "center" })
    elseif Menu.screen == "pause" then
        ui.panel({ x = x - 30, y = top + 30, w = WIDTH + 60, h = 300 }, PANEL)
        ui.label({ x = x, y = top + 50, w = WIDTH, h = 56 }, "PAUSED",
            { font = "digits", size = 34, color = ACCENT, align = "center" })
        local col = ui.column(x, top + 120, WIDTH, 12)
        if ui.button("pause.resume", col:next(ROW), "Resume", BTN) then Menu.screen = "game" end
        if ui.button("pause.options", col:next(ROW), "Options", BTN) then
            Menu.from = "pause"
            Menu.screen = "options"
        end
        if ui.button("pause.title", col:next(ROW), "Quit to main menu", BTN) then Menu.screen = "title" end
    elseif Menu.screen == "options" then
        ui.panel({ x = x - 30, y = top - 10, w = WIDTH + 60, h = 380 }, PANEL)
        ui.label({ x = x, y = top + 6, w = WIDTH, h = 50 }, "OPTIONS",
            { font = "label", size = 34, color = ACCENT, align = "center" })
        local col = ui.column(x, top + 80, WIDTH, 10)
        ui.label(col:next(26), "Master volume", { size = 20, color = DIM })
        Menu.volume = ui.slider("options.volume", col:next(ROW), Menu.volume, 0, 100, SLIDER)
        Menu.showHud = ui.toggle("options.hud", col:next(ROW), "Show HUD", Menu.showHud, BTN)
        if ui.button("options.controls", col:next(ROW), "Controls", BTN) then
            Menu.controlsMsg = ""
            Menu.screen = "controls"
        end
        if ui.button("options.back", col:next(ROW), "Back", BTN) then Menu.screen = Menu.from end
    elseif Menu.screen == "controls" then
        local actions = input.actions()
        local panelY = top - 40
        local panelH = 70 + #actions * 46 + 2 * 46 + 70
        ui.panel({ x = x - 30, y = panelY, w = WIDTH + 60, h = panelH }, PANEL)
        ui.label({ x = x, y = top - 26, w = WIDTH, h = 50 }, "CONTROLS",
            { font = "label", size = 34, color = ACCENT, align = "center" })
        local col = ui.column(x, top + 30, WIDTH, 6)
        for _, action in ipairs(actions) do
            local b = input.bindings(action)
            local caption = Menu.listen == action and "press input" or "rebind"
            local text = prettyName(action) .. ": " .. bindingText(b) .. "  [" .. caption .. "]"
            if ui.button("controls." .. action, col:next(40), text, BTN_ROW) then
                if Menu.listen == action then
                    input.listen_cancel()
                    Menu.listen = nil
                else
                    Menu.listen = action
                    input.listen_next()
                    Menu.controlsMsg = "Press a key, button or stick (Escape cancels)"
                end
            end
        end
        if ui.button("controls.reset", col:next(40), "Reset to defaults", BTN_ROW) then
            input.reset_bindings()
            input.save_bindings()
            Menu.listen = nil
            Menu.controlsMsg = "Defaults restored"
        end
        if ui.button("controls.back", col:next(40), "Back", BTN_ROW) then
            if Menu.listen then input.listen_cancel(); Menu.listen = nil end
            Menu.screen = "options"
        end
        ui.label({ x = x, y = panelY + panelH - 44, w = WIDTH, h = 30 }, Menu.controlsMsg,
            { size = 16, color = DIM, align = "center" })
    end
end)

return Menu

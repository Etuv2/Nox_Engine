-- Menus of the sample arena (docs/lua_api.md "UI widgets and menus"): a title screen, a pause menu and options.
-- Escape (ctx:ui_back_pressed) opens and closes the pause menu. The mouse, Enter or Space, the arrow keys and the pad
-- drive the widgets. While a menu is open the gameplay systems return early (Menu.blocking()).
-- Nothing here is engine-specific: the widgets and the event/input API are the same ones any project can use.

local Menu = { screen = "title", from = "title", volume = 80, showHud = true, quit = false, shown = nil }

local PANEL = { bg = 0xD80A0C10, border = 0xFF3A4A5A, rounding = 14 }
local BTN = { bg = 0xE6202C3A, hover = 0xF0325068, active = 0xF0162330, focus = 0xFF5FD1FF,
              text = 0xFFEEF3F8, rounding = 8, size = 24, font = "label" }
local SLIDER = { bg = 0xE6202C3A, hover = 0xF0325068, accent = 0xFF5FD1FF, track = 0xFF3A4756,
                 knob = 0xFFFFFFFF, focus = 0xFF5FD1FF, rounding = 8, size = 24, font = "label", step = 5 }
local ACCENT = 0xFF5FD1FF
local DIM = 0xFFB8C4D0
local WIDTH = 380
local ROW = 54

function Menu.blocking()
    return Menu.screen ~= "game"
end

-- Escape: the arena pauses, the pause menu resumes, options go back where they were opened from.
nox.system({
    name = "menu",
    order = -20,
    update = function(ctx, dt)
        if Menu.quit then ctx:request_exit() end
        audio.bus_volume("master", Menu.volume / 100)
        if ctx:ui_back_pressed() then
            if Menu.screen == "game" then Menu.screen = "pause"
            elseif Menu.screen == "pause" then Menu.screen = "game"
            elseif Menu.screen == "options" then Menu.screen = Menu.from end
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
        if ui.button("options.back", col:next(ROW), "Back", BTN) then Menu.screen = Menu.from end
    end
end)

return Menu

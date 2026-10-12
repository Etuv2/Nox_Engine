-- Menu and START EVENT prompt: drawing geometry, pointer hit tests and the pointer half of the menu
-- interaction. Keys and pad are the session's (it owns selection and activation); this module only
-- reports what the pointer did as actions (see scripts/ui/README.md, "hud.frame actions").
--
-- Hit regions are the rectangles drawn in the previous draw (the same rule as the native
-- ui::Pointer regions): hud.draw stores them, hud.frame tests the pointer against them.

local D = require("ui.draw")

local M = {}

-- Regions from the last draw, in window coordinates: items[i] = {x0,y0,x1,y1}, start = rect or nil,
-- modal = view rect while a menu is open (blocks clicks below it and takes the wheel).
local regions = { items = {}, start = nil, modal = nil, count = 0 }

-- Pointer state carried between frames (not race state).
local press_item = nil
local press_start = false

local function inside(r, x, y)
  return r ~= nil and x >= r[1] and x <= r[3] and y >= r[2] and y <= r[4]
end

-- Reference geometry (menu centre = MC of the view). cp(dx, dy) in the native code.
function M.centre_xf(view, viewH)
  return D.xf_at(view.x + view.w * 0.5, view.y + view.h * 0.5, viewH / D.REF_H)
end

-- Vertical layout of a menu: title line top and item rows.
function M.menu_top(phase)
  return (phase == "finished") and 0.0 or -44.0
end

function M.item_y(top, i)
  return top + 18.0 + (i - 1) * 15.0
end

-- Rect of menu item i (1-based Lua index) around (cx, cy) in the MC transform.
function M.item_rect(cx, cy, s, top, i)
  local y = M.item_y(top, i)
  return { cx - 70 * s, cy + (y - 7) * s, cx + 70 * s, cy + (y + 7) * s }
end

-- Called from hud.draw: remember what was drawn.
function M.begin_draw()
  regions.items = {}
  regions.start = nil
  regions.modal = nil
  regions.count = 0
end

function M.record_item(i, rect)
  regions.items[i] = rect
  regions.count = math.max(regions.count, i)
end

function M.record_start(rect) regions.start = rect end
function M.record_modal(rect) regions.modal = rect end

-- Pointer step. input: { pointer = {x, y} | nil, moved = bool, pressed = bool, released = bool,
-- down = bool, wheel = number, menu_open = bool, item_count = int, selected = int (1-based),
-- start_visible = bool }. Returns the pointer part of the actions.
function M.pointer_step(input)
  local out = {
    hovered = -1, pressed = -1, select = nil, click = nil, wheel = 0,
    start_hovered = false, start_pressed = false, start_click = false,
  }
  local p = input.pointer
  if not p then
    press_item = nil
    press_start = false
    return out
  end
  local x, y = p.x, p.y
  if input.menu_open then
    local hover = nil
    for i = 1, regions.count do
      if inside(regions.items[i], x, y) then hover = i end
    end
    out.hovered = hover or -1
    if hover and (input.moved or input.pressed) and input.selected ~= hover then
      out.select = hover
    end
    if input.pressed then press_item = hover end
    if hover and input.down and press_item == hover then out.pressed = hover end
    if input.released then
      if hover and press_item == hover then out.click = hover end
      press_item = nil
    end
    if input.wheel and input.wheel ~= 0 and regions.modal then
      -- Wheel up steps back, wheel down steps forward (native: wheel > 0 -> previous item).
      out.wheel = input.wheel > 0 and -1 or 1
    end
  elseif input.start_visible then
    local hit = inside(regions.start, x, y)
    out.start_hovered = hit
    if input.pressed then press_start = hit end
    out.start_pressed = hit and input.down and press_start
    if input.released then
      if hit and press_start then out.start_click = true end
      press_start = false
    end
  end
  return out
end

return M

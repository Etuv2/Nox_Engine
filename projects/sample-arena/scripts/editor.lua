-- Editor demo (docs/lua_api.md "Editor"): a project panel with immediate widgets and one viewport handle.
-- Nothing here runs in play; the editor draws it. The handle's position is owned by the engine while it is dragged.

local tuning = { scale = 1.0, mode = 2, label = "arena" }

nox.editor_panel{
  id = "arena",
  title = "Arena tuning",
  draw = function(ctx, ed)
    ed.text("HUD and camera")
    ed.separator()
    tuning.scale = ed.slider("HUD scale", tuning.scale, 0.5, 2.0)
    tuning.mode = ed.combo("Mode", tuning.mode, { "eco", "sport", "track" })
    tuning.label = ed.input_text("Label", tuning.label)
    if ed.button("Reset") then
      tuning.scale = 1.0
      tuning.mode = 2
    end
    ed.tree("Spawn point", function(e)
      local _, x, y, z = e.handle_position("spawn")
      if x then e.text(string.format("x %.2f  z %.2f", x, z)) end
    end)
  end,
}

nox.editor_viewport(function(ctx, ed)
  -- Drag the marker on the ground plane; the drag is one undo step in the editor.
  local dragging, x, y, z = ed.handle("spawn", 0.0, 0.0, 0.0, { radius = 8, color = 0xFF5FD1FF })
end)

-- Editor panel "HUD": live edit of projects/nox-racing/config/hud.json (placement, colours, gauges, minimap) with Save and
-- Reload. Port of RacingHUD::DrawEditorPanel. The edits apply to the presentation at once (the HUD and
-- map read scripts/ui/config.lua values every frame). Save writes that file through the
-- authoring save path (request R2); Reload reads it back through the JSON binding (request R1).

local config = require("ui.config")

local HUD = "projects/nox-racing/config/hud.json"
local status = HUD

local PLACEMENTS = { "cluster", "minimap", "event", "panel", "hint" }

local function slider_color(ed, label, c)
  c[1] = ed.slider(label .. " r", c[1], 0.0, 1.0)
  c[2] = ed.slider(label .. " g", c[2], 0.0, 1.0)
  c[3] = ed.slider(label .. " b", c[3], 0.0, 1.0)
  c[4] = ed.slider(label .. " a", c[4], 0.0, 1.0)
end

local function save()
  if not nox.save_authoring_json then
    status = "save unavailable: no authoring save binding (request R2)"
    return
  end
  local ok, reason = nox.save_authoring_json(HUD, config.document())
  status = ok and ("saved " .. HUD) or ("save failed: " .. tostring(reason))
end

local function reload()
  if not nox.json then
    status = "reload unavailable: no JSON binding (request R1)"
    return
  end
  local doc = nox.json(HUD)
  config.apply(doc)
  status = "reloaded " .. HUD
end

nox.editor_panel {
  id = "lua.hud",
  title = "HUD (Lua)",
  draw = function(ctx, ed)
    local cfg = config.get()
    ed.text(status)
    ed.separator()
    if ed.button("Save hud.json") then save() end
    ed.same_line()
    if ed.button("Reload") then reload() end

    ed.tree("HUD placement", function(e)
      for _, name in ipairs(PLACEMENTS) do
        e.tree(name, function(t)
          local p = cfg.placement[name]
          p.anchor = t.input_text("anchor (TL TC TR ML MC MR BL BC BR)", p.anchor)
          p.offset[1] = t.slider("offset x (ref px)", p.offset[1], -400.0, 400.0)
          p.offset[2] = t.slider("offset y (ref px)", p.offset[2], -400.0, 400.0)
          p.scale = t.slider("scale", p.scale, 0.2, 3.0)
        end)
      end
    end)

    ed.tree("HUD colours", function(e)
      slider_color(e, "accent", cfg.colors.accent)
      slider_color(e, "needle", cfg.colors.needle)
      slider_color(e, "redline", cfg.colors.redline)
      slider_color(e, "speed", cfg.colors.speed)
      slider_color(e, "lavender", cfg.colors.lavender)
      slider_color(e, "text", cfg.colors.text)
      cfg.colors.backing_alpha = e.slider("backing alpha", cfg.colors.backing_alpha, 0.0, 1.0)
      cfg.colors.glow = e.slider("glow", cfg.colors.glow, 0.0, 2.0)
    end)

    ed.tree("HUD fonts (declared in project.json; text scale applies now)", function(e)
      cfg.fonts.text_scale = e.slider("text scale", cfg.fonts.text_scale, 0.3, 3.0)
      e.text("font files are declared in project.json (ids: label, digital)")
    end)

    ed.tree("HUD gauges", function(e)
      local g = cfg.gauges
      g.rpm_max = e.input_float("rpm max", g.rpm_max)
      g.redline = e.input_float("redline rpm", g.redline)
      g.boost_label = e.input_text("boost label", g.boost_label)
      g.boost_max = e.slider("boost max (load)", g.boost_max, 0.05, 4.0)
      g.n2o_label = e.input_text("n2o label", g.n2o_label)
      g.speed_units = e.input_text("speed units", g.speed_units)
    end)

    ed.tree("HUD minimap", function(e)
      local m = cfg.minimap
      m.radius_m = e.slider("radius (m)", m.radius_m, 5.0, 300.0)
      m.zoom_with_speed = e.slider("zoom with speed (m per km/h)", m.zoom_with_speed, 0.0, 0.6)
      m.opacity = e.slider("opacity", m.opacity, 0.0, 1.0)
      m.road_px = e.slider("road width (px)", m.road_px, 0.2, 4.0)
      m.radius_px = e.slider("size (ref px)", m.radius_px, 8.0, 120.0)
    end)
  end,
}

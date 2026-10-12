-- Racing editor panels and the viewport authoring overlay (Lua). The project entry requires this module once.
-- model first: it registers the scene save hook (nox.on_save_game) and the configure hook (nox.on_configure).
require("editor.model")
require("editor.race_panel")
require("editor.drive_panel")
require("editor.hud_panel")
require("editor.overlay")

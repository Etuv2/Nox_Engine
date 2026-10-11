# NOX racing UI icons

105 UI icon roles: 104 independently generated ImageGen symbols and one
close/cancel alias using the back arrow. The pack matches the existing
cyan/lavender night-racing HUD and covers current gameplay plus
garage, tuning, event, menu, settings and control needs. Future mechanics may
require additional symbols; this inventory does not imply those features exist.

## Files

- `hud/`, `navigation/`, `events/`, `garage/`, `menus/`, `settings/`, `controls/`:
  selected original ImageGen PNGs, preserved without modification.
- `64/`, `128/`, `256/`: individual game-ready RGBA PNGs at each square size,
  organized into the same categories.
- `atlas/ui_icons_64.png`, `atlas/ui_icons_128.png`, `atlas/ui_icons_256.png`:
  packed RGBA atlases with four transparent gutter pixels around each cell.
- `manifest.json`: unique category/icon IDs, purpose, source dimensions,
  SHA-256 hashes, exact prompts, export paths and atlas rectangles.
- `icon_plan.json`: the inventory and subject specifications.
- `generation_records.json`: selected generation outputs and prompts.
- `contact_sheet.png`: all icons at a larger review size and at 32 px in white,
  cyan and lavender. This sheet is a preview, not a runtime atlas.

## Inventory

| Category | Count | Coverage |
| --- | --- | --- |
| HUD | 12 | Nitrous, boost, speed, engine, gearbox, handbrake, headlights, damage, traction and warnings |
| Navigation | 16 | Player, destination, checkpoints, start, finish, parking, meets, photo, rest, garage, map, routing and road levels |
| Events | 12 | Sprint, circuit, drift, time trial, drag, speed trap, trophy, record, standings, medal and locks |
| Garage | 18 | Vehicle, wheels, tyres, brakes, suspension, exhaust, intake, ECU, differential, aero, body kit, paint, livery, repair, upgrades, credits, purchase and equipped |
| Menus | 18 | Home, play, pause, retry, reset, free roam, exit, settings, back, close, confirm, information, save, delete, search, filter, favorite and help |
| Settings | 9 | Audio, mute, music, display, graphics, controls, accessibility, language and camera |
| Controls | 20 | Keyboard, mouse, steering, pedals, D-pad, stick, trigger, Enter, Shift, arrows, positional face buttons, menu and view |

Controller face-button symbols identify their position on a generic controller.
Render platform-specific labels alongside them with the game's fonts. Other
key names and countdown numbers should also remain live text.

ImageGen rejected the X close glyph twice. `menus/close` therefore uses the
same approved generated arrow as `menus/back`; `alias_of` records this in the
plan and manifest. The alias files are identical copies rather than a newly
generated design.

## Using the icons

Start with the 128 px atlas for ordinary 24-48 px HUD or menu icons. Use the
256 px exports for larger interfaces and the 64 px exports for compact UI.
Every export has a white foreground with straight alpha. Multiply RGB by the
runtime tint and alpha by the desired opacity. Suggested colors from
`config/hud.json` are white `(1, 1, 1)`, cyan `(0.35, 0.85, 1)` and lavender
`(0.8, 0.62, 1)`. Keep danger states red where appropriate. Draw selection,
disabled opacity and glow in the UI rather than baking duplicate icon states.

The exported silhouette fits within 75 percent of its square canvas and is
centered with consistent padding. This makes icon placement independent of
the original ImageGen canvas dimensions. RGB stays white even in transparent
export pixels, avoiding dark fringes with straight-alpha filtering.

Atlas pixel rectangles are `[left, top, right, bottom]`, with exclusive right
and bottom coordinates. UV rectangles use top-left image coordinates. Adapt
the vertical UV direction if the texture upload flips rows. Use clamp-to-edge,
linear filtering and ordinary straight-alpha blending. Disable atlas mipmaps
unless the integration supplies mip-safe gutters. The full icon cell includes
transparent padding, so use the recorded rectangle directly.

This is an asset pack. `RacingHUD` still draws its existing symbols, and no
gameplay, HUD layout or renderer code is changed. When integrating, reference
the chosen atlas and manifest from scene/config data so `tools/Package.ps1`
discovers them. A reference to an individual JSON manifest does not recursively
package every PNG named inside it; also reference the chosen export directory
or atlas PNG. CMake's existing Release synchronization already includes the
`textures` folder.

## Provenance and rebuild

Created with the built-in `image_gen.imagegen` tool. Each original symbol was generated
separately.
The subjects come from the game documentation and current HUD conventions.
The user approved Python processing for smaller PNG exports and atlas packing.
Pillow removes minor dark fringe/speck remnants, extracts the generated white
silhouette, normalizes its size and creates antialiased exports. This preserves
the selected originals and does not replace generated symbols with drawings.

Rebuild or validate from the repository root using the existing Python/Pillow
environment:

```powershell
python tools/ui_icons.py
python tools/ui_icons.py --check
```

Validation checks inventory completeness, unique IDs, original/export/atlas
hashes, source/export RGBA dimensions and alpha, transparent padding, fully white export RGB, opaque
symbol cores, and byte-exact atlas cells. The contact sheet supports visual
review of every symbol and its small-size tints. Repository text files are
ASCII-compatible UTF-8 without BOM.

In-engine texture upload, HUD placement, runtime tinting and packaged-game
rendering still need checking when the icons are integrated.

## Engine integration

`config/hud.json` has an `icons` block that drives the atlas: `manifest`
(`textures/ui_icons/manifest.json`), `atlas` (`atlas/ui_icons_128.png`) and
`size` (128). `UiIconAtlas` (`src/UiIconAtlas.*`) loads that atlas once, via the
existing `Texture` class with mipmaps disabled, clamp-to-edge and linear
filtering, and resolves manifest rectangles and `alias_of` entries. Icons are
drawn with ImGui vertex tints (straight-alpha blending), so the cyan, lavender and
red colours come from the config colours. `icons.roles` maps HUD roles to icon
ids, keyed `<section>/<key>`: `gauge/*`, `warning/<message prefix>`, `map/*`,
`event/<event type>`, `result/*`, `menu/<menu label or title>`, `key/<start key>`
and `editor/*` (the editor toolbar). Missing icons, a missing manifest or a failed
atlas upload keep the previous vector glyphs and text.

Packaging: `tools/Package.ps1` ships the atlas PNG and manifest because
`config/hud.json` references them. Only the 128 px atlas is shipped. The 64 px and
256 px atlases are not referenced by the engine.

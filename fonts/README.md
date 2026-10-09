# HUD fonts

Fonts used by the racing HUD (`src/RacingHUD.cpp`). Paths are resolved relative to the working
directory (the engine asset root). All fonts are licensed under the SIL Open Font License 1.1.

| File | Family | Source | License |
| --- | --- | --- | --- |
| `BarlowCondensed-Medium.ttf` | Barlow Condensed Medium | https://github.com/google/fonts/tree/main/ofl/barlowcondensed | OFL 1.1 (`licenses/Barlow-OFL.txt`) |
| `BarlowCondensed-SemiBold.ttf` | Barlow Condensed SemiBold | https://github.com/google/fonts/tree/main/ofl/barlowcondensed | OFL 1.1 (`licenses/Barlow-OFL.txt`) |
| `DSEG7Classic-Bold.ttf` | DSEG7 Classic Bold (7-segment) | https://github.com/keshikan/DSEG/releases/tag/v0.46 | OFL 1.1 (`licenses/DSEG-LICENSE.txt`) |
| `arial.ttf` | Arial (existing engine font, fallback) | bundled with the engine | see engine licensing |

Config keys: `fonts.label`, `fonts.digital`, `fonts.fallback` in `config/hud.json`.

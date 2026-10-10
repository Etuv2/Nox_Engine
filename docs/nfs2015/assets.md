# NFS 2015 district: CC0 prop assets

Source list and licenses: `models/cc0/SOURCES.md` (git-ignored models folder). All props come from Poly Haven
(CC0 by site policy). Downloads and Blender scripts live outside the repo.

## Props placed by GenerateDistrict

Placed as generated `model` child nodes (`ProceduralStreetKit::Result::propNodes`, appended to the generated children
in `SceneLoader`). Each prop model is loaded once through `ModelManager`'s cache, so repeated placements share GPU
buffers and only add draw nodes. Every placed prop uses `material_overrides` with `wetness_response: 0.5`.
A prop is skipped when its `.glb` is missing, and the district option `"props": false` disables them all.

| Model | Placement |
|---|---|
| fire_hydrant | sidewalk, replaces the procedural hydrant |
| metal_trash_can | sidewalk bins, replaces the procedural bin |
| planter_box_01 | sidewalk, between the lamp row and the shopfronts |
| potted_plant_01 | sidewalk at the building line |
| shrub_01 | sidewalk hedge at the building line |
| modular_street_seating | sidewalk bench |
| utility_box_01, power_box_01 | sidewalk transformer and utility boxes |
| exterior_aircon_unit | rooftop units, about 35 percent of roof units |

Every solid prop adds a layout obstacle (`<output>_layout.json`).

## Procedural additions (no assets)

- Setback towers: about 30 percent of multi-storey lots get a taller inset mass with its own window grid.
- Service lanes: about 30 percent of wide lots leave a 2.8 m alley on the +X side, with a dumpster, wall pipes and a
  fire escape on the alley wall. Other buildings get a fire escape on the back face at about 30 percent.
- Surface parking lots: about 10 percent of wide lots become asphalt with stall lines and parked cars.
- Junction span gantries (four-way junctions) on the diagonal signal poles, and corner street-name signs (all junctions).
- Pole-mounted transformers on about one utility pole in three.

## Not done / gaps

- Palms, awnings, neon and shopfronts remain procedural. No CC0 palm was found, and no CC0 storefront or facade pieces
  were downloaded.
- The `_lod1` GLBs are exported but not wired, since the engine has no LOD selection for scene model nodes.
- No bus shelter and no street signs beyond the procedural name plates.

# Texture packs: road decals, lens flares, window rooms

The engine features are generic. The night-street game content only supplies data.

**Engine (no game knowledge).** `SceneGraph::renderExtras` (`src/SceneRenderExtras.h`) holds three things:
- `DecalSet`s: texture layers, one surface material, instances. `DecalPass` draws them.
- A `LensFlareSet`: sprite layers, data-defined styles (texture layer, gain, width, tint, glare
  dependence), sources. `LensFlarePass` draws them.
- An `InteriorAtlas`: N five-face rooms, per-face mean radiance, per-room light, per-style variant
  weights. The G-buffer interior mapping samples it.

Any loader or game layer can fill these structures. Textures are file lists that `TextureArrayBuilder`
turns into cached GL arrays: sRGB decode, optional linear premultiply with a residual-alpha floor,
area resample, premultiplied (alpha-aware) mips, `SRGB8_ALPHA8`, clamp-to-edge.
- `DecalPass` runs between `GBufferPass` ("GBufferBase") and every "GBuffer" consumer. Quads are
  depth-tested read-only with a polygon offset. Coverage blends only albedo (RT1.rgb) and roughness
  (RT0.b), and the set's wetness response is applied. Decals stay non-emissive and reflect like their
  surface.
- `LensFlarePass` runs after bloom and auto exposure, before tone mapping:
  1. A compute pass tests a 13-tap depth disc and eases visibility over time.
  2. Up to four sprite layers per source are drawn as screen-space quads, clamped in pixel size, with
     distance and size fades, added in linear HDR with pre-exposure.
  Flares go into the pass's own copy of the post source, so TAA history and SSR never see them.
  `NOX_FLARE_DEBUG=1` logs the nearest sources' visibility.
- Interior mapping keeps one texture-array layer per face, so faces never bleed. LOD comes from the
  propagated footprint. The material `interior.style` picks the weight row; `interior.pane` (half
  extents) and `interior.sill` set the room box. No procedural light or furniture is added over the
  atlas. Blinds, curtains, frame and mullion stay in front.

**Game content.** `src/NightStreetPacks.*` knows the three packs under `textures/` (see their READMEs):
- Paint bounds from `generation_prompts.json`, plus arrow stems measured from alpha.
- The road paint material.
- Flare styles: sodium = sodium glow + warm 4-ray starburst; LED = LED glow + cool 6-ray starburst;
  flood adds a streak and a glare halo; signal = source-tinted glow.
- Room faces from `atlas_manifest.json` `sample_uv_rect`, with room weights for apartment, shop and
  office.

`ProceduralStreetKit` places the instances: lines as non-repeating pieces from inside the painted
segment, spaced dashes, approach-lane stop bars, repeated six-bar crossings, tail-anchored arrows (right-
hand traffic), and flare sources at the lamp and signal lenses. Generator params:

| Param | Default |
| --- | --- |
| `decal_root` | `textures/road_decals` |
| `lens_flare_root` | `textures/lens_flares` |
| `window_room_root` | `textures/window_rooms` |
| `lens_flare_gain` | `1.0` |

A missing pack falls back to flat paint geometry, procedural rooms, or no flares. The NFS scenes
reference the roots explicitly, so `tools/Package.ps1` mirrors the folders.

Measured on the sprint scene at 3440x1440: `DecalPass` ~0.04 ms GPU (2215 decals), `LensFlarePass`
~0.11 ms GPU (58 sources, including the image copy).

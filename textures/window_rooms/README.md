# Parallax window room atlases

Six complete five-face room-box atlases for the night-street window interiors.
Each contains a back wall, left wall, right wall, ceiling, and floor. They are
opaque RGB PNGs, 1254 x 1254, with baked indoor lighting encoded as sRGB.

NOX loads these faces through `NightStreetPacks::BuildInteriorAtlas` and samples
the resulting texture array in `shaders/includes/interior_mapping.glsl`.
Imported scenes opt in with `"window_room_root": "textures/window_rooms"` at
the scene root. Window materials also require `extras.nox.interior`, correct
pane dimensions and UV/tangent mapping. Copying images beside a GLB is insufficient.
The Shuto building probe validates this import path; see
`docs/shuto/buildings.md` for the exact material and pane contract.

## Rooms

| File | Interior |
| --- | --- |
| [living_room_warm.png](living_room_warm.png) | Cream plaster, olive sofa, wood floor, warm lamp |
| [bedroom_warm.png](bedroom_warm.png) | Bed, bedside lamp, wardrobe, dark wood floor |
| [office_cool.png](office_cool.png) | Desks and monitors, gray walls, carpet, cool ceiling panel |
| [shop_warm.png](shop_warm.png) | Product shelving, counter, warm walls and tile floor |
| [cafe_warm.png](cafe_warm.png) | Coffee counter, cups, wood paneling and terracotta tiles |
| [living_room_tv_dim.png](living_room_tv_dim.png) | Dark apartment with restrained blue TV spill |

## Face layout

All six use this semantic plus-shaped layout, viewed as an image:

```text
      unused       ceiling        unused
      left wall    back wall      right wall
      unused       floor          unused
```

Use [atlas_manifest.json](atlas_manifest.json) for the actual rectangles. The
generator's boundaries are not uniformly spaced at exact thirds, even though
the prompts requested that grid. Hardcoding `(cell + uv) / 3` will sample the
wrong regions in several files. The manifest preserves the generated images
unchanged and specifies each face separately. In the shop and cafe it also
excludes narrow residual floor bands at the bottom of the wall panels.

`pixel_rect` is `[left, top, right, bottom]` in image pixels, with exclusive right
and bottom bounds. `sample_uv_rect` contains normalized pixel-center bounds with
a two-pixel inset, in the same order. UVs use image top-left as the origin. If
the loader flips image rows, convert vertical UVs accordingly. The four dark
corner regions are unused storage, not front-wall textures or alpha masks.

Walls were refined to mostly flat elevations rather than photographs of whole
rooms. Furniture is shallow detail painted into its supporting face; it does
not have independent depth. The floor and ceiling occupy their own panels.
Small furniture perspective, lighting seams, and edge trim remain artistic
approximations, so inspect close grazing-angle views during integration.

## Room-space mapping

The current NOX room uses x right, y up, and negative z into the building.
Let W and H be room half-width and half-height, and D the full room depth.
For a ray-box hit position `(x, y, z)`, define these normalized coordinates:

- `X = (x + W) / (2 * W)`, left to right.
- `Y = (H - y) / (2 * H)`, ceiling to floor.
- `T = -z / D`, front to back.

Select the surface hit by the ray and map its local UV through that face's
`sample_uv_rect`:

| Hit face | Local UV | Orientation |
| --- | --- | --- |
| Back, z = -D | (X, Y) | Left to right, top to bottom |
| Left, x = -W | (T, Y) | Front to back, top to bottom |
| Right, x = W | (1 - T, Y) | Back to front, top to bottom |
| Ceiling, y = H | (X, T) | Left to right, front to back |
| Floor, y = -H | (X, 1 - T) | Left to right, back to front |

Clamp local coordinates to [0, 1], then interpolate each axis between its
recorded minimum and maximum atlas UV. Existing `NoxInteriorWindow` already
computes the box hit and identifies the face. Its pane dimensions remain
separate from the room dimensions and this atlas layout. The existing 4x4
window-selection atlas is a different resource; these are five-face room maps,
not replacements that can use the old 4x4 UVs unchanged.

## Sampling, lighting, and variation

Load these color textures with sRGB decoding enabled. Alpha is unnecessary.
Use clamp-to-edge and begin with bilinear sampling when checking orientation.
Atlas-wide mip generation can mix adjacent faces or black unused regions, and
a two-pixel base-level inset alone does not prevent bleed at coarser mip levels.
For production, use per-face padding or a texture array, or compute a face-aware
inset and LOD limit from the sampled footprint. Clamp coordinates within the
chosen face and inspect anisotropic sampling at grazing angles.

The manifest includes `mean_linear_rgb` for every face, averaged from decoded
sRGB pixels. These values can supply a stable distant-room fallback instead of
sampling the black-biased average of the entire atlas.

The images already include the room's lighting and lamp pools. Treat samples
as relative room radiance; apply the material's brightness and pre-exposure
convention once. Do not multiply them by the procedural lamp lighting again.
Likewise, avoid applying a strong warm emissive tint over an already warm atlas.
Retain glass reflection, roughness, blinds, curtains, frames, and mullions in
their existing foreground treatment rather than baking them into these maps.

Assign room variants with a stable per-window seed; do not change variants every
frame. Keep the dim TV-lit variant at a lower brightness and leave many windows
unoccupied or dark. The engine adds no procedural furniture or figures, so the
painted furnishings are the only room content. Texture furniture will not gain
independent parallax unless separate depth layers or geometry are added later.

## Engine integration

Interior mapping is a generic engine feature: the engine knows nothing about
this pack. The game (`NightStreetPacks::BuildInteriorAtlas`, invoked by the
game-registered scene key `"window_room_root"` in `src/NoxRacingGame.cpp`)
reads this manifest and fills the engine descriptor `InteriorAtlas` in
`src/SceneRenderExtras.h`. `GBufferPass` uploads it; the shader is
`shaders/includes/interior_mapping.glsl`.

### Descriptor (`InteriorAtlas`)

| Field | Meaning | Engine default |
| --- | --- | --- |
| `faceRoles` | One role per face slot (`back`, `left`, `right`, `ceiling`, `floor`); its size is the faces per room | empty = no atlas |
| `faceLayers` | `room * faceRoles.size() + slot`, one texture-array layer each (source file + UV rect) | |
| `faceMean` | Mean linear radiance per layer (distant/flat path, blind lighting) | 0.1 |
| `roomLight[room]` | (occupied brightness, chance on when unoccupied, that brightness) | (1, 0, 0) |
| `styleWeights[style][room]` | Variant weights; the material style picks the row (clamped to the row count) | uniform |
| `roomShape` | Side margin [min,max] m, floor-to-ceiling [min,max] m, depth jitter, sill scale [min,max] | [1,1.4], [2.8,3.2], 0.1, [1,1] |
| `paneUV` | Default pane UV layout (grid, inset, extent) for materials without one | grid 0 = inferred (pane centred in its UV cell) |
| `farDistance` | Default distance beyond which panes use the flat room mean | 0 = never |
| `textureOptions.size` | Face layer edge in texels | |

Room count, faces per room and style rows are data. The uploader clamps
anything beyond the engine limits (512 rooms, 8 faces per room, 64 style rows)
and logs a warning; it never truncates silently. A role without a face slot
shows the room's mean radiance. Per-room tables are an RGBA32F texture
(`uInteriorRoomData`: room light, room mean, cumulative style weights), so no
shader array is sized at compile time.

### Manifest block (`nox_interior`)

The game builds the descriptor from `assets` (rooms, in file order) and this
block of `atlas_manifest.json`:

```json
"nox_interior": {
  "face_order": ["back", "left", "right", "ceiling", "floor"],
  "face_size": 512,
  "styles": [ { "name": "apartment", "weights": { "living_room_warm.png": 0.34 } } ],
  "room_light_default": [1.0, 0.0, 0.0],
  "room_light": { "living_room_tv_dim.png": [0.5, 0.35, 0.5] },
  "room_shape": { "side_margin": [1.0, 1.4], "height": [3.4, 3.8], "depth_jitter": 0.1, "sill_scale": [0.95, 1.08] },
  "pane_uv": [4, 0.004, 0.242],
  "far_distance": 0
}
```

`face_order` defaults to the keys of the top-level `faces` object. Style rows
are referenced by index from materials; the game's `NightStreetPacks::RoomStyle`
(apartment 0, shop 1, office 2) must match the row order, and the loader warns
when a row name does not. Weights and room light are keyed by asset file name.
`pane_uv` is the game's window-quad convention (one pane per cell of a 4x4 UV
grid, 0.004 inset, 0.242 extent), used by materials that do not state their own.

### Material (`extras.nox.interior`)

| Key | Meaning | Default |
| --- | --- | --- |
| `depth` | Room depth behind the glass, m | 4 |
| `pane` | Pane half extents [w, h], m | derived from the UV derivatives |
| `pane_uv` | Pane UV layout [grid, inset, extent] | atlas `paneUV` |
| `sill` | Room floor below the pane's bottom edge, m | 0.8 |
| `style` | Style row | 0 |
| `seed`, `lighting`, `blinds`, `strength` | Variation seed, occupancy bias, blind/curtain probability, radiance scale | 0, 0.5, 0, 1 |
| `far_distance` | Flat room mean beyond this camera distance, m (0 = never) | atlas `farDistance` |
| `flat` | `true`: always the flat room mean (cheap distant materials) | false |

The pane's image right is the glTF tangent and image up is
`cross(N, T) * tangent.w`; UV v grows downwards. The per-window seed comes from
the pane centre, so a wrong tangent sign moves the centre and changes the room
per pixel. Without a bound atlas the engine shows a plain, neutral lit box (no
furniture, figures or animation). Walls never receive interior emission; only
materials with `extras.nox.interior` do.

## Provenance and validation

Generated with the built-in `image_gen.imagegen` tool. Exact original and
refinement prompts are in [generation_prompts.json](generation_prompts.json).
The tool delivered 1254 x 1254 images after a request for 1536 x 1536. Selected
outputs were copied unchanged; the manifest records SHA-256 and dimensions.

Visually checked room identities, the five face regions, orthographic surface
corrections, and lack of window frames or people. File checks cover PNG decoding,
opaque RGB storage, six assets with thirty face rectangles, UV bounds, dark
unused corners, copy integrity, valid JSON, and UTF-8 without BOM. Seam matching
and correct ray-box projection still require an in-engine integration check.
No engine build is needed for this asset-only change, and none was run.

Release asset synchronization includes `textures`. The separate
`tools/Package.ps1` path needs scene/config references or an explicit directory
inclusion when these assets are wired into a material.

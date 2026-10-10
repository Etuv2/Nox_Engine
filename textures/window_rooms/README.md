# Parallax window room atlases

Six complete five-face room-box atlases for the night-street window interiors.
Each contains a back wall, left wall, right wall, ceiling, and floor. They are
opaque RGB PNGs, 1254 x 1254, with baked indoor lighting encoded as sRGB.

The current `shaders/includes/interior_mapping.glsl` draws rooms procedurally,
and `shaders/gbuffer_frag.glsl` bypasses the emissive texture while interior
mapping is enabled. These files therefore need a texture-sampling integration;
copying them into the project does not make them appear in existing windows.
This pass creates assets and mapping metadata, without modifying renderer code.

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
unoccupied or dark. Existing procedural furniture/person intersections should
be disabled or deliberately matched to an atlas, otherwise they can cover the
painted furnishings with unrelated shapes. Texture furniture will not gain
independent parallax unless separate depth layers or geometry are added later.

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

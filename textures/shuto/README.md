# Shuto environment texture pack

Seventeen selected PNG images generated with the built-in image_gen tool for
Blender authoring and later NOX Engine integration. The files are local project
assets; no further image-generation access is required by the implementation
prompt in ../../docs/shuto_environment_prompt.md.

Every selected image is 1254 x 1254 pixels. Fifteen are opaque RGB and two are
RGBA. These are base-color/artwork assets, not complete physically based
material sets. They are not installed into any game scene by this delivery.

## Inventory

| File | Intended use |
| --- | --- |
| asphalt_fine.png | Expressway pavement, ramps, parking surfaces |
| concrete_cast.png | Piers, parapets, barriers, retaining structures |
| concrete_weathered.png | Older structures, seawalls, underpasses |
| tunnel_tiles-v2.png | Ivory tunnel wall tiles |
| galvanized_steel.png | Gantries, guardrails, unpainted metal |
| corrugated_metal.png | Painted warehouse cladding |
| rooftop_membrane.png | Flat roofs |
| ground_gravel.png | Industrial lots, bare ground, verges |
| noise_barrier.png | Acoustic metal panel faces |
| facades_apartment.png | Four residential facade patches |
| facades_commercial.png | Four office/retail facade patches |
| facades_industrial.png | Warehouse, industrial wall, red/teal containers |
| sign_panel_blanks.png | Green highway, white safety, blue services, dark message-board substrates |
| billboards_art.png | Four fictional text-free advertisement artworks |
| roadside_props.png | Hazard paint, warning paint, reflector grid, hatch |
| foliage_cutouts.png | Shrub, grass, weed, hedge cutouts |
| surface_wear_decals-v2.png | Runoff, crack, rust, oil artwork |

The unselected tunnel_tiles.png and surface_wear_decals.png are first-pass
versions. Use the selected -v2 files. generation_prompts.json records the exact
initial prompts and revision prompts. manifest.json records filenames, SHA-256,
dimensions, alpha coverage, atlas rectangles, and suggested material values.

## Atlas mapping

Atlas cells are top-left, top-right, bottom-left, bottom-right in image space.
Read each semantic cell and pixel rectangle from manifest.json. Rectangles use
exclusive right/bottom bounds. sample_uv_rect uses pixel centers with a two-pixel
inset. Flip V where required by Blender or the engine's image-row convention;
validate orientation with one test plane.

Opaque atlases have no extruded gutters. They are independent patches, not
repeat textures. Do not repeat the entire atlas or rely on GL_CLAMP_TO_EDGE to
protect internal cells. Clamp UVs within the selected cell. For distant mip
levels, extract and pad individual patches in the authorized Blender asset
pipeline or provide per-cell mip handling; a two-pixel inset alone cannot prevent
all lower-mip bleed. Set a sensible mip limit until that processing is done.

Foliage and wear use true transparency and straight alpha. The manifest includes
content bounds at alpha >= 16. Some RGB values in nearly transparent pixels are
saturated; correct alpha compositing makes those pixels negligible. Use
alpha-aware/premultiplied filtering and discard negligible coverage when needed.
Wear needs alpha blending and low strength; foliage can use a tuned alpha test.
Do not assume a 0.5 cutoff preserves all fine leaves. Inspect grazing angles and
distant mip levels. Source images have not been alpha-cleaned or packed again.

## Materials

Color/artwork is sRGB; alpha and any later masks are linear. manifest.json gives
starting roughness/metallic factors for surface materials, not measured material
properties. Painted metal is generally dielectric; unpainted galvanized steel
can use metallic 1. Keep signs non-emissive unless the modeled device is lit.
Retroreflection requires material/shader behavior, not an emission boost.

There are no normal, roughness, metallic, height, AO, or emissive-mask images in
this pack. Use scalar materials, modeled detail and, where needed, Blender-baked
maps from appropriate geometry. Do not treat grayscale color variation as a
physically verified height field.

Facade and prop artwork contains residual painted shading around recesses.
Use it for middle/far scenery. Model nearby balcony rails, frames, reflectors
and hatches so lighting and silhouettes behave correctly. Keep window emission
selective using authored masks or separate window geometry; never illuminate
an entire facade texture.

Surface images were requested as repeat tiles, but seamlessness is not
certified. Edge statistics in manifest.json identify mismatch, not visual proof
of seamless tiling. The tunnel revision fixes the major grid discontinuity,
but small albedo seams remain. Validate repeated surfaces in Blender/NOX;
hide physical panel joins, blend repeats, or make seamless derivatives in the
authorized Blender pipeline. Match texel density to real dimensions and inspect
repetition at driving speeds.

Wetness, road reflections, tunnel lighting, water movement, glass and night
exposure should come from engine materials/geometry rather than baked imagery.
Use existing HDRs for sky/lighting where appropriate.

## Existing assets to reuse

- ../road_decals/: road-paint artwork and its placement/material documentation.
  These are artistic markings, not a claim of Japanese-standard dimensions.
  Author exact expressway lane layouts from reference.
- ../window_rooms/: interior artwork and atlas metadata. The README documents
  the required shader integration; merely copying the files does not enable it.
- Original supplied GLBs: preserve working UVs/materials and embedded textures.
  Verify actual source licenses; a filename containing CC0 is not proof.

## Validation and delivery scope

All selected PNGs decode successfully. Dimensions, modes, alpha availability,
hashes and atlas layout were checked, and generated images were inspected.
Text artifacts were checked as UTF-8 without BOM and ASCII punctuation.
The textures have not been validated in Blender or NOX and have not changed
gameplay, engine code or existing scenes. The prompt requires runtime integration
and repeat/mipmap QA as part of the environment build.

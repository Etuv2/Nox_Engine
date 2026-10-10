# Worn road marking decals

Eleven road-paint decals matching the worn white arrows and warm yellow lines in
the supplied night-driving references. These are base-color textures with
straight alpha, viewed from directly above. The engine should supply lighting,
wetness, and reflections. This asset pack does not place decals in a scene or
change the existing road renderer.

## Texture inventory

| File | Marking | Dimensions |
| --- | --- | --- |
| [arrow_straight_white.png](arrow_straight_white.png) | Straight ahead | 1254 x 1254 |
| [arrow_left_white.png](arrow_left_white.png) | Left turn | 1254 x 1254 |
| [arrow_right_white.png](arrow_right_white.png) | Right turn | 1254 x 1254 |
| [arrow_straight_left_white.png](arrow_straight_left_white.png) | Straight or left | 1254 x 1254 |
| [arrow_straight_right_white.png](arrow_straight_right_white.png) | Straight or right | 1254 x 1254 |
| [line_double_yellow.png](line_double_yellow.png) | Double yellow center-line segment | 1254 x 1254 |
| [line_single_yellow.png](line_single_yellow.png) | Single yellow line segment | 1254 x 1254 |
| [lane_dash_white.png](lane_dash_white.png) | Single white lane dash | 1254 x 1254 |
| [line_single_white.png](line_single_white.png) | White road-edge line segment | 1254 x 1254 |
| [stop_bar_white.png](stop_bar_white.png) | Horizontal stop bar | 1672 x 941 |
| [crosswalk_white.png](crosswalk_white.png) | Six-bar crossing | 1254 x 1254 |

All files are 8-bit RGBA PNGs. The stop bar was delivered as a landscape image;
the other images are square. OpenGL supports these non-power-of-two dimensions.
Do not infer world dimensions from the image dimensions: each file includes
transparent padding and each marking has its own footprint.

## Placement and scale

Project onto the road surface or use a thin road-facing overlay mesh. Align the
decal with the road tangent and surface normal; it must remain attached to the
road as the camera moves. Apply a small depth bias or surface offset to prevent
z-fighting. Restrict projected decals to road surfaces so they do not cover
vehicles, curbs, or nearby walls.

Straight arrows point toward the image's top edge. Turn arrows enter from the
bottom and point left or right; combined arrows have one shared tail. Position
the tail on the lane center rather than assuming every arrow's stem lies at UV
(0.5, 0.5). Orient the crosswalk across the road, with its six bar lengths along
the road direction. The stop bar is horizontal in its image.

The existing street kit uses 0.12 m lane/center stripes, 0.15 m white edge lines,
3 m lane dashes separated by 9 m gaps, 0.4 m stop bars, and 0.5 m wide crossing
bars separated by 0.5 m gaps. Use these as initial scene dimensions, not as a
claim that the generated silhouettes follow a road-marking standard. Arrow
paint can start around 3-4 m long, then be fitted to the scene's lane width.

[generation_prompts.json](generation_prompts.json) records each file's dimensions,
SHA-256, exact prompt, and measured paint bounds. The paint bounds use normalized
top-left image coordinates (left, top, right, bottom), measured at alpha >= 128.
Allow a few extra pixels for antialiased edges. Flip the vertical coordinate if
the texture loader flips image rows. These bounds help size the actual painted
area without including all the surrounding transparent canvas.

## Long lines and repetition

The line textures are finite segments with transparent padding at both ends;
they are not seamless repeat tiles. Repeating the whole image creates gaps.
For a dash, keep the padded ends and space copies explicitly. For a continuous
line, map a longitudinal region from inside the painted segment onto a road
strip, or overlap consecutive segments slightly after excluding the padded
ends. Do not stretch one small wear pattern over an entire district. Vary the
sampled region and segment lengths, and inspect joins because generated wear
does not guarantee matching seams.

The double-yellow image contains both stripes and their gap. Apply it once to
the combined center-line footprint; do not apply the pair separately to each
existing yellow stripe. A single crossing decal is convenient for small roads.
For different widths, repeat individual bars or adjust geometry instead of
stretching all six bars until their widths become unrealistic.

## Material setup

Load color as sRGB while keeping alpha linear. NOX's file-based `Texture`
constructor has a `srgb` argument; enable it for these color textures. Use
mipmaps, trilinear filtering, anisotropic filtering at grazing road angles, and
`GL_CLAMP_TO_EDGE` for individual decals. Use alpha-aware mipmaps where possible
and inspect distant markings for fringes or disappearing thin lines.

Alpha is paint coverage. Blend decal base color into the road material using
that coverage before lighting, or use a correctly lit alpha-blended overlay.
Do not use the additive blend intended for the lens-flare pack. An opaque quad
without alpha handling will cover the road with the transparent pixels' RGB.
For a masked overlay, tune alpha cutoff and mipmap coverage to preserve cracks
and edge detail. A few combined-arrow edge pixels have alpha 1/255; ignore
negligible alpha to avoid a faint rectangular footprint.

Start with metallic 0 and dry roughness around 0.55, matching the street kit's
existing paint materials. Wetness should reduce roughness through the engine's
wet-road shading. Keep paint non-emissive, lit by the same streetlights and
headlights as the asphalt, and eligible for the same reflection pipeline.
The reference screenshots' reflections are not baked into these files.

This pack contains color/coverage only, not normal, roughness, displacement, or
retroreflection maps. Some gray wear is present in RGB as well as alpha; an
independent projected decal will not perfectly conform those details to every
possible underlying asphalt. Tune color and coverage on the chosen road.

## Provenance and checks

Created with the built-in `image_gen.imagegen` tool. The two supplied screenshots
were used only as road-paint style references; the textures contain no screenshot
UI or branding. Each marking was generated separately. The crossing received
one cleanup edit to remove detached paint flecks between its six bars. Exact
generation and refinement prompts are in `generation_prompts.json`.

Validated PNG decoding, RGBA channels, opaque paint, transparent backgrounds,
antialiased edges, and unchanged copies of the selected generated files. All
marking silhouettes were visually inspected. Metadata and documentation are
UTF-8 without BOM with ASCII punctuation. Runtime decal projection, scale,
tiling joins, wet-material response, and distant filtering still need checking
inside NOX; no renderer code was changed or built for this asset-only pass.

The normal Release asset synchronization copies `textures`. The separate
`tools/Package.ps1` packaging path includes scene/config-referenced assets, so
add references or explicitly include this directory when integrating the pack.

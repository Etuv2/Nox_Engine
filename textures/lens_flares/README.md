# Night-driving lens flare textures

Six generated optical layers for warm street lamps, cool white LEDs, headlights,
and other bright visible sources. These are 2D sprites intended for placement at
3D light sources, using camera-facing quads. They are not volume textures or meshes.
This pack supplies assets and usage guidance; it does not add a renderer pass or
attach sprites to existing streetlights.

## Files

All six files are 1254 x 1254, 8-bit RGBA PNGs with soft alpha. Preserve the alpha
channel. The four optical layers are approximately neutral and can be tinted.

| File | Purpose | Starting width relative to glow | Starting gain |
| --- | --- | --- | --- |
| [glow_sodium.png](glow_sodium.png) | White core fading to amber sodium glow | 1.0 | 1.00 |
| [glow_led.png](glow_led.png) | White core fading to pale cool-blue LED glow | 1.0 | 1.00 |
| [starburst_4.png](starburst_4.png) | Four-ray diffraction for streetlights | 2.0 | 0.15 |
| [starburst_6.png](starburst_6.png) | Six-ray diffraction for brighter sources | 2.0 | 0.12 |
| [streak_horizontal.png](streak_horizontal.png) | Thin horizontal glare accent | 3.0 | 0.04 |
| [halo_ring.png](halo_ring.png) | Soft optical ring with transparent center | 3.0 | 0.02 |

Widths describe the entire square sprite, including transparent padding. Gains
are artistic ratios, not calibrated light units. Start with a glow 24-64 pixels
wide at the intended gameplay resolution, then tune against exposure and bloom.

## Suggested combinations

- Ordinary sodium streetlight: sodium glow plus four-ray starburst. Give the
  starburst a mild warm tint; leave the already colored glow untinted.
- Cool LED streetlight: LED glow plus six-ray starburst with a mild cool-white
  tint. Leave the already colored glow untinted.
- Very bright headlight or floodlight: either glow, one starburst, and a faint
  horizontal streak. Add the halo only for unusually strong glare.

Choose one starburst per source. Stacking both makes ordinary lamps too busy.
Keep streak and halo gains low; a halo on every distant lamp looks artificial.
The existing street kit uses a warmer 4000 K LED color. The supplied cool LED
glow is an artistic alternative, not an exact color-temperature match.

## Placement and visibility

Place the sprite pivot at the visible lamp lens or bulb, not the pole origin.
For the procedural street kit, find the emissive lens in
`src/ProceduralStreetKit.cpp`; the spotlight position is slightly below it.
Use UV center (0.5, 0.5) as the common pivot for all layers. The generated centers
are approximate, so check alignment when layering very small sources.

Build the quad from camera right and up so it always faces the viewer. Use the
same pivot for all layers and keep the streak horizontal in camera space. Scale
from the projected source size with a restrained minimum and maximum screen
size. Fade intensity with distance and source visibility rather than drawing
every lamp at the same size and strength. Reject sources behind the camera.

Test visibility at the source against scene depth or an occlusion query, and
fade all its layers together when a wall, vehicle, or lamp housing blocks it.
Sample a small area around the source to soften partial occlusion and smooth
visibility changes over time. Depth testing an entire world-space quad alone
can cut the flare rays at nearby silhouette edges. For a camera-optics overlay,
source occlusion should control the effect; disable depth writes and composite
the visible flare in screen space. Respect the fixture opening direction too:
a downward lamp should not glow equally through the back of its housing.

## Sampling and compositing

Load RGB as sRGB and let the GPU decode it to linear space; alpha remains linear.
NOX's file-based `Texture` constructor supports this with its `srgb` argument,
which defaults to false. Enable mipmaps, use linear filtering, and explicitly
set both wrap axes to `GL_CLAMP_TO_EDGE` instead of repeating the sprite.

Treat the files as straight-alpha input. For additive emission, output linear
RGB multiplied by tint and gain, with the sampled alpha as output alpha, then
blend with `GL_SRC_ALPHA, GL_ONE`. Alternatively, multiply RGB by alpha in the
shader and blend with `GL_ONE, GL_ONE`. Multiply alpha exactly once. An additive
blend using raw RGB with `GL_ONE, GL_ONE` can expose colors in transparent pixels.

Generate mipmaps with alpha-aware filtering if available, especially when these
sprites shrink to a few pixels. Inspect for fringes at the actual gameplay size.
Several generated files contain negligible edge alpha up to 2/255. A soft remap
`max((alpha - 2/255) / (1 - 2/255), 0)` removes that residual opacity; keep the
constants floating point. Apply it before the single alpha multiplication.

Composite into the linear HDR image before tone mapping. Match NOX's current
pre-exposure convention when converting source brightness to emission. Do not
multiply texture gain directly by the streetlight's lumen value. Disable depth
writes, receive no lighting, and cast no shadows. Keep the real emissive lamp
and its point/spot light; the sprite adds perceived camera glare, not illumination.
Tune the existing bloom with the flare pass to avoid counting the glow twice.
If rendered before TAA, account for camera-facing sprite motion and history
rejection; a later pass needs its own stable visibility and subpixel filtering.

## Provenance and validation

Generated using the built-in `image_gen.imagegen` tool with transparent output.
The exact six prompts are recorded in [generation_prompts.json](generation_prompts.json).
The prompts requested 1024 x 1024; the generator delivered matching 1254 x 1254
images. Files were copied unchanged, preserving the generated alpha channels.

Validated PNG decoding, matching dimensions, RGBA mode, soft transparency,
edge-alpha range, transparent halo center, and byte-identical copies of the
generated originals. Visually checked all six generated layers. The assets have
not been rendered inside NOX; runtime sizing, source alignment, occlusion, HDR
gain, and TAA behavior remain integration checks.

The normal Release asset synchronization copies the `textures` directory.
`tools/Package.ps1` copies scene/config-referenced assets; when integrating this
pack, add references or explicitly include this folder in that packaging path.

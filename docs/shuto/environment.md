# Night sky (HDRI) for the Shuto section

## Choice

`hdrs/cc0/kloppenheim_07_4k.hdr` - "Kloppenheim 07" by Greg Zaal, Poly Haven
(<https://polyhaven.com/a/kloppenheim_07>), CC0 1.0 (Poly Haven licence page:
<https://polyhaven.com/license>; commercial use and redistribution allowed, no attribution required).
Downloaded 2026-10-11 from the Poly Haven API (`https://api.polyhaven.com/files/kloppenheim_07`,
4k Radiance .hdr, 4096 x 2048, 26,535,613 bytes, MD5 `8658f900d16f5edaad8c198a6f2e4fcb`, verified).
Published metadata: categories night / overcast / artificial light, EV cap 22, white balance 3776 K.

It is an overcast night whose cloud base is lit from below by artificial light, which reads as urban
light pollution, with only a soft moon. The rest of the panorama is a rural field with a tree line and
a few lamps on the horizon. Those parts are removed in the engine at load time (below), so no foreign
geography appears behind Tokyo.

The previous sky, `hdrs/cc0/night_city.hdr`, is the Shanghai Bund (the Oriental Pearl Tower and the
Lujiazui skyline are baked into its horizon). It conflicted with the Tokyo geography. The engine
also sampled every equirect upside down until this change (see "Engine fix"), which partly hid it.

## Candidates considered

All are Poly Haven unless noted. Licences were checked against the source pages. Resolutions come
from the Poly Haven API `max_resolution`.

| Candidate | Max res | Horizon content | Moon / stars | Verdict |
| --- | --- | --- | --- | --- |
| kloppenheim_07 | 24576 x 12288 | field, tree line, warm distant lamps | soft moon, faint stars | chosen: best cloud structure lit from below |
| kloppenheim_04 | 24576 x 12288 | misty field, tree line, bright lamps | none | fallback quality: flat grey fog dome |
| rooftop_night | 16384 x 8192 | rooftop parking, floodlights | strong moon, purple twilight | rejected: twilight and moon too strong |
| donkin_reserve | 24576 x 12288 | plaza, monument, buildings | - | rejected: buildings on the horizon |
| moonless_golf | 16384 x 8192 | golf field, clubhouse | Milky Way | rejected: dark-sky stars, no city glow |
| courtyard_night | 16384 x 8192 | brick facades all round | moon | rejected: buildings |
| shanghai_bund, modern_buildings_night, neuer_zollhof, ... | - | real city skylines | - | rejected: foreign skylines |
| ambientCG NightSkyHDRI005 / NightEnvironmentHDRI002 (CC0) | 16k zips | tagged city / plaza | - | not tested: zip only, EXR/HDR content unverified |

Measured on the 2k version: the 99.9th luminance percentile is 1.30 and the maximum is 15,047. The
maximum is single horizon lamp pixels. The mean above the horizon is 8.2 times the mean below.

## Integration (engine, generic)

The scene `"skybox"` key accepts a path string (unchanged) or an object, processed on the CPU
before the cubemap, irradiance and prefiltered specular maps are generated (`src/EnvironmentMap.cpp`,
`src/Skybox.cpp`):

```json
"skybox": { "path": "hdrs/cc0/kloppenheim_07_4k.hdr", "fallback": "hdrs/cc0/night_clear.hdr",
            "rotation_deg": 0.0, "intensity": 1.0, "max_radiance": 8.0,
            "ground": { "color": [0.06, 0.05, 0.04], "blend_start_deg": 5.0, "blend_end_deg": -2.0 } }
```

- `rotation_deg` rotates about +Y.
- `intensity` scales linear radiance.
- `max_radiance` clamps luminance per texel, keeping hue. This removes the horizon lamp hot spots
  (602 texels at 4k) so the specular IBL has no fireflies.
- `ground` replaces everything below `blend_end_deg` with a linear colour and smoothsteps from
  `blend_start_deg`. The value used is a dark, warm, light-polluted ground at about the HDRI's own
  ground level (median 0.05). Its band hides the tree line (0 to 2 degrees).
- A missing or unreadable `path` falls back to `fallback`. If both fail, the sky is disabled, as before.

Data stays linear. Stb loads it unflipped, with the zenith row first.

The values come from measured radiance bands of the 4k file:

| Elevation band | Median RGB |
| --- | --- |
| 60 to 90 degrees | 0.27 / 0.32 / 0.38 |
| 5 to 10 degrees | 0.28 / 0.28 / 0.29 |
| 0 to 2 degrees (tree line, lamps) | 0.17 / 0.19 / 0.19, with lamps up to 36 |
| below the horizon | about 0.05 |

The Shuto values live in the content profile `tools/shuto/build_scene.py` (`SKYBOX`) and in the
generated Shuto scenes. The engine defaults are neutral.

### Engine fix

`shaders/equirect2cube_frag.glsl` mapped +Y to `v = 1`, the image's bottom row, because stb
uploads rows top-first. Every equirect sky was therefore upside down. It now maps the zenith to
`v = 0`. This affects all scenes that use a skybox. The NFS scenes use the same Shanghai panorama
and now show it the right way up.

## Validation

- Captures from the Rainbow Bridge approach, old and new: `captures/sky_test/old/road_rainbow/t0.90.png`
  and `captures/sky_test/0/road_rainbow/t0.90.png`.
- Fallback: a missing `path` logs the warning and loads `night_clear.hdr` (`captures/env_test/c_fallback`).
- The final look under the game's exposure and tone mapping is judged from road level in the round-3
  captures (see `docs/shuto/README.md`).

## Packaging

`hdrs/` is git-ignored. The packager (`tools/Package.ps1`) copies every `.hdr` referenced anywhere
in a packaged scene, including the object form's `path` and `fallback`. Keep this file and its
licence note with any distribution of the 4k HDRI.

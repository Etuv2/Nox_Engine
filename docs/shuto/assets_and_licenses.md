# Shuto kit: assets and licenses

Status as of 2026-10-10. Scope: the three supplied building GLBs, the generated
texture pack in textures/shuto/, and the derived files built from them.

## Source assets

### 1. asian_themed_low_poly_night_city_buildings.glb

- Origin: https://sketchfab.com/3d-models/asian-themed-low-poly-night-city-buildings-9f0343aff4814b758dc6e905aba5b5e0
- Author: 99.Miles (https://sketchfab.com/99.Miles)
- License (glTF asset.extras, verbatim): `CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)`
- Attribution required: credit 99.Miles, title "Asian Themed Low Poly Night City Buildings", link above, licence CC BY 4.0, and an indication that changes were made.
- Redistribution: allowed under CC BY 4.0 with attribution.
- Derived from it: `models/shuto/kit/buildings_asian_themed.glb` (17 buildings plus height variants; flare sprites removed to metadata; scale 0.3), `buildings_kit.blend`.

### 2. japanese_building__cc0.glb

- Origin: https://sketchfab.com/3d-models/japanese-building-cc0-4bde4dd51a3f4cacabc22324a7d0c62c
- Author: Lit For 20 (https://sketchfab.com/LitFor20)
- License (glTF asset.extras, verbatim): `SKETCHFAB Standard (https://sketchfab.com/licenses)`
- Sketchfab page check (fetched 2026-10-10): the licence field reads "Free Standard" with a "Learn more" link. The page says "CC0" only in the title "Japanese Building | CC0" and in a tag. It is NOT CC0. The page states no attribution text and no redistribution grant beyond the Sketchfab Standard terms.
- Attribution required: not stated on the page; treat as required by the Standard terms.
- Redistribution: NOT verified. Do NOT ship `models/shuto/kit/buildings_japanese.glb` or any derivative until the Sketchfab Standard licence terms are confirmed for redistributing modified geometry or textures. Use only for local development and for scale/layout reference. Ask the author or replace with a verified CC0 or CC-BY source.
- Notes: the source requires `KHR_materials_pbrSpecularGlossiness` (listed as required). NOX does not read it. Blender converted the materials to principled metal-rough on export, so the kit GLB does not use that extension.
- Derived from it: `buildings_japanese.glb` (1 building, main body 21 m tall, base-centre pivot, mirrored part re-wound, scale 1.0), its height variant, `buildings_kit.blend`.

### 3. low-poly_city_buildings.glb

- Origin: https://sketchfab.com/3d-models/low-poly-city-buildings-e0209ac5bb684d2d85e5ade96c92d2ff
- Author: smooth998 (https://sketchfab.com/smooth998)
- License (glTF asset.extras, verbatim): `CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)`
- Attribution required: credit smooth998, title "Low-poly City Buildings", link above, licence CC BY 4.0, indication of changes.
- Redistribution: allowed under CC BY 4.0 with attribution.
- Derived from it: `models/shuto/kit/buildings_lowpoly.glb` (22 buildings minus ground plane, plus height variants, 4096 textures downscaled to 2048, scale 9.4), `buildings_kit.blend`.

## Generated texture pack (textures/shuto/)

- Source: built-in image generation for this project (see textures/shuto/README.md and generation_prompts.json). No third-party image is included.
- Licence: project-internal. No external attribution required.
- Derived from it: `textures/shuto/derived/` (padded atlas patches, seamless surface derivatives, `derived_manifest.json`). Metrics are in that manifest.

## Derived project files

- `models/shuto/kit/roundtrip_test.glb`: generated in Blender (procedural geometry, procedural normal map, concrete_cast.png from the texture pack). No external assets.
- `models/shuto/kit/kit_index.json`: generated metadata. Licence field copied per building from the sources above.
- `scenes/shuto_roundtrip.json`: project scene.

## Shipping rules

- CC BY sources (asian_themed, lowpoly): may ship with the attribution above in the game credits or documentation.
- japanese: blocked until licence verified. The kit index marks its entries as not shippable.
- `models/` is git-ignored; the sources and kit GLBs are local build products unless a shipping step copies them.

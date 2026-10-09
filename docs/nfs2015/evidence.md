# Evidence notes and sources

Retrieved 2026-10-09. Tags as in `README.md`: [NFS], [FB], [OBS], [UNV].

## Primary sources read

1. **Bryn Alban, "Creating the Cars of Need for Speed"**, Speedhunters, July 2015 (mirror):
   https://automayt.github.io/speedhunters-archive/2015/07/creating-the-cars-of-need-for-speed/
   - Photo-referenced modelling with matched camera/lens; CAD for modern cars. [NFS]
   - "Physically Based Rendering (PBR) capabilities of our Frostbite engine". [NFS]
   - "around 24 parts and around 50 textures" per stock car; "around 25 materials". [NFS]
   - Look-dev prototyping of dust, dirt, damage and rain layering on cars. [NFS]

2. **Fredriksson & Scott, "Generating Real Time Reflections by Ray Tracing Approximate Geometry"**, Chalmers MSc thesis 2016:123 (supported by Ghost Games and Frostbite):
   https://odr.chalmers.se/bitstreams/f07cb909-2d71-49ac-8e7c-dad4d3b1b87f/download
   - Section 1.1: "The existing system for handling reflections uses screen space reflections (SSR) and image based lighting (IBL) through parallax-corrected cube maps". Failure modes: high velocity (cubemaps fail to update), dynamic lighting, parallax approximation. [NFS]
   - Section 2.2: cubemaps "generated in run time, by creating one side of the cube map per frame"; "manually inserted in the scenes"; seams between SSR and IBL; "The resolution of the cube maps are 128x128 for each side of the cube, and there are constantly 41 cube maps loaded in memory." [NFS]
   - Target hardware: Xbox One / PS4. The prototype (proxy-geometry ray tracing as SSR fallback) is research. [NFS]

3. **Lagarde & de Rousiers, "Moving Frostbite to Physically Based Rendering 3.0"**, SIGGRAPH 2014 course notes:
   https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf
   - Punctual light: `E = I / max(d^2, 0.01^2)`; window `smooth = saturate(1 - (d^2 * invR^2)^2); att *= smooth^2` (Listing 4). [FB]
   - Exposure: `EV100 = log2(N^2/t * 100/S)`, `maxLum = 1.2 * 2^EV100`, `exposure = 1/maxLum` (Listing 28); auto exposure from `log2(Lavg * 100/12.5)`. [FB]
   - Light buffer float16, pre-exposed with previous-frame exposure. [FB]
   - Specular occlusion: `saturate(pow(NdotV + AO, exp2(-16*roughness - 1)) - 1 + AO)`. [FB]
   - Reflection chain: SSR -> nested local light probes (parallax corrected) -> distant probe. [FB]
   - Reference luminances (cd/m^2): clear sky ~8000, overcast ~2000, moon ~2500; sun illuminance ~100k lux. [FB]

4. **Stachowiak & Uludag, "Stochastic Screen-Space Reflections"**, SIGGRAPH 2015 Advances:
   https://www.advances.realtimerendering.com/s2015/index.html
   - Tile classification, min/max ray allocation, hi-Z traversal, GGX importance sampling (Halton), ray reuse weighted by `brdf/pdf` and normalised, half-res trace, hit-depth temporal reprojection, roughness cutoff for HQ rays (< 0.2), PS4 2.19-4.41 ms at 1600x900 fully reflective. Example: Mirror's Edge Catalyst. **NFS not mentioned.** [FB]

5. **Hillaire, "Physically Based and Unified Volumetric Rendering in Frostbite"**, SIGGRAPH 2015 Advances (same index):
   - Froxels aligned to 16x16 light tiles; default 8x8 px volume tiles and 64 slices; media (absorption, scattering, phase g, emissive); RGBA16F V-buffer; sun via CSM and local lights from the tiled list; Halton jitter + 5% temporal blend; energy-conserving integration; PS4 900p 2.95 ms (8x8). [FB]
   - Slide 44: physically based sky "in production" in Mirror's Edge Catalyst, **Need for Speed** (Ghost), Mass Effect Andromeda. [NFS: in-production statement]

## Secondary sources

- Wikipedia, Need for Speed (2015): https://en.wikipedia.org/wiki/Need_for_Speed_(2015_video_game) - Frostbite 3; PS4/XB1 Nov 2015, PC Mar 2016; PC unlocked frame rate and 4K (citing IGN / official channel); reception quotes. [OBS]
- Notebookcheck benchmark article (snippet only; page returned 403): https://www.notebookcheck.net/Need-for-Speed-2016-Notebook-and-Desktop-Benchmarks.161519.0.html - ~12 settings, FXAA/TAA, SSAO/HBAO. [OBS]
- Steam store page, app 1262540: https://store.steampowered.com/app/1262540 - "Snapshot" photo feature, unlocked frame rate, 4K. Ten promotional 1920x1080 screenshots. [OBS]

## Not retrieved / unverified

- PCGamingWiki settings page (Cloudflare 403), Ventura Bay fan wiki (403), Digital Foundry analysis (not found), any Ghost GDC rendering talk (not found).
- No primary documentation of a wet-road system, rain system, or time-of-day progression for NFS 2015.

## Reference media (found by title; not yet reviewed frame-by-frame)

| URL | Platform | Settings | Type | Notes |
| --- | --- | --- | --- | --- |
| https://www.youtube.com/watch?v=2CcJj1lk5As | PC | 4K 60, Ultra | gameplay | stock per title |
| https://www.youtube.com/watch?v=PXjES6LDW1o | PC | Ultra | gameplay | |
| https://www.youtube.com/watch?v=yY4yEVlfm80 | PC (R9 Fury, VSR 4K) | Max/Ultra | gameplay | |
| https://www.youtube.com/watch?v=hlc2mE86Wh0 | PC (GTX 1050 Ti) | Ultra 1080p | gameplay | |
| https://www.youtube.com/watch?v=4CRapvR_XV8 | PC | 1080p60 | gameplay | |
| https://www.youtube.com/watch?v=ATavSaaqETc | unknown | unknown | gameplay | "Welcome in Ventura Bay" |
| https://www.youtube.com/watch?v=DBi0qyCJMmA | unknown | unknown | walkthrough | |
| https://www.youtube.com/watch?v=jYinI-llh30 | PC | Ultra + SweetFX | **modded** | excluded from targets |
| Steam screenshots (10) | PC store | n/a | **promotional** | look reference only |

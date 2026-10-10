# Shuto Expressway environment implementation prompt

Extend the current NOX Engine driving game into a complete, connected Shuto
Expressway driving environment. Use Blender, the three supplied building GLBs,
the already generated texture pack in textures/shuto/, and existing repository
assets. Image generation is already complete for this reusable environment kit;
do not depend on access to an image-generation tool.

Required building sources:
- asian_themed_low_poly_night_city_buildings.glb
- japanese_building__cc0.glb
- low-poly_city_buildings.glb

The sources are the original attachments from this conversation. Locate them by
their original filenames or attachment metadata. If unavailable in your session,
request the missing source files. Keep originals intact and verify their actual
licenses before redistribution.

1. Inspect and establish requirements.

Read repository guidance, README.md, docs/nfs2015/game.md, scene conventions,
and the current asset pipeline before planning changes. Build on NOX's existing
C++/OpenGL renderer, GLB imports, JSON scenes, driving controls, vehicle rig,
free roam, checkpoint races, editor, routing, minimap and saved best times.

Audit RoadNetwork, DriveMode and GameSession: planar roads and checkpoints need
proper elevation and road-level identification for stacked carriageways,
tunnels, slopes and interchanges. Preserve existing scenes and handling.

Before committing to major architecture or visual direction, clarify the
reference year, treatment of closed/historical routes, geographic scale,
acceptable compression, visual fidelity, target hardware/performance and
activity priorities. Clarify multiplayer only if it is desired. Continue useful
asset and repository inspection while waiting; record agreed requirements.

2. Define and verify complete coverage.

Research the overview and authoritative route sources:
https://en.wikipedia.org/wiki/Shuto_Expressway
https://www.shutoko.jp/driving/navi-map/
https://www.shutoko.jp/driving/route/

Use dated official maps, appropriately licensed geographic data, elevation
references and driving footage. Create a complete route/segment manifest for the
agreed era, including C1, C2, B, applicable Y sections, every Tokyo radial route,
and the Kanagawa and Saitama routes. Verify the list rather than treating these
examples as exhaustive.

Inventory both carriageways, lane directions, junction connectors, ramps,
entrances/exits, bridges, tunnels, toll facilities and parking areas. Define
boundaries at connections to other operators' roads, with usable exit handling.
Treat closures and historical sections explicitly. Recreate the full expressway
network; surrounding city scenery can use selective detail, with local driving
roads wherever activities or expressway access require them.

3. Use the delivered textures correctly.

Read textures/shuto/README.md, manifest.json and generation_prompts.json.
Use the seventeen selected files, including the -v2 tunnel and wear revisions:

- Roads: asphalt_fine.png; reuse textures/road_decals/ for suitable paint.
- Structures: concrete_cast.png, concrete_weathered.png,
  tunnel_tiles-v2.png, galvanized_steel.png, noise_barrier.png.
- Surroundings: corrugated_metal.png, rooftop_membrane.png,
  ground_gravel.png, foliage_cutouts.png.
- Buildings: facades_apartment.png, facades_commercial.png,
  facades_industrial.png; preserve useful original GLB textures and reuse
  textures/window_rooms/ where the required renderer integration is implemented.
- Signs/artwork: sign_panel_blanks.png, billboards_art.png,
  roadside_props.png, surface_wear_decals-v2.png.

These are 1254 x 1254 base-color/artwork images, including two alpha atlases.
Use manifest cell rectangles and content bounds. Do not repeat whole atlases.
Prevent internal mip bleed through padded extracted patches or correct atlas
sampling. Handle straight alpha and low-alpha fringe RGB correctly.

Use sensible scalar PBR values and modeled detail initially. Bake additional
maps in Blender only where they improve the result. Do not assume missing PBR
maps exist, certify generated surfaces as seamless, or use painted facade
shadows as real geometry. Validate repeats and make seamless derivatives in
Blender where necessary. Use existing HDRs and engine materials for sky, water,
glass, wetness, reflections and lighting.

Compose navigation signs deterministically over the delivered blank substrates.
Use properly licensed Japanese/Latin fonts, exact route names, arrows, shields,
exit identifiers and distances from verified road data. Rasterize or bake those
signs with normal graphics/Blender tools; no AI image generation is required.
Do not substitute fictional lettering for navigation information. Apply the
billboard artwork only to scenery.

4. Author the map in Blender.

Inspect and normalize each GLB's units, pivots, UVs, materials and geometry.
Convert the supplied buildings into a modular kit by separating components,
repairing materials, simplifying meshes and creating variations in height,
roofline, facade and window arrangement. Match district character and skyline
placement; model distinctive landmarks separately.

Build reusable road decks, ramps, barriers, piers, gantries, expansion joints,
noise screens, tunnel sections, toll booths, lamps and parking-area props.
Use curves, Geometry Nodes or Python where they simplify repeated work. Generate
geometry and gameplay metadata from one authoritative geographic road dataset
with stable segment, carriageway, lane and junction IDs.

Preserve lane widths, road curvature, elevation, banking, gradients and clearance
from reference. Include recognizable locations such as Rainbow Bridge,
Yokohama Bay Bridge, Yamate Tunnel, Tatsumi and Daikoku within the agreed network.

Validate one Blender-to-NOX round trip for scale, axes, transforms, normals,
UVs, textures and materials before mass export. Deliver editable .blend sources
and chunked GLB exports with separate collision meshes and road metadata.

5. Make the environment drive correctly.

Extend road contact, collision, navigation, respawns, checkpoints and traffic to
distinguish vertically overlapping road levels. Ensure smooth seams, continuous
barriers, stable high-speed wheel contact and reliable tunnel camera behavior.
An imported render mesh alone is insufficient.

Implement directional lane connections and left-hand traffic, including lane
following, merging, braking and adjustable density. Navigation, minimap routes,
signs and events must agree with the same graph.

Profile large-world precision and rendering. Add streaming, LODs, instancing,
culling, collision activation or origin management where required by evidence.
Preload enough road and collision ahead of the fastest supported vehicle and
avoid visible unloading across bridges or junctions.

6. Provide a range of driving activities.

Extend the existing event framework with configurable data for free exploration,
point-to-point sprints, loop time trials, checkpoint challenges, speed zones,
traffic-driving challenges, delivery/navigation runs and drift challenges in
suitable areas. Prioritize according to agreed scope.

Each implemented activity needs a valid route, accessible start, clear objective,
timing/scoring, completion results, retry and return to free roam. Distribute
activities across regions and road types. Use parking areas as event, rest,
meeting and photo locations. Preserve best times and add persistence only where
needed.

Build coherent night lighting with headlights, road lamps, selective lit windows,
sign illumination and restrained bloom. Add weather/time-of-day behavior within
the agreed scope and hardware budget.

7. Complete and validate the whole network.

First establish complete coverage and graph connectivity. Validate a playable
section combining an elevated road, ramp, stacked junction, tunnel, signage,
traffic, parking area and event. Then expand the proven pipeline across every
route. The representative section is a milestone, not the final delivery.

Track geometry, scenery, signs, collision, routing, traffic and activity coverage
separately. Test both directions and every junction connection. Validate
high-speed seams, wheel contact, overlapping levels, checkpoints, respawns,
camera collision, texture repetition/mips, save/load and packaged asset paths.

Use repository builds, smoke tests, graph checks, scripted route traversal and
manual driving. Measure frame times, memory and loading on the agreed hardware.
Inspect the actual NOX output as well as Blender.

Deliver the integrated map and activities, editable Blender sources, material
assignments, baked derivatives, reproducible export scripts, source/license
records, complete route checklist, screenshots and validation/performance
results. Document precise remaining gaps. Do not call an incomplete, disconnected
or unvalidated network complete.

Make focused changes and follow existing conventions. Save text/configuration/
scripts as UTF-8 without BOM with ASCII punctuation, except verified Japanese
content where explicitly required.

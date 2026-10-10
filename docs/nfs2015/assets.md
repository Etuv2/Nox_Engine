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

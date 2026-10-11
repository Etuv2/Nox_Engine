"""Compose a NOX scene for a Shuto section from generated assets (docs/shuto/README.md).

Inputs (all generated, never hand-edited):
  models/shuto/<section>/render/*.glb, collision/*_col.glb, lamps.json   (build_roads.py)
  models/shuto/<section>/scenery/*.gltf, scenery_collision/*_col.glb       (build_scenery.py, optional)
  models/shuto/<section>/signs.glb                                       (compose_signs.py, optional)
  data/shuto/nox_road_network.json, data/shuto/network.json              (build_network.py)
  data/shuto/activities_<section>.json                                    (events / places, optional)
Template: render settings, car rig, handling and HUD come from scenes/nfs_sprint.json (unchanged).

Usage: python -I tools/shuto/build_scene.py --section rep_section --out scenes/shuto_rep_section.json
"""
import argparse
import glob
import json
import math
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SODIUM = [1.0, 0.56, 0.2]   # classic-era Shuto lighting is high-pressure sodium
# Scene-level render overrides for an elevated city-scale view (the template is tuned for a
# 400 m street): thinner distance haze so the skyline reads over kilometres, longer froxel range.
HEADLIGHT = {'type': 'spot', 'color': [0.95, 0.97, 1.0], 'luminous_power': 1600.0, 'intensity': 1.0, 'range': 60.0,
             'cutOff': 14.0, 'outerCutOff': 30.0, 'source_radius': 0.05, 'castsShadows': False, 'enabled': True}
CAR_LIGHTS = {   # car-local transforms (models/car_r33_rig.glb), as in scenes/nfs_night_street.json
    'Headlight_L': ([0.62, 0.68, 2.25], [-3.0, 180.0, 0.0], HEADLIGHT),
    'Headlight_R': ([-0.62, 0.68, 2.25], [-3.0, 180.0, 0.0], HEADLIGHT),
    'TailLight': ([0.0, 0.8, -2.35], [0.0, 0.0, 0.0], {'type': 'point', 'color': [1.0, 0.05, 0.02], 'luminous_power': 120.0,
                  'intensity': 1.0, 'range': 5.0, 'source_radius': 0.3, 'castsShadows': False, 'enabled': True}),
}
# Night sky (docs/shuto/environment.md): Poly Haven "Kloppenheim 07" (CC0), an overcast night whose
# cloud base is lit from below. Its rural ground and tree line are replaced by a dark light-polluted
# ground below the horizon and the horizon lamps are clamped, so no foreign skyline or field shows.
SKYBOX = {'path': 'hdrs/cc0/kloppenheim_07_4k.hdr', 'fallback': 'hdrs/cc0/night_clear.hdr', 'rotation_deg': 0.0,
          'intensity': 1.0, 'max_radiance': 8.0,
          'ground': {'color': [0.06, 0.05, 0.04], 'blend_start_deg': 5.0, 'blend_end_deg': -2.0}}
RENDER_OVERRIDES = {
    'fog_density': 0.0025,
    'fog_distant_density': 0.00045,
    'fog_height_falloff': 0.04,
    'volumetric_far': 400.0,
}


def rel(p):
    return os.path.relpath(p, ROOT).replace('\\', '/')


def chunk_files(pattern):
    return sorted(rel(p) for p in glob.glob(os.path.join(ROOT, pattern)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--section', default='rep_section')
    ap.add_argument('--out', default='scenes/shuto_rep_section.json')
    ap.add_argument('--template', default='scenes/nfs_sprint.json')
    ap.add_argument('--max-lights', type=int, default=600)
    args = ap.parse_args()
    sec = os.path.join('models', 'shuto', args.section)
    tpl = json.load(open(os.path.join(ROOT, args.template), encoding='utf-8'))

    scene = {k: v for k, v in tpl.items() if k not in ('nodes', 'game', 'camera')}
    scene['scene_name'] = 'shuto_' + args.section
    scene['render_settings'] = dict(scene.get('render_settings', {}), **RENDER_OVERRIDES)
    scene['skybox'] = SKYBOX
    nodes = []
    for path in chunk_files(os.path.join(sec, 'render', '*.glb')) + chunk_files(os.path.join(sec, 'scenery', '*.gltf')):
        name = 'Shuto_' + os.path.splitext(os.path.basename(path))[0] + ('_scenery' if '/scenery/' in path else '_road')
        nodes.append({'type': 'model', 'name': name, 'path': path, 'position': [0, 0, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1]})
    if os.path.exists(os.path.join(ROOT, sec, 'signs.glb')):
        nodes.append({'type': 'model', 'name': 'Shuto_signs', 'path': rel(os.path.join(ROOT, sec, 'signs.glb')),
                      'position': [0, 0, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1]})

    # Road lamps -> downward spot lights (no shadows). Evenly thinned to the light budget.
    lamps = json.load(open(os.path.join(ROOT, sec, 'lamps.json'), encoding='utf-8'))['lamps']
    step = max(1, math.ceil(len(lamps) / max(args.max_lights, 1)))
    for lamp in lamps[::step]:
        p = lamp['pos']
        nodes.append({'type': 'light', 'name': f"shuto_lamp_{lamp['id']}", 'position': [p[0], p[1] - 0.1, p[2]],
                      'rotation': [-90.0, 0.0, 0.0],
                      'light': {'type': 'spot', 'color': SODIUM if lamp.get('kind') != 'tunnel' else lamp['color'],
                                'luminous_power': 22000.0 if lamp.get('kind') != 'tunnel' else 7000.0, 'intensity': 1.0,
                                'range': float(lamp.get('range', 30.0)) + 5.0, 'source_radius': 0.25, 'castsShadows': False,
                                'cutOff': 40.0, 'outerCutOff': 70.0}})

    # Player car from the template. Car light transforms come from CAR_LIGHTS: the template's
    # saved light positions were zeroed by an engine bug (LightNode::SetLight, fixed).
    tg = tpl['game']
    def fix_lights(n):
        if n.get('type') == 'light' and n.get('name') in CAR_LIGHTS:
            pos, rot, light = CAR_LIGHTS[n['name']]
            n['position'], n['rotation'], n['light'] = list(pos), list(rot), dict(light)
        for c in n.get('children', []):
            fix_lights(c)
    for n in tpl['nodes']:
        if n.get('name') == tg.get('player_node'):
            fix_lights(n)
            nodes.append(n)
    scene['nodes'] = nodes

    game = {k: v for k, v in tg.items() if k not in ('road_network', 'events', 'spawn', 'drivable_areas')}
    game['road_network'] = {'file': 'data/shuto/nox_road_network.json'}
    # World map screen (src/WorldMapUI): baked base image (tools/shuto/make_worldmap.py), labels, metadata.
    game['world_map'] = {'image': 'textures/shuto/map/worldmap_base.png', 'bounds': [-19000, -23500, 17000, 37500],
                         'labels': 'data/shuto/map_labels.json', 'network': 'data/shuto/network.json',
                         'attribution': 'Map data (c) OpenStreetMap contributors (ODbL)'}
    game['collision'] = {'files': chunk_files(os.path.join(sec, 'collision', '*_col.glb')) +
                         chunk_files(os.path.join(sec, 'scenery_collision', '*_col.glb')), 'height_tolerance': 4.0}
    acts_path = os.path.join(ROOT, 'data', 'shuto', f'activities_{args.section}.json')
    if os.path.exists(acts_path):
        acts = json.load(open(acts_path, encoding='utf-8'))
        game['events'] = acts.get('events', [])
        if acts.get('places'):
            game['places'] = acts['places']
        if acts.get('spawn'):
            game['spawn'] = acts['spawn']
    else:
        game['events'] = []
    scene['game'] = game
    sp = game.get('spawn', {}).get('position', [0, 3, 0])
    h = math.radians(game.get('spawn', {}).get('heading_deg', 0.0))
    scene['camera'] = {'position': [sp[0] - math.sin(h) * 12, sp[1] + 5, sp[2] - math.cos(h) * 12], 'look_at': sp,
                       'fov': 55.0, 'near': 0.1, 'far': 3000.0, 'movement_speed': 20.0}
    out = os.path.join(ROOT, args.out)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(scene, f, indent=1, ensure_ascii=True)
        f.write('\n')
    print(f'{args.out}: {len(nodes)} nodes ({len(lamps[::step])} lamps), {len(game["collision"]["files"])} collision files, '
          f'{len(game["events"])} events')


if __name__ == '__main__':
    main()

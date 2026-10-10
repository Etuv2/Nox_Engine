"""Debug a spot of a generated Shuto section: collision plot + network centrelines + nearby carriageways.

Usage: python -I tools/shuto/debug_spot.py <section> x z [radius] [--log logs/run.log]
Writes captures/shuto/debug/spot_<x>_<z>.png and prints carriageways within radius.
"""
import glob
import json
import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def main():
    args = sys.argv[1:]
    log = None
    if '--log' in args:
        i = args.index('--log')
        log = args[i + 1]
        del args[i:i + 2]
    section, x, z = args[0], float(args[1]), float(args[2])
    r = float(args[3]) if len(args) > 3 else 50.0
    sys.stdout.reconfigure(encoding='utf-8')
    net = json.load(open(os.path.join(ROOT, 'data', 'shuto', 'network.json'), encoding='utf-8'))
    ys = []
    print('carriageways within %.0f m of (%.1f, %.1f):' % (r, x, z))
    for c in net['carriageways']:
        P = c['points']
        d, i = min((math.hypot(p[0] - x, p[2] - z), k) for k, p in enumerate(P))
        if d < r:
            ys.append(P[i][1])
            s = sum(math.hypot(b[0] - a[0], b[2] - a[2]) for a, b in zip(P[:i], P[1:i + 1]))
            print('  %-18s %-8s d=%5.1f y=%7.2f s=%6.0f/%6.0f lanes=%d %s' % (
                c['id'], c['kind'], d, P[i][1], s, sum(math.hypot(b[0] - a[0], b[2] - a[2]) for a, b in zip(P, P[1:])),
                c['lanes'], [t['type'] for t in c.get('structure', [])]))
    y0 = (min(ys) - 3) if ys else -20
    y1 = (max(ys) + 4) if ys else 60
    out = os.path.join(ROOT, 'captures', 'shuto', 'debug', 'spot_%d_%d.png' % (x, z))
    cmd = [sys.executable, '-I', os.path.join(ROOT, 'tools', 'shuto', 'plot_collision.py'), out,
           str(x - r), str(z - r), str(x + r), str(z + r), str(y0), str(y1)]
    cmd += sorted(glob.glob(os.path.join(ROOT, 'models', 'shuto', section, 'collision', '*_col.glb')))
    if log:
        cmd += ['--track', log]
    subprocess.run(cmd, check=True)
    img = Image.open(out)
    d = ImageDraw.Draw(img)
    W = img.width
    S = lambda p: ((p[0] - (x - r)) / (2 * r) * W, (p[2] - (z - r)) / (2 * r) * W)
    for c in net['carriageways']:
        P = [p for p in c['points'] if abs(p[0] - x) < r * 1.5 and abs(p[2] - z) < r * 1.5]
        if len(P) < 2:
            continue
        d.line([S(p) for p in P], fill=(255, 255, 255), width=1)
        d.text(S(P[len(P) // 2]), '%s y%.1f' % (c['id'], P[len(P) // 2][1]), fill=(255, 255, 255))
    d.ellipse([W / 2 - 6, W / 2 - 6, W / 2 + 6, W / 2 + 6], outline=(0, 200, 255), width=2)
    img.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main()

"""Save every embedded image of a GLB to an output folder as PNG (Blender headless).

Usage: blender -b --factory-startup --python-exit-code 1 --python dump_textures.py -- <glb> <outdir>
"""
import os
import sys
import bpy

args = sys.argv[sys.argv.index("--") + 1:]
src, outdir = args[0], args[1]
os.makedirs(outdir, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
for img in bpy.data.images:
    if img.size[0] == 0:
        continue
    name = "".join(c if c.isalnum() or c in "_-." else "_" for c in img.name) + ".png"
    img.filepath_raw = os.path.join(outdir, name)
    img.file_format = "PNG"
    img.save()
    print("saved", name, tuple(img.size))

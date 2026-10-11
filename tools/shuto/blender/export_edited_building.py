"""Export an edited kit collection with its base pivot and NOX material extras.

blender -b --python this_file -- --blend FILE --collection NAME --output FILE.glb
Output must be within models/shuto/buildings or an immediate subdirectory so
the shared textures remain portable. This never saves over the editable file.
"""
import argparse
from pathlib import Path
import sys

import bpy

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_architecture as architecture

parser = argparse.ArgumentParser()
parser.add_argument("--blend", required=True)
parser.add_argument("--collection", required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
output = Path(args.output).resolve()
assert output.suffix.lower() == ".glb"
assert output.parent == architecture.OUT or output.parent.parent == architecture.OUT
bpy.ops.wm.open_mainfile(filepath=str(Path(args.blend).resolve()))
collection = bpy.data.collections[args.collection]
objects = [obj for obj in collection.all_objects if obj.type == "MESH"]
assert objects, "Collection has no meshes"
for obj in objects:
    obj.location -= collection.instance_offset
for material in bpy.data.materials:
    if "nox" in material and "interior" in material["nox"]:
        architecture.ROOMS[material.name] = material["nox"]["interior"].to_dict()
architecture.export(objects, output)
print("Exported", len(objects), "editable mesh objects to", output)

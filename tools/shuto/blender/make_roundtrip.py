"""Build the Blender-to-NOX round-trip test GLB (Blender headless).

Run from the repository root:
  blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/make_roundtrip.py -- <repo_root>

Output: models/shuto/kit/roundtrip_test.glb (glTF Y-up; Blender +X is glTF +X, east in NOX).
Contents:
  - 10 m cube standing on the ground at (+20, 0, 0) (base at z=0)
  - 2 m tall arrow in the XZ plane pointing +X at (0, -10, 0), plus 'EAST' text on the ground
  - 20 x 20 m ground plane with concrete_cast.png at 2 m per tile (UV 0..10)
  - normal-mapped 3 x 3 m panel at (-10, 20, 1.5), generated procedural normal map
  - emissive 3 x 3 m panel at (10, 20, 1.5), emission strength 5 (KHR_materials_emissive_strength)
"""
import math
import os
import sys

import bmesh
import bpy
from mathutils import Vector


def new_principled(name, base_rgb=(0.8, 0.8, 0.8), rough=0.8, metal=0.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*base_rgb, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    return mat, bsdf


def make_box(name, size, loc, mat):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc)
    o = bpy.context.active_object
    o.name = name
    o.scale = (size[0], size[1], size[2])
    bpy.ops.object.transform_apply(scale=True)
    o.data.materials.append(mat)
    return o


def make_plane(name, w, d, loc, rot, mat, uv_tiles=1.0):
    bpy.ops.mesh.primitive_plane_add(size=1.0, location=loc, rotation=rot)
    o = bpy.context.active_object
    o.name = name
    o.scale = (w, d, 1.0)
    bpy.ops.object.transform_apply(scale=True)
    o.data.materials.append(mat)
    if uv_tiles != 1.0:
        bm = bmesh.new()
        bm.from_mesh(o.data)
        uv = bm.loops.layers.uv.active
        for f in bm.faces:
            for l in f.loops:
                l[uv].uv = l[uv].uv * uv_tiles
        bm.to_mesh(o.data)
        bm.free()
    return o


def make_arrow(name, loc):
    """2 m tall flat arrow standing in the XZ plane, tip pointing +X (east)."""
    outline = [(-1.5, 0.8), (0.2, 0.8), (0.2, 0.0), (1.5, 1.0), (0.2, 2.0), (0.2, 1.2), (-1.5, 1.2)]
    me = bpy.data.meshes.new(name)
    me.from_pydata([(x, 0.0, z) for x, z in outline], [], [tuple(range(len(outline)))])
    me.update()
    o = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(o)
    o.location = loc
    return o


def make_normal_map(size=256):
    """Procedural bump normal map (sinusoidal dimples), tangent space, linear data."""
    img = bpy.data.images.new("panel_normal_img", width=size, height=size, alpha=False)
    pix = []
    k = 3.0
    for y in range(size):
        for x in range(size):
            u = 2 * math.pi * 4 * x / size
            v = 2 * math.pi * 4 * y / size
            n = Vector((-k * math.cos(u) * 0.3, -k * math.cos(v) * 0.3, 1.0)).normalized()
            pix.extend([n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5, 1.0])
    img.pixels = pix
    img.colorspace_settings.name = "Non-Color"
    img.pack()
    return img


def main():
    repo = sys.argv[sys.argv.index("--") + 1]
    os.chdir(repo)
    bpy.ops.wm.read_factory_settings(use_empty=True)

    tex_path = os.path.abspath(os.path.join("textures", "shuto", "concrete_cast.png"))
    concrete_img = bpy.data.images.load(tex_path)
    concrete_mat, bsdf = new_principled("concrete_cast", rough=0.85)
    tex = concrete_mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = concrete_img
    concrete_mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])

    cube_mat, _ = new_principled("roundtrip_cube", base_rgb=(0.9, 0.2, 0.1), rough=0.5)
    cube = make_box("roundtrip_cube_10m", (10, 10, 10), (20, 0, 5), cube_mat)

    arrow_mat, _ = new_principled("roundtrip_arrow_white", base_rgb=(0.95, 0.95, 0.95), rough=0.6)
    arrow = make_arrow("roundtrip_arrow_east", (0, -10, 0))
    arrow.data.materials.append(arrow_mat)

    bpy.ops.object.text_add(location=(0, -15, 0.01))
    text = bpy.context.active_object
    text.name = "roundtrip_text_EAST"
    text.data.body = "EAST"
    text.data.extrude = 0.02
    text.data.size = 1.2
    text.data.align_x = "CENTER"
    text.rotation_euler = (0, 0, 0)
    text.data.materials.append(arrow_mat)

    ground = make_plane("roundtrip_ground_concrete", 20, 20, (0, 20, 0), (0, 0, 0), concrete_mat, uv_tiles=10.0)

    normal_mat, nb = new_principled("roundtrip_normal_panel", base_rgb=(0.6, 0.6, 0.62), rough=0.4, metal=0.0)
    nimg = make_normal_map()
    ntex = normal_mat.node_tree.nodes.new("ShaderNodeTexImage")
    ntex.image = nimg
    nmapnode = normal_mat.node_tree.nodes.new("ShaderNodeNormalMap")
    normal_mat.node_tree.links.new(ntex.outputs["Color"], nmapnode.inputs["Color"])
    normal_mat.node_tree.links.new(nmapnode.outputs["Normal"], nb.inputs["Normal"])
    panel_n = make_box("roundtrip_normal_panel", (3, 0.1, 3), (-10, 20, 1.5), normal_mat)

    emis_mat, eb = new_principled("roundtrip_emissive_panel", base_rgb=(0.1, 0.1, 0.1), rough=0.5)
    eb.inputs["Emission Color"].default_value = (1.0, 0.55, 0.15, 1.0)
    eb.inputs["Emission Strength"].default_value = 5.0
    panel_e = make_box("roundtrip_emissive_panel", (3, 0.1, 3), (10, 20, 1.5), emis_mat)

    bpy.ops.object.select_all(action="SELECT")
    out = os.path.abspath(os.path.join("models", "shuto", "kit", "roundtrip_test.glb"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", use_selection=True, export_yup=True,
                              export_apply=False, export_image_format="AUTO", export_lights=False,
                              export_cameras=False)
    print("wrote", out)


if __name__ == "__main__":
    main()

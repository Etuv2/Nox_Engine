"""Read-only audit of the supplied GLBs. Run inside Blender from the repo root."""
import hashlib
import json
import math
from pathlib import Path
import struct

import bmesh
import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[3]
FILES = (
    "asian_themed_low_poly_night_city_buildings.glb",
    "japanese_building__cc0.glb",
    "low-poly_city_buildings.glb",
)


def render_source(filename):
    points = [o.matrix_world @ Vector(c) for o in bpy.data.objects if o.type == "MESH" for c in o.bound_box]
    lo = Vector(tuple(min(p[i] for p in points) for i in range(3)))
    hi = Vector(tuple(max(p[i] for p in points) for i in range(3)))
    center = (lo + hi) / 2
    span = max(hi - lo)
    bpy.ops.object.camera_add(location=center + Vector((span, -span * 1.4, span)))
    camera = bpy.context.object
    camera.rotation_euler = (center - camera.location).to_track_quat('-Z', 'Y').to_euler()
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = span * 1.5
    camera.data.clip_end = span * 10
    scene = bpy.context.scene
    scene.camera = camera
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 1200
    scene.render.resolution_y = 900
    scene.render.resolution_percentage = 100
    scene.world = bpy.data.worlds.new("AuditWorld")
    scene.world.color = (0.25, 0.25, 0.25)
    bpy.ops.object.light_add(type="SUN", rotation=(0.5, -0.5, -0.3))
    bpy.context.object.data.energy = 2
    scene.view_settings.view_transform = "AgX"
    out = ROOT / "captures/shuto/buildings/sources"
    out.mkdir(parents=True, exist_ok=True)
    scene.render.filepath = str(out / (Path(filename).stem + ".png"))
    bpy.ops.render.render(write_still=True)


def main():
    report = {"format": "nox-building-source-audit", "blender": bpy.app.version_string,
              "sources_unmodified": True, "sources": []}
    for filename in FILES:
        path = ROOT / "models/shuto/src/originals" / filename
        raw = path.read_bytes()
        doc = json.loads(raw[20:20 + struct.unpack_from("<I", raw, 12)[0]])
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.gltf(filepath=str(path))
        src = {"file": filename, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
               "asset_metadata": doc["asset"], "extensions_required": doc.get("extensionsRequired", []),
               "extensions_used": doc.get("extensionsUsed", []), "objects": [], "materials": [],
               "images": []}
        for obj in bpy.data.objects:
            item = {"name": obj.name, "type": obj.type,
                    "parent": obj.parent.name if obj.parent else None,
                    "matrix_world_columns": [list(c) for c in obj.matrix_world.col],
                    "origin_world": list(obj.matrix_world.translation),
                    "mirrored": obj.matrix_world.determinant() < 0}
            if obj.type == "MESH":
                mesh = obj.data
                points = [obj.matrix_world @ v.co for v in mesh.vertices]
                lo = [min(p[i] for p in points) for i in range(3)]
                hi = [max(p[i] for p in points) for i in range(3)]
                bm = bmesh.new()
                bm.from_mesh(mesh)
                pending = set(bm.verts)
                components = []
                while pending:
                    stack = [pending.pop()]
                    count = 0
                    while stack:
                        v = stack.pop()
                        count += 1
                        for edge in v.link_edges:
                            other = edge.other_vert(v)
                            if other in pending:
                                pending.remove(other)
                                stack.append(other)
                    components.append(count)
                item.update(vertices=len(mesh.vertices), polygons=len(mesh.polygons),
                            triangles=sum(len(p.vertices) - 2 for p in mesh.polygons),
                            bounds_blender_z_up=[lo, hi], dimensions=[hi[i] - lo[i] for i in range(3)],
                            materials=[m.name if m else None for m in mesh.materials],
                            boundary_edges=sum(e.is_boundary for e in bm.edges),
                            non_manifold_edges=sum(not e.is_manifold for e in bm.edges),
                            loose_vertices=sum(not v.link_faces for v in bm.verts),
                            zero_area_faces=sum(f.calc_area() < 1e-10 for f in bm.faces),
                            connected_components=len(components), component_vertex_counts=sorted(components, reverse=True),
                            uv_layers=[])
                bm.free()
                for uv in mesh.uv_layers:
                    coords = [tuple(loop.uv) for loop in uv.data]
                    item["uv_layers"].append({"name": uv.name, "loops": len(coords),
                                              "min": [min(p[i] for p in coords) for i in range(2)],
                                              "max": [max(p[i] for p in coords) for i in range(2)],
                                              "nonfinite": sum(not all(math.isfinite(v) for v in p) for p in coords)})
            src["objects"].append(item)
        for mat in bpy.data.materials:
            nodes = mat.node_tree.nodes if mat.use_nodes else []
            p = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
            src["materials"].append({"name": mat.name,
                                     "roughness": p.inputs["Roughness"].default_value if p else None,
                                     "metallic": p.inputs["Metallic"].default_value if p else None,
                                     "emission_strength": p.inputs["Emission Strength"].default_value if p else None,
                                     "image_nodes": [{"image": n.image.name, "colorspace": n.image.colorspace_settings.name}
                                                     for n in nodes if n.type == "TEX_IMAGE" and n.image]})
        for img in bpy.data.images:
            src["images"].append({"name": img.name, "size": list(img.size),
                                  "channels": img.channels, "packed": bool(img.packed_file),
                                  "colorspace": img.colorspace_settings.name})
        report["sources"].append(src)
        print(filename, "meshes", sum(o["type"] == "MESH" for o in src["objects"]),
              "triangles", sum(o.get("triangles", 0) for o in src["objects"]), flush=True)
        render_source(filename)
    out = ROOT / "data/shuto/buildings/source_audit.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

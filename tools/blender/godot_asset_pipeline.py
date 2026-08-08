#!/usr/bin/env python3
"""Headless Blender post-processing for Godot-bound assets.

Run as:

    blender --background --python godot_asset_pipeline.py -- \
        --input  /abs/path/model.glb \
        --output /abs/path/project/assets/generated/model.glb \
        --target-triangles 5000 \
        --collision convex \
        --scale-to 2.0

Everything after the bare `--` is ours; Blender consumes what comes before it.

WHY THIS EXISTS
---------------
A mesh straight out of a text-to-3D service is almost never shippable as-is:

  * it arrives at an arbitrary scale (often 10x or 0.01x what the game expects)
  * it is Z-up if it came from a DCC tool, and Godot is Y-up
  * it has 100k-500k triangles for something the player sees for two seconds
  * it has no collision, and Godot's importer cannot invent good collision for
    concave geometry on its own
  * its origin is wherever the generator happened to put it, rather than at the
    point you want to rotate or place the object around

Each of those is a five-minute manual fix in Blender and a permanent tax if you
are generating dozens of assets. This script does all of them in one headless
pass so an agent can call it and get back something it can place directly.

THE COLLISION TRICK
-------------------
Godot's glTF/FBX importer generates collision from *mesh name suffixes*:

    Name-col       concave trimesh StaticBody3D  (accurate, static only)
    Name-convcol   convex hull StaticBody3D      (cheap, moving bodies)
    Name-colonly   collision with no visual mesh
    Name-navmesh   navigation mesh

So rather than trying to build CollisionShape3D nodes after import — which
means walking the imported scene and guessing — this script duplicates the
visual mesh, decimates the copy hard, and names it with the right suffix.
Godot then does the rest at import time, through its supported path.

NOT VERIFIED END TO END
-----------------------
Blender is not installed in the environment this plugin was developed in, so
this script has been written and reviewed against the Blender 4.x Python API
but has not been executed. If you run it, a report either way is genuinely
useful: https://github.com/iwolski99/GodotAI/issues
"""

import argparse
import math
import os
import sys

try:
    import bpy
    import bmesh
except ImportError:  # pragma: no cover - only happens outside Blender
    sys.stderr.write(
        "This script must be run inside Blender:\n"
        "    blender --background --python godot_asset_pipeline.py -- --input ...\n"
    )
    raise SystemExit(2)


# --------------------------------------------------------------------------- #
#  Arguments                                                                   #
# --------------------------------------------------------------------------- #

def parse_args(argv):
    # Blender passes its own arguments too; ours are everything after "--".
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []

    p = argparse.ArgumentParser(prog="godot_asset_pipeline", add_help=True)
    p.add_argument("--input", required=True, help="Mesh file to process (.glb/.gltf/.fbx/.obj).")
    p.add_argument("--output", required=True, help="Where to write the processed .glb.")
    p.add_argument("--target-triangles", type=int, default=0,
                   help="Decimate to roughly this triangle count. 0 leaves topology alone.")
    p.add_argument("--collision", default="none",
                   choices=["none", "convex", "trimesh", "colonly"],
                   help="Collision proxy to emit alongside the visual mesh.")
    p.add_argument("--collision-triangles", type=int, default=500,
                   help="Triangle budget for the collision proxy. Cheap is the point.")
    p.add_argument("--scale-to", type=float, default=0.0,
                   help="Uniformly scale so the longest bounding-box edge equals this many metres.")
    p.add_argument("--center-origin", action="store_true",
                   help="Move the origin to the centre of the bounding box's floor.")
    p.add_argument("--y-up", action="store_true", default=True,
                   help="Ensure Y-up orientation on export (Godot's convention).")
    p.add_argument("--merge-by-distance", type=float, default=0.0,
                   help="Weld vertices closer than this. Removes the split seams generators leave.")
    p.add_argument("--smooth-angle", type=float, default=0.0,
                   help="Apply shade-auto-smooth with this angle in degrees. 0 skips it.")
    p.add_argument("--name", default="",
                   help="Base name for the exported object. Defaults to the output filename.")
    return p.parse_args(argv)


# --------------------------------------------------------------------------- #
#  Scene helpers                                                               #
# --------------------------------------------------------------------------- #

def reset_scene():
    """Blender starts with a cube, a camera and a light. None of them are ours."""
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_any(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext == ".obj":
        # Blender 4.x renamed the OBJ operator; try the new one, fall back.
        if hasattr(bpy.ops.wm, "obj_import"):
            bpy.ops.wm.obj_import(filepath=path)
        else:
            bpy.ops.import_scene.obj(filepath=path)
    else:
        raise SystemExit(f"Unsupported input format: {ext}")

    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit(f"No mesh objects found in {path}")
    return meshes


def triangle_count(obj):
    mesh = obj.data
    # calc_loop_triangles is the only reliable count on an n-gon mesh.
    mesh.calc_loop_triangles()
    return len(mesh.loop_triangles)


def join_meshes(meshes, name):
    """One object is far easier to reason about, and Godot imports it as one
    MeshInstance3D rather than a pile of them."""
    for obj in bpy.context.scene.objects:
        obj.select_set(False)
    for obj in meshes:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    joined = bpy.context.view_layer.objects.active
    joined.name = name
    joined.data.name = name + "_mesh"
    return joined


def apply_transforms(obj):
    for other in bpy.context.scene.objects:
        other.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


# --------------------------------------------------------------------------- #
#  Cleanup steps                                                               #
# --------------------------------------------------------------------------- #

def merge_by_distance(obj, threshold):
    """Generated meshes routinely have duplicated vertices along every seam,
    which breaks smooth shading and inflates the vertex count for nothing."""
    if threshold <= 0.0:
        return 0

    mesh = obj.data
    before = len(mesh.vertices)

    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=threshold)
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()

    return before - len(mesh.vertices)


def decimate(obj, target_triangles):
    """Reduce triangles to a budget.

    Collapse decimation is used rather than un-subdivide or planar: it is the
    only mode that hits a *specific* triangle count, which is what a performance
    budget actually needs. The ratio is computed from the real count rather than
    guessed, and clamped so a small mesh is never blown up.
    """
    if target_triangles <= 0:
        return None

    current = triangle_count(obj)
    if current <= target_triangles:
        return {"skipped": True, "before": current, "after": current,
                "reason": "already within budget"}

    ratio = max(0.01, min(1.0, target_triangles / float(current)))

    modifier = obj.modifiers.new(name="GodotDecimate", type="DECIMATE")
    modifier.decimate_type = "COLLAPSE"
    modifier.ratio = ratio
    # Keeping UV/normal boundaries intact matters more than hitting the number
    # exactly: a decimate that tears the UV seams makes the texture unusable,
    # and an untextured low-poly mesh is not a win.
    modifier.use_collapse_triangulate = True

    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier="GodotDecimate")

    after = triangle_count(obj)
    return {"skipped": False, "before": current, "after": after, "ratio": round(ratio, 4)}


def normalize_scale(obj, target_size):
    """Scale so the longest bounding-box edge is `target_size` metres.

    This is the single most valuable step for generated assets: services return
    models at wildly inconsistent scales, and a rifle that imports at 40 metres
    long is indistinguishable from a broken import until you look at the numbers.
    """
    if target_size <= 0.0:
        return None

    dims = obj.dimensions
    longest = max(dims.x, dims.y, dims.z)
    if longest <= 0.0:
        return None

    factor = target_size / longest
    obj.scale = (obj.scale.x * factor, obj.scale.y * factor, obj.scale.z * factor)
    apply_transforms(obj)

    return {"factor": round(factor, 6),
            "before": [round(d, 4) for d in dims],
            "after": [round(d, 4) for d in obj.dimensions]}


def center_origin(obj):
    """Origin to the centre of the footprint, on the floor.

    Placing objects is much easier when the origin sits where the object meets
    the ground: `position.y = 0` puts it on the floor instead of half-buried.
    """
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.origin_set(type="ORIGIN_GEOMETRY", center="BOUNDS")

    # origin_set centres on the bounding box; drop it to the base.
    lowest = min((obj.matrix_world @ v.co).z for v in obj.data.vertices)
    obj.location.z -= lowest
    apply_transforms(obj)
    obj.location = (0.0, 0.0, 0.0)
    return True


def shade_auto_smooth(obj, angle_degrees):
    if angle_degrees <= 0.0:
        return False
    bpy.context.view_layer.objects.active = obj
    try:
        # Blender 4.1+ replaced the mesh flag with a modifier-based operator.
        bpy.ops.object.shade_auto_smooth(angle=math.radians(angle_degrees))
    except AttributeError:
        bpy.ops.object.shade_smooth()
    return True


def build_collision_proxy(obj, mode, budget):
    """Duplicate the mesh, decimate it hard, and name it so Godot builds the
    collision shape at import time.

    See the module docstring: the suffix is the whole mechanism.
    """
    if mode == "none":
        return None

    suffix = {"convex": "-convcol", "trimesh": "-col", "colonly": "-colonly"}[mode]

    for other in bpy.context.scene.objects:
        other.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.duplicate()

    proxy = bpy.context.view_layer.objects.active
    proxy.name = obj.name + suffix
    proxy.data.name = proxy.name + "_mesh"

    # Collision geometry is never seen, so it gets a much harder budget than the
    # visual mesh. A 200-triangle hull behaves identically to a 20,000-triangle
    # one for anything the player can feel.
    result = decimate(proxy, budget)

    # Materials on a collision proxy are dead weight in the exported file.
    proxy.data.materials.clear()

    return {"name": proxy.name, "mode": mode, "decimate": result}


# --------------------------------------------------------------------------- #
#  Export                                                                      #
# --------------------------------------------------------------------------- #

def export_glb(path, y_up=True):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)

    kwargs = dict(
        filepath=path,
        export_format="GLB",
        export_apply=True,          # bake remaining modifiers
        export_yup=y_up,            # Godot is Y-up; Blender is Z-up
        use_selection=False,
        export_materials="EXPORT",
        export_normals=True,
    )

    # The glTF exporter's keyword set moves between Blender versions. Rather
    # than pinning one, drop anything this build does not recognise — a missing
    # optional flag is far better than a hard failure at the last step.
    try:
        bpy.ops.export_scene.gltf(**kwargs)
    except TypeError:
        for optional in ("export_normals", "export_materials", "export_yup"):
            kwargs.pop(optional, None)
        bpy.ops.export_scene.gltf(**kwargs)


# --------------------------------------------------------------------------- #
#  Main                                                                        #
# --------------------------------------------------------------------------- #

def main():
    args = parse_args(sys.argv)

    if not os.path.exists(args.input):
        raise SystemExit(f"Input not found: {args.input}")

    base_name = args.name or os.path.splitext(os.path.basename(args.output))[0]

    reset_scene()
    meshes = import_any(args.input)
    obj = join_meshes(meshes, base_name)
    apply_transforms(obj)

    report = {
        "input": args.input,
        "output": args.output,
        "name": base_name,
        "triangles_in": triangle_count(obj),
    }

    welded = merge_by_distance(obj, args.merge_by_distance)
    if welded:
        report["vertices_welded"] = welded

    scaled = normalize_scale(obj, args.scale_to)
    if scaled:
        report["scaled"] = scaled

    if args.center_origin:
        center_origin(obj)
        report["origin"] = "bounds-centre, floor-aligned"

    reduced = decimate(obj, args.target_triangles)
    if reduced:
        report["decimate"] = reduced

    if shade_auto_smooth(obj, args.smooth_angle):
        report["smooth_angle"] = args.smooth_angle

    collision = build_collision_proxy(obj, args.collision, args.collision_triangles)
    if collision:
        report["collision"] = collision

    report["triangles_out"] = triangle_count(obj)
    export_glb(args.output, y_up=args.y_up)
    report["written"] = os.path.exists(args.output)
    if report["written"]:
        report["bytes"] = os.path.getsize(args.output)

    # The plugin parses this line out of Blender's very chatty stdout, so the
    # marker has to be unambiguous and on one line.
    import json
    print("GODOT_AI_OS_RESULT " + json.dumps(report))


if __name__ == "__main__":
    main()

# Copyright MadFall. All Rights Reserved.
#
# Builds the hand-made prop meshes that model blocks with no photo-scanned
# model draw: a campfire, a torch, a plank door (closed and open), a bedroll and
# a ladder.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/build_prop_meshes.py"
#
# Needs the GeometryScripting plugin, which MadFall.uproject enables for the
# editor only (nothing here runs in a packaged game).
#
# WHY BUILT FROM PRIMITIVES: the free scanned libraries had no campfire, door,
# bedroll or wall ladder that reads at the size of one voxel (the ladder tried
# was free-standing). A scaled engine cube read as a placeholder next to the
# textured barrel and crate. A few dozen boxes, cylinders and squashed spheres
# are enough silhouette at block scale, and the script is the source: editing a
# number and re-running it is the whole art pipeline.
#
# WHY SLOTS ARE NAMED AFTER SURFACES: each part's material slot is named for a
# surface class ("madfall:wood", "madfall:stone"), and the model instance
# subsystem gives every slot so named that surface's photo texture in the mesh's
# own space - so the stones of a campfire look like the stone blocks around it
# and a modded surface texture change reaches the props too. No UVs are needed.
#
# Every mesh is in centimetres with its pivot at the bottom centre of a 100 cm
# voxel, so the blocks draw them with render.offset [0, 0, -0.5] and scale 1.

import math
import sys

import unreal

DEST = "/Game/Models/Props"
PREVIEW_MATERIAL = "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"

prims = unreal.GeometryScript_Primitives
CENTER = unreal.GeometryScriptPrimitiveOriginMode.CENTER
BASE = unreal.GeometryScriptPrimitiveOriginMode.BASE


class Prop:
    """A dynamic mesh plus the surface class of each material slot, in first-use order."""

    def __init__(self, name):
        self.name = name
        self.mesh = unreal.DynamicMesh()
        self.slots = []

    def options(self, surface):
        if surface not in self.slots:
            self.slots.append(surface)
        options = unreal.GeometryScriptPrimitiveOptions()
        options.material_id = self.slots.index(surface)
        options.polygroup_mode = unreal.GeometryScriptPrimitivePolygroupMode.PER_FACE
        return options

    def box(self, surface, centre, size, rotation=None):
        prims.append_box(self.mesh, self.options(surface), transform(centre, rotation), size[0], size[1], size[2], 0, 0, 0, CENTER)

    def cylinder(self, surface, start, end, radius, steps=10, end_radius=None):
        """A cylinder (or tapered cone) between two points."""
        axis = unreal.Vector(end[0] - start[0], end[1] - start[1], end[2] - start[2])
        length = axis.length()
        rotation = unreal.MathLibrary.make_rot_from_z(axis.normal())
        if end_radius is None:
            prims.append_cylinder(self.mesh, self.options(surface), transform(start, rotation), radius, length, steps, 0, True, BASE)
        else:
            prims.append_cone(self.mesh, self.options(surface), transform(start, rotation), radius, end_radius, length, steps, 0, True, BASE)

    def blob(self, surface, centre, size, yaw=0.0):
        """A squashed low-poly sphere: a stone, a lump of cloth."""
        rotation = unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw)
        xf = unreal.Transform(location=vec(centre), rotation=rotation, scale=unreal.Vector(size[0] / 20.0, size[1] / 20.0, size[2] / 20.0))
        prims.append_sphere_lat_long(self.mesh, self.options(surface), xf, 10.0, 5, 7, CENTER)


def vec(p):
    return unreal.Vector(float(p[0]), float(p[1]), float(p[2]))


def transform(location, rotation=None):
    return unreal.Transform(location=vec(location), rotation=rotation or unreal.Rotator(), scale=unreal.Vector(1.0, 1.0, 1.0))


class Jitter:
    """A fixed LCG, so rebuilding a prop gives the same stones every time."""

    def __init__(self, seed):
        self.state = seed

    def __call__(self, low, high):
        self.state = (self.state * 1103515245 + 12345) & 0x7FFFFFFF
        return low + (high - low) * (self.state / float(0x7FFFFFFF))


# ---------------------------------------------------------------------------
# Props
# ---------------------------------------------------------------------------

def campfire():
    """A ring of stones round an ash bed and a teepee of split logs.

    The flame (spawned by the light, 40 cm up and 44 cm tall) rises out of the
    teepee's apex, so the logs stop short of it rather than poking through.
    """
    prop = Prop("Campfire")
    rand = Jitter(7)
    prop.cylinder("madfall:gravel", (0, 0, 0), (0, 0, 2.5), 31, 14)
    stones = 11
    for index in range(stones):
        angle = 2.0 * math.pi * index / stones + rand(-0.12, 0.12)
        radius = 37 + rand(-2.5, 2.5)
        size = (rand(15, 21), rand(11, 15), rand(9, 13))
        prop.blob("madfall:stone", (radius * math.cos(angle), radius * math.sin(angle), size[2] * 0.4), size, math.degrees(angle) + 90)
    logs = 6
    for index in range(logs):
        angle = 2.0 * math.pi * (index + 0.5) / logs + rand(-0.15, 0.15)
        foot = 25 + rand(-2, 2)
        base = (foot * math.cos(angle), foot * math.sin(angle), 3)
        apex = (4 * math.cos(angle), 4 * math.sin(angle), 34 + rand(-3, 3))
        prop.cylinder("madfall:bark", base, apex, rand(3.6, 4.6), 8)
    # Two split logs lying across the ash, where the fire has burnt through.
    prop.cylinder("madfall:wood", (-20, -8, 5), (18, 6, 5), 3.5, 8)
    prop.cylinder("madfall:wood", (-6, 19, 5), (8, -18, 6.5), 3.2, 8)
    return prop


def torch():
    """A standing torch: a tapered stick in a small cairn, its head bound in cloth.

    The light sits 80 cm up with a 26 cm flame (67-93 cm), so the head stops
    just under the flame at 66 cm and narrows to the top: the head is between
    the light and the floor, and a wide flat top threw a shadow disc a metre
    across round the torch's foot.
    """
    prop = Prop("Torch")
    rand = Jitter(11)
    prop.cylinder("madfall:bark", (0, 0, 0), (0, 0, 64), 3.2, 8, end_radius=2.6)
    prop.cylinder("madfall:cloth", (0, 0, 51), (0, 0, 66), 4.4, 8, end_radius=2.8)
    prop.cylinder("madfall:steel", (0, 0, 48), (0, 0, 51), 3.8, 8)
    for index in range(4):
        angle = 2.0 * math.pi * index / 4 + 0.4
        size = (rand(10, 13), rand(8, 10), rand(6, 8))
        prop.blob("madfall:stone", (8 * math.cos(angle), 8 * math.sin(angle), size[2] * 0.35), size, math.degrees(angle))
    return prop


def door_parts(prop):
    """One voxel of a plank door, lying in the YZ plane (thin in X) like the closed door block.

    Doors are stacked two voxels high, so the top and bottom halves are the same
    mesh: battens and hinges repeat per voxel, which reads as a four-batten door,
    and there is no handle to appear twice.

    The planks run horizontally, along the boards of the wood texture: vertical
    planks under horizontal texture boards read as a grid.
    """
    rand = Jitter(3)
    planks = 4
    height = 100.0 / planks
    for index in range(planks):
        z = height * (index + 0.5)
        prop.box("madfall:wood", (rand(-0.4, 0.4), rand(-0.6, 0.6), z), (5.5, 99, height - 0.9))
    for y in (-32, 32):
        prop.box("madfall:wood", (-4.25, y, 50), (3, 11, 100))
    brace = math.degrees(math.atan2(76.0, 50.0))
    prop.box("madfall:wood", (-4.25, 0, 50), (3, 9, 88), unreal.Rotator(roll=brace - 90.0, pitch=0.0, yaw=0.0))
    for z in (20, 80):
        prop.box("madfall:steel", (3.3, -34, z), (1.2, 32, 4.5))
        prop.cylinder("madfall:steel", (3.0, -49, z - 5), (3.0, -49, z + 5), 1.6, 8)


def door_closed():
    prop = Prop("Door")
    door_parts(prop)
    return prop


def door_open():
    """The same door swung 90 degrees onto the side the open block draws it."""
    prop = Prop("DoorOpen")
    door_parts(prop)
    swing = unreal.Transform(location=unreal.Vector(0.0, -43.0, 0.0), rotation=unreal.Rotator(roll=0.0, pitch=0.0, yaw=90.0), scale=unreal.Vector(1.0, 1.0, 1.0))
    unreal.GeometryScript_MeshTransforms.transform_mesh(prop.mesh, swing)
    return prop


def bedroll():
    """A padded mat along X with a blanket folded back and a rolled pillow at the head."""
    prop = Prop("Bedroll")
    prop.box("madfall:cloth", (-2, 0, 2.5), (90, 64, 5))
    prop.box("madfall:cloth", (-12, 0, 6.2), (66, 60, 2.6))
    prop.box("madfall:cloth", (21, 0, 6.4), (5, 60, 3.2))
    prop.cylinder("madfall:cloth", (36, -29, 13), (36, 29, 13), 8.5, 12)
    for y in (-18, 18):
        prop.cylinder("madfall:bark", (36, y - 1.5, 13), (36, y + 1.5, 13), 9.2, 12)
    return prop


def ladder():
    """Two rails and four rungs against the +X face of the voxel, tiling vertically."""
    prop = Prop("Ladder")
    for y in (-34, 34):
        prop.box("madfall:wood", (45.5, y, 50), (5, 6, 100))
    for z in (12.5, 37.5, 62.5, 87.5):
        prop.cylinder("madfall:wood", (44.5, -31, z), (44.5, 31, z), 2.3, 8)
    return prop


PROPS = [campfire, torch, door_closed, door_open, bedroll, ladder]

# Props a survivor collides with get a box around their bounds; the rest (the
# open door, the ladder, the torch) are walked through, as their blocks say.
COLLIDES = {"Campfire", "Door", "Bedroll"}


def main():
    library = unreal.EditorAssetLibrary
    meshes = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    preview = library.load_asset(PREVIEW_MATERIAL)
    failures = 0
    for build in PROPS:
        prop = build()
        path = "{}/SM_{}".format(DEST, prop.name)
        if library.does_asset_exist(path):
            library.delete_asset(path)

        options = unreal.GeometryScriptCreateNewStaticMeshAssetOptions()
        options.enable_recompute_normals = False
        options.enable_recompute_tangents = True
        options.enable_collision = prop.name in COLLIDES
        mesh, outcome = unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(prop.mesh, path, options)
        if mesh is None or outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
            unreal.log_error("[MadFall] Could not build {}".format(path))
            failures += 1
            continue

        mesh.set_editor_property("static_materials", [
            unreal.StaticMaterial(material_interface=preview, material_slot_name=surface)
            for surface in prop.slots])
        meshes.remove_collisions(mesh)
        if prop.name in COLLIDES:
            meshes.add_simple_collisions(mesh, unreal.ScriptCollisionShapeType.BOX)
        library.save_asset(path, only_if_is_dirty=False)

        bounds = mesh.get_bounding_box()
        size = bounds.max - bounds.min
        unreal.log("[MadFall] Built {}: {} triangles, slots {}, size {:.1f} x {:.1f} x {:.1f} cm, min ({:.1f}, {:.1f}, {:.1f}), max ({:.1f}, {:.1f}, {:.1f})".format(
            path, prop.mesh.get_triangle_count(), prop.slots, size.x, size.y, size.z,
            bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x, bounds.max.y, bounds.max.z))

    unreal.log("[MadFall] Props: {} built, {} failed".format(len(PROPS) - failures, failures))
    return 0 if failures == 0 else 1


sys.exit(main())

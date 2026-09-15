# Copyright MadFall. All Rights Reserved.
#
# The photo-scanned surface texture sets and how each is used, shared by
# import_surface_textures.py (textures, instances, arrays) and
# make_pbr_material.py (the array materials' per-layer constants).

# Set -> (tile size in voxels, tint toward the surface colour 0..1, side set or None, metallic)
SETS = {
    "Rock030": (3.0, 0.15, None, 0.0),
    "Ground048": (2.0, 0.2, None, 0.0),
    "Grass004": (2.0, 0.25, "Ground048", 0.0),
    "Ground080": (3.0, 0.1, None, 0.0),
    "Planks021": (1.0, 0.35, None, 0.0),
    "Bark012": (1.0, 0.15, None, 0.0),
    "Concrete034": (2.0, 0.1, None, 0.0),
    "Bricks076C": (1.0, 0.1, None, 0.0),
    "Metal041B": (1.0, 0.1, None, 0.85),
    "Gravel022": (2.0, 0.1, None, 0.0),
    "Snow006": (3.0, 0.0, None, 0.0),
    "Fabric066": (1.0, 0.3, None, 0.0),
}

# The sets packed into the surface texture arrays, in layer order. A surface's
# "texture_layer" is an index into this list, so it is append-only.
#
# WHY ARRAYS: a chunk's mesh becomes one component section per material, and
# each section is a mesh creation, a collision update and a draw call. With a
# material instance per set, a forest chunk went from one or two sections to
# four to six and meshing's frame time rose by half. One array material draws
# every layered surface in one section; the surface picks its layer through
# vertex colour alpha.
#
# Not layered: Concrete034 is 2048x1024 and an array's slices must match (it
# keeps its own instance - concrete is mostly buildings, not terrain), and
# Bricks076C and Snow006 have no surface using them yet.
ARRAY_LAYERS = [
    "Rock030",
    "Ground048",
    "Grass004",
    "Ground080",
    "Planks021",
    "Bark012",
    "Metal041B",
    "Gravel022",
    "Fabric066",
]

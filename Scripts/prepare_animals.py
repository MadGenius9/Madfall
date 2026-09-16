# Copyright MadFall. All Rights Reserved.
#
# Normalises the downloaded animal glTFs so Unreal imports a sound skeleton.
#
#   1. Scripts/fetch_animals.ps1 downloads SourceArt/animals/<Name>.glb
#   2. python Scripts/prepare_animals.py   -> SourceArt/animals/prepared/<Name>.glb
#   3. Scripts/import_animals.py imports the prepared files
#
# WHY: the pack was exported from Blender with a 100x scale and a -90 degree
# turn about X (Blender's Z-up to glTF's Y-up) on both the armature and the mesh
# node, and tiny bone translations under them. Unreal's glTF importer carries
# neither through a skinned mesh ("parent transforms will not affect a skinned
# mesh"): with the scale, the imported deer's head and tail bones sat on the
# same side of its body and its bone bounds were kilometres wide; with the scale
# fixed, the turn stood it on its tail. Baking both into the data they transform
# leaves identity nodes the importer cannot get wrong:
#
#   - the armature's and the skinned mesh node's scale become 1 (rotations kept);
#   - every joint's translation, and every translation key animating a joint, is
#     multiplied by the scale (a uniform scale on a parent is exactly a scale on
#     its descendants' translations);
#   - vertex positions are multiplied by it;
#   - each inverse bind matrix M becomes S M S^-1: its rotation is unchanged and
#     its translation is multiplied by the scale;
#   - then the turn R: the armature's direct children (the root bone and the IK
#     targets) get R applied to their translation and rotation and to their keys;
#     positions, normals and tangents are rotated; and each inverse bind matrix
#     M becomes M R^-1, so a skinned vertex R v lands where v did.
#
# The model then has its authored proportions at 100x the tiny source units;
# the game fits every model to its definition's size anyway.

import json
import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SourceArt", "animals")
OUT = os.path.join(ROOT, "prepared")
ANIMALS = ["Deer", "Wolf", "Fox", "Stag"]

COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path):
    data = open(path, "rb").read()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and version == 2, "not a glTF 2 binary"
    offset = 12
    document, binary = None, None
    while offset < len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8: offset + 8 + length]
        if kind == 0x4E4F534A:
            document = json.loads(chunk)
        elif kind == 0x004E4942:
            binary = bytearray(chunk)
        offset += 8 + length
    return document, binary


def write_glb(path, document, binary):
    binary = bytes(binary) + b"\0" * ((4 - len(binary) % 4) % 4)
    document["buffers"][0]["byteLength"] = len(binary)  # views may have been appended
    text = json.dumps(document, separators=(",", ":")).encode("utf-8")
    text += b" " * ((4 - len(text) % 4) % 4)
    total = 12 + 8 + len(text) + 8 + len(binary)
    with open(path, "wb") as out:
        out.write(struct.pack("<III", 0x46546C67, 2, total))
        out.write(struct.pack("<II", len(text), 0x4E4F534A))
        out.write(text)
        out.write(struct.pack("<II", len(binary), 0x004E4942))
        out.write(binary)


def float_elements(document, accessor_index):
    """(byte offset, component count) of every element of a float accessor."""
    accessor = document["accessors"][accessor_index]
    assert accessor["componentType"] == 5126, "expected a float accessor"
    view = document["bufferViews"][accessor["bufferView"]]
    count = COMPONENTS[accessor["type"]]
    stride = view.get("byteStride", 4 * count)
    base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    return [(base + i * stride, count) for i in range(accessor["count"])]


def scale_accessor(document, binary, accessor_index, factor, components=None):
    for offset, count in float_elements(document, accessor_index):
        for c in (components if components is not None else range(count)):
            value = struct.unpack_from("<f", binary, offset + 4 * c)[0]
            struct.pack_into("<f", binary, offset + 4 * c, value * factor)
    accessor = document["accessors"][accessor_index]
    for bound in ("min", "max"):
        if bound in accessor:
            accessor[bound] = [v * factor if components is None or i in components else v for i, v in enumerate(accessor[bound])]


def quat_multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def quat_matrix(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def rotate(matrix, v):
    return [sum(matrix[r][c] * v[c] for c in range(3)) for r in range(3)]


def transform_accessor(document, binary, accessor_index, function):
    """Replaces every element of a float accessor with function(element)."""
    for offset, count in float_elements(document, accessor_index):
        values = list(struct.unpack_from("<{}f".format(count), binary, offset))
        struct.pack_into("<{}f".format(count), binary, offset, *function(values))


def descendants(document, index):
    found = []
    for child in document["nodes"][index].get("children", []):
        found.append(child)
        found += descendants(document, child)
    return found


def matrix_multiply(a, b):
    """Row-major 4x4 multiply (both affine: the last row is 0 0 0 1)."""
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def node_matrix(node):
    t = node.get("translation", [0.0, 0.0, 0.0])
    s = node.get("scale", [1.0, 1.0, 1.0])
    m = quat_matrix(node.get("rotation", [0.0, 0.0, 0.0, 1.0]))
    return [[m[r][c] * s[c] for c in range(3)] + [t[r]] for r in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def affine_inverse(m):
    """Inverse of an affine 4x4 by cofactors on the linear part."""
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    assert abs(det) > 1e-12, "a degenerate transform cannot be inverted"
    inverse = [[((a[(c + 1) % 3][(r + 1) % 3] * a[(c + 2) % 3][(r + 2) % 3]
                  - a[(c + 1) % 3][(r + 2) % 3] * a[(c + 2) % 3][(r + 1) % 3]) / det) for c in range(3)]
               for r in range(3)]
    translation = [-sum(inverse[r][c] * m[c][3] for c in range(3)) for r in range(3)]
    return [inverse[r] + [translation[r]] for r in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def apply(m, v, w):
    return [sum(m[r][c] * v[c] for c in range(3)) + m[r][3] * w for r in range(3)]


def add_view(document, binary, data, target=None):
    """Appends a tightly packed buffer view and returns its index."""
    while len(binary) % 4:
        binary += b"\0"
    view = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)}
    if target is not None:
        view["target"] = target
    binary += data
    document["bufferViews"].append(view)
    return len(document["bufferViews"]) - 1


def add_accessor(document, binary, data, component_type, kind, count, bounds=None):
    accessor = {"bufferView": add_view(document, binary, data, target=34962),
                "componentType": component_type, "count": count, "type": kind}
    if bounds is not None:
        accessor["min"], accessor["max"] = bounds
    document["accessors"].append(accessor)
    return len(document["accessors"]) - 1


def bind_rigid_children(document, binary, skin):
    """Skins meshes that just hang off a bone, so the glTF has one skinned mesh.

    WHY: the stag's antlers are a separate unskinned mesh parented to its Head
    node. Unreal imports such a node as its own *static* mesh, so the stag's
    skeletal mesh came in bald. A vertex v of that child lands at
    head * L * v, where L is the chain of local transforms from the bone down to
    it; a vertex p weighted entirely to that bone lands at head * IBM * p. So
    p = IBM^-1 * L * v gives the same animated position from inside the skin,
    and the horns become a primitive of the body mesh that follows the head.
    """
    nodes = document["nodes"]
    slots = {joint: slot for slot, joint in enumerate(skin["joints"])}
    inverse_binds = {}
    for slot, (offset, count) in enumerate(float_elements(document, skin["inverseBindMatrices"])):
        columns = struct.unpack_from("<16f", binary, offset)  # glTF matrices are column-major.
        inverse_binds[slot] = [[columns[4 * c + r] for c in range(4)] for r in range(4)]

    parent = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parent[child] = index

    skinned = next(n for n in nodes if n.get("skin") == 0 and "mesh" in n)
    bound = []
    for index, node in enumerate(list(nodes)):
        if "mesh" not in node or "skin" in node or index not in parent:
            continue
        local = node_matrix(node)
        walk = parent[index]
        while walk not in slots and walk in parent:
            local = matrix_multiply(node_matrix(nodes[walk]), local)
            walk = parent[walk]
        if walk not in slots:
            continue
        transform = matrix_multiply(affine_inverse(inverse_binds[slots[walk]]), local)

        for primitive in document["meshes"][node["mesh"]]["primitives"]:
            attributes = primitive["attributes"]
            count = document["accessors"][attributes["POSITION"]]["count"]
            moved = {}
            for attribute, w in (("POSITION", 1.0), ("NORMAL", 0.0)):
                if attribute not in attributes:
                    continue
                values = [apply(transform, struct.unpack_from("<3f", binary, offset), w)
                          for offset, _ in float_elements(document, attributes[attribute])]
                bounds = ([min(v[i] for v in values) for i in range(3)],
                          [max(v[i] for v in values) for i in range(3)]) if attribute == "POSITION" else None
                moved[attribute] = add_accessor(document, binary, b"".join(struct.pack("<3f", *v) for v in values),
                                                5126, "VEC3", count, bounds)
            moved["JOINTS_0"] = add_accessor(document, binary, struct.pack("<4H", slots[walk], 0, 0, 0) * count,
                                             5123, "VEC4", count)
            moved["WEIGHTS_0"] = add_accessor(document, binary, struct.pack("<4f", 1.0, 0.0, 0.0, 0.0) * count,
                                              5126, "VEC4", count)
            merged = {"attributes": moved, "indices": primitive["indices"]}
            if "material" in primitive:
                merged["material"] = primitive["material"]
            document["meshes"][skinned["mesh"]]["primitives"].append(merged)

        # Drop the node from the hierarchy and the mesh from the file. Orphaning
        # the node is not enough: Unreal's importer walks the mesh list, so a
        # left-behind mesh still arrives as a stray static mesh asset.
        nodes[parent[index]]["children"] = [c for c in nodes[parent[index]]["children"] if c != index]
        bound.append((node.get("name"), nodes[walk].get("name"), node.pop("mesh")))

    for _, _, mesh in sorted(bound, key=lambda entry: -entry[2]):
        del document["meshes"][mesh]
        for node in nodes:
            if node.get("mesh", -1) > mesh:
                node["mesh"] -= 1
    return [(child, bone) for child, bone, _ in bound]


def prepare(name):
    document, binary = read_glb(os.path.join(ROOT, name + ".glb"))
    nodes = document["nodes"]
    skin = document["skins"][0]
    joints = set(skin["joints"])

    # Before anything is scaled or turned: fold rigid child meshes into the skin.
    bound = bind_rigid_children(document, binary, skin)

    # The armature: the scaled node whose descendants include the joints.
    armature = next(i for i, n in enumerate(nodes)
                    if n.get("scale", [1, 1, 1]) != [1, 1, 1] and joints & set(descendants(document, i)))
    factor = nodes[armature]["scale"][0]
    assert nodes[armature]["scale"] == [factor] * 3, "only a uniform armature scale is handled"
    scaled = set(descendants(document, armature))

    nodes[armature]["scale"] = [1.0, 1.0, 1.0]
    for index in scaled:
        if "translation" in nodes[index]:
            nodes[index]["translation"] = [v * factor for v in nodes[index]["translation"]]

    positions = set()
    for index, node in enumerate(nodes):
        if "skin" in node and "mesh" in node:
            assert node.get("scale", [factor] * 3) == [factor] * 3, "the mesh node's scale should match the armature's"
            node["scale"] = [1.0, 1.0, 1.0]
            for primitive in document["meshes"][node["mesh"]]["primitives"]:
                positions.add(primitive["attributes"]["POSITION"])
    for accessor in positions:
        scale_accessor(document, binary, accessor, factor)

    scale_accessor(document, binary, skin["inverseBindMatrices"], factor, components={12, 13, 14})

    keys = set()
    for animation in document.get("animations", []):
        for channel in animation["channels"]:
            target = channel["target"]
            if target.get("path") == "translation" and target.get("node") in scaled:
                keys.add(animation["samplers"][channel["sampler"]]["output"])
    for accessor in keys:
        scale_accessor(document, binary, accessor, factor)

    # --- the rotation ---------------------------------------------------------------
    quat = nodes[armature].get("rotation", [0.0, 0.0, 0.0, 1.0])
    matrix = quat_matrix(quat)
    inverse = [[matrix[c][r] for c in range(3)] for r in range(3)]
    nodes[armature]["rotation"] = [0.0, 0.0, 0.0, 1.0]
    roots = set(nodes[armature].get("children", []))
    for index in roots:
        node = nodes[index]
        node["translation"] = rotate(matrix, node.get("translation", [0.0, 0.0, 0.0]))
        node["rotation"] = quat_multiply(quat, node.get("rotation", [0.0, 0.0, 0.0, 1.0]))

    vectors, tangents = set(), set()
    for index, node in enumerate(nodes):
        if "skin" in node and "mesh" in node:
            assert all(abs(a - b) < 1e-5 for a, b in zip(node.get("rotation", quat), quat)), "the mesh node's rotation should match the armature's"
            node["rotation"] = [0.0, 0.0, 0.0, 1.0]
            for primitive in document["meshes"][node["mesh"]]["primitives"]:
                attributes = primitive["attributes"]
                vectors.update(attributes[a] for a in ("POSITION", "NORMAL") if a in attributes)
                if "TANGENT" in attributes:
                    tangents.add(attributes["TANGENT"])
    for accessor in vectors:
        transform_accessor(document, binary, accessor, lambda v: rotate(matrix, v))
    for accessor in tangents:
        transform_accessor(document, binary, accessor, lambda v: rotate(matrix, v[:3]) + [v[3]])
    # The spec requires position bounds; recompute them from the rotated data.
    for accessor in positions:
        elements = [struct.unpack_from("<3f", binary, offset) for offset, _ in float_elements(document, accessor)]
        document["accessors"][accessor]["min"] = [min(e[i] for e in elements) for i in range(3)]
        document["accessors"][accessor]["max"] = [max(e[i] for e in elements) for i in range(3)]

    def times_inverse_rotation(m):
        # Column-major 4x4: columns 0-2 are the linear part, column 3 the translation.
        columns = [m[0:4], m[4:8], m[8:12], m[12:16]]
        result = []
        for c in range(3):
            result += [sum(columns[k][r] * inverse[k][c] for k in range(3)) for r in range(4)]
        return result + columns[3]
    transform_accessor(document, binary, skin["inverseBindMatrices"], times_inverse_rotation)

    root_translations, root_rotations = set(), set()
    for animation in document.get("animations", []):
        for channel in animation["channels"]:
            target = channel["target"]
            if target.get("node") in roots:
                output = animation["samplers"][channel["sampler"]]["output"]
                if target.get("path") == "translation":
                    root_translations.add(output)
                elif target.get("path") == "rotation":
                    root_rotations.add(output)
    for accessor in root_translations:
        transform_accessor(document, binary, accessor, lambda v: rotate(matrix, v))
    for accessor in root_rotations:
        transform_accessor(document, binary, accessor, lambda q: quat_multiply(quat, q))

    os.makedirs(OUT, exist_ok=True)
    write_glb(os.path.join(OUT, name + ".glb"), document, binary)
    print("{}: baked a {}x scale and a {} turn into {} joints ({} roots), {} vertex and {} key accessors{}".format(
        name, factor, [round(v, 3) for v in quat], len(scaled), len(roots), len(vectors) + len(tangents),
        len(keys | root_translations | root_rotations),
        "".join("; skinned {} to {}".format(child, bone) for child, bone in bound)))


def main():
    for name in ANIMALS:
        prepare(name)
    return 0


if __name__ == "__main__":
    sys.exit(main())

# Copyright MadFall. All Rights Reserved.
#
# Imports the photo-scanned surface textures (ambientCG, CC0) and builds one
# material instance of M_MadVoxelPBR per texture set.
#
#   1. Unzip each <Asset>_2K-JPG.zip into SourceArt/ambientCG/<Asset>/
#   2. UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/import_surface_textures.py"
#
# Run make_pbr_material.py first (it builds the parent material).
#
# WHAT GOES WHERE
# ambientCG ships colour, a DirectX normal map, roughness and more per set.
# Colour imports as sRGB; the normal as a normal map; roughness as linear
# masks. Everything lands under /Game/Surfaces/<Set>/, and the instance
# /Game/Surfaces/MI_<Set> is what a surface's "material" names. Re-running
# re-imports changed files and leaves the rest.
#
# The textures are CC0 (public domain): no attribution required, fine to ship.
#
# It also packs the sets in surface_sets.ARRAY_LAYERS into three texture arrays
# (TA_SurfaceBaseColor, TA_SurfaceNormal, TA_SurfaceRoughness) for
# M_MadVoxelPBRArray; run make_pbr_material.py again afterwards to build that.

import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from surface_sets import ARRAY_LAYERS, SETS  # noqa: E402

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SOURCE = os.path.join(PROJECT, "SourceArt", "ambientCG")
DEST = "/Game/Surfaces"
PARENT = "/Game/Materials/M_MadVoxelPBR.M_MadVoxelPBR"

MAPS = {
    "Color": ("BaseColor", True, unreal.TextureCompressionSettings.TC_DEFAULT),
    "NormalDX": ("Normal", False, unreal.TextureCompressionSettings.TC_NORMALMAP),
    "Roughness": ("Roughness", False, unreal.TextureCompressionSettings.TC_MASKS),
}


def find_file(folder, suffix):
    for name in os.listdir(folder):
        stem, ext = os.path.splitext(name)
        if ext.lower() in (".jpg", ".png") and stem.endswith("_" + suffix):
            return os.path.join(folder, name)
    return None


def import_texture(path, destination, name, srgb, compression):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", destination)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset_path = "{}/{}".format(destination, name)
    texture = unreal.EditorAssetLibrary.load_asset(asset_path)
    if texture is None:
        unreal.log_error("[MadFall] Could not import {}".format(path))
        return None
    texture.set_editor_property("srgb", srgb)
    texture.set_editor_property("compression_settings", compression)
    if compression == unreal.TextureCompressionSettings.TC_NORMALMAP:
        texture.set_editor_property("flip_green_channel", False)   # DirectX normals, as Unreal expects
    unreal.EditorAssetLibrary.save_asset(asset_path, only_if_is_dirty=False)
    return texture


def import_set(set_name):
    folder = os.path.join(SOURCE, set_name)
    if not os.path.isdir(folder):
        unreal.log_warning("[MadFall] {} is not unzipped at {}".format(set_name, folder))
        return None
    textures = {}
    for suffix, (slot, srgb, compression) in MAPS.items():
        path = find_file(folder, suffix)
        if path is None:
            unreal.log_warning("[MadFall] {} has no {} map".format(set_name, suffix))
            continue
        textures[slot] = import_texture(path, "{}/{}".format(DEST, set_name), "T_{}_{}".format(set_name, slot), srgb, compression)
    return textures


def main():
    editing = unreal.MaterialEditingLibrary
    library = unreal.EditorAssetLibrary
    parent = library.load_asset(PARENT)
    if parent is None:
        unreal.log_error("[MadFall] Run make_pbr_material.py first: {} is missing".format(PARENT))
        return 1

    imported = {}
    for set_name in SETS:
        textures = import_set(set_name)
        if textures:
            imported[set_name] = textures

    failures = 0
    for set_name, (tile, tint, side, metallic) in SETS.items():
        if set_name not in imported:
            failures += 1
            continue
        instance_name = "MI_{}".format(set_name)
        instance_path = "{}/{}".format(DEST, instance_name)
        if library.does_asset_exist(instance_path):
            instance = library.load_asset(instance_path)
        else:
            tools = unreal.AssetToolsHelpers.get_asset_tools()
            instance = tools.create_asset(instance_name, DEST, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        editing.set_material_instance_parent(instance, parent)
        textures = imported[set_name]
        for slot in ("BaseColor", "Normal", "Roughness"):
            if textures.get(slot) is not None:
                editing.set_material_instance_texture_parameter_value(instance, slot, textures[slot])
        editing.set_material_instance_scalar_parameter_value(instance, "TileVoxels", tile)
        editing.set_material_instance_scalar_parameter_value(instance, "Tint", tint)
        editing.set_material_instance_scalar_parameter_value(instance, "Metallic", metallic)
        if side and side in imported:
            for slot in ("BaseColor", "Normal", "Roughness"):
                if imported[side].get(slot) is not None:
                    editing.set_material_instance_texture_parameter_value(instance, "Side" + slot, imported[side][slot])
            editing.set_material_instance_scalar_parameter_value(instance, "UseSides", 1.0)
        editing.update_material_instance(instance)
        library.save_asset(instance_path, only_if_is_dirty=False)
        unreal.log("[MadFall] Built {}".format(instance_path))

    unreal.log("[MadFall] Surface textures: {} sets built, {} missing".format(len(SETS) - failures, failures))
    failures += build_arrays(imported)
    return 0 if failures == 0 else 1


def build_arrays(imported):
    library = unreal.EditorAssetLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    failures = 0
    for slot, srgb, compression in (("BaseColor", True, unreal.TextureCompressionSettings.TC_DEFAULT),
                                    ("Normal", False, unreal.TextureCompressionSettings.TC_NORMALMAP),
                                    ("Roughness", False, unreal.TextureCompressionSettings.TC_MASKS)):
        missing = [name for name in ARRAY_LAYERS if name not in imported or imported[name].get(slot) is None]
        if missing:
            unreal.log_error("[MadFall] Cannot build the {} array: {} not imported".format(slot, ", ".join(missing)))
            failures += 1
            continue
        name = "TA_Surface{}".format(slot)
        path = "{}/{}".format(DEST, name)
        array = library.load_asset(path) if library.does_asset_exist(path) else \
            tools.create_asset(name, DEST, unreal.Texture2DArray, unreal.Texture2DArrayFactory())
        array.set_editor_property("srgb", srgb)
        array.set_editor_property("compression_settings", compression)
        # Setting the source list rebuilds the array's source from the slices,
        # which must all be the same size and format.
        array.set_editor_property("source_textures", [imported[layer][slot] for layer in ARRAY_LAYERS])
        library.save_asset(path, only_if_is_dirty=False)
        unreal.log("[MadFall] Built {} with {} layers".format(path, len(ARRAY_LAYERS)))
    return failures


sys.exit(main())

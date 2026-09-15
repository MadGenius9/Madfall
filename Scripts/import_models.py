# Copyright MadFall. All Rights Reserved.
#
# Imports the photo-scanned models (Poly Haven, CC0 - public domain) that model
# blocks draw, and builds a material instance for each.
#
#   1. Scripts/fetch_models.ps1 downloads them into SourceArt/polyhaven/<Asset>/
#   2. Scripts/make_model_material.py builds M_MadModel and M_MadModelFoliage
#   3. UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/import_models.py"
#
# Each lands in /Game/Models/<Asset>/: SM_<Asset>, its textures, MI_<Asset>. The
# script logs every mesh's bounds, which is what a block's render.scale and
# render.offset are worked out from (a model block fits a 100 uu voxel).
#
# Poly Haven normal maps are OpenGL convention; Unreal's is DirectX, so the
# green channel is flipped on import. Roughness and metallic arrive as EXR and
# import as linear masks.

import os
import sys

import unreal

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SOURCE = os.path.join(PROJECT, "SourceArt", "polyhaven")
DEST = "/Game/Models"

# Asset -> (foliage, metallic scale)
#
# Tried and dropped: wooden_ladder_02 (a free-standing ladder, not one that lies
# against a wall), fern_02 and shrub_sorrel_01 (at block scale a flat cluster and
# a few blades - the box shapes read better as a bush and a crop). The foliage
# material stays for a future model that fits.
MODELS = {
    "Barrel_01": (False, 1.0),
    "wooden_crate_01": (False, 1.0),
}

# File name fragment -> (material parameter, sRGB, compression, flip green)
MAPS = [
    ("_diff_", "BaseColor", True, unreal.TextureCompressionSettings.TC_DEFAULT, False),
    ("_nor_gl_", "Normal", False, unreal.TextureCompressionSettings.TC_NORMALMAP, True),
    ("_rough_", "Roughness", False, unreal.TextureCompressionSettings.TC_MASKS, False),
    ("_roughness_", "Roughness", False, unreal.TextureCompressionSettings.TC_MASKS, False),
    ("_metal_", "Metallic", False, unreal.TextureCompressionSettings.TC_MASKS, False),
    ("_metallic_", "Metallic", False, unreal.TextureCompressionSettings.TC_MASKS, False),
    ("_alpha_", "Alpha", False, unreal.TextureCompressionSettings.TC_MASKS, False),
]


def run_task(filename, destination, name, options=None):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", filename)
    task.set_editor_property("destination_path", destination)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", False)
    if options is not None:
        task.set_editor_property("options", options)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return unreal.EditorAssetLibrary.load_asset("{}/{}".format(destination, name))


def import_mesh(path, destination, name):
    ui = unreal.FbxImportUI()
    ui.set_editor_property("import_mesh", True)
    ui.set_editor_property("import_as_skeletal", False)
    ui.set_editor_property("import_materials", False)
    ui.set_editor_property("import_textures", False)
    ui.set_editor_property("import_animations", False)
    ui.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_STATIC_MESH)
    data = ui.get_editor_property("static_mesh_import_data")
    data.set_editor_property("combine_meshes", True)
    data.set_editor_property("auto_generate_collision", False)
    data.set_editor_property("generate_lightmap_u_vs", False)
    return run_task(path, destination, name, ui)


def main():
    library = unreal.EditorAssetLibrary
    editing = unreal.MaterialEditingLibrary
    failures = 0
    for asset, (foliage, metallic_scale) in MODELS.items():
        folder = os.path.join(SOURCE, asset)
        fbx = os.path.join(folder, "{}_1k.fbx".format(asset))
        if not os.path.isfile(fbx):
            unreal.log_warning("[MadFall] {} is not downloaded: {}".format(asset, fbx))
            failures += 1
            continue
        destination = "{}/{}".format(DEST, asset)
        mesh = import_mesh(fbx, destination, "SM_{}".format(asset))
        if mesh is None:
            unreal.log_error("[MadFall] Could not import {}".format(fbx))
            failures += 1
            continue
        library.save_asset("{}/SM_{}".format(destination, asset), only_if_is_dirty=False)

        parent = library.load_asset("/Game/Materials/{}".format("M_MadModelFoliage" if foliage else "M_MadModel"))
        instance_path = "{}/MI_{}".format(destination, asset)
        if library.does_asset_exist(instance_path):
            instance = library.load_asset(instance_path)
        else:
            instance = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                "MI_{}".format(asset), destination, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        editing.set_material_instance_parent(instance, parent)
        editing.set_material_instance_scalar_parameter_value(instance, "MetallicScale", metallic_scale)

        textures = os.path.join(folder, "textures")
        for file_name in sorted(os.listdir(textures)):
            for fragment, parameter, srgb, compression, flip in MAPS:
                if fragment in file_name:
                    texture_name = "T_{}_{}".format(asset, parameter)
                    texture = run_task(os.path.join(textures, file_name), destination, texture_name)
                    if texture is None:
                        unreal.log_error("[MadFall] Could not import {}".format(file_name))
                        failures += 1
                        break
                    texture.set_editor_property("srgb", srgb)
                    texture.set_editor_property("compression_settings", compression)
                    texture.set_editor_property("flip_green_channel", flip)
                    library.save_asset("{}/{}".format(destination, texture_name), only_if_is_dirty=False)
                    editing.set_material_instance_texture_parameter_value(instance, parameter, texture)
                    break
        editing.update_material_instance(instance)
        library.save_asset(instance_path, only_if_is_dirty=False)

        bounds = mesh.get_bounding_box()
        size = bounds.max - bounds.min
        unreal.log("[MadFall] Built {}: size {:.1f} x {:.1f} x {:.1f} cm, min ({:.1f}, {:.1f}, {:.1f}), max ({:.1f}, {:.1f}, {:.1f})".format(
            destination, size.x, size.y, size.z, bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x, bounds.max.y, bounds.max.z))

    unreal.log("[MadFall] Models: {} imported, {} failed".format(len(MODELS) - failures, failures))
    return 0 if failures == 0 else 1


sys.exit(main())

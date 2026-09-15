# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadLightning - the glowing channel of a lightning
# bolt drawn by UMadWeatherSubsystem during storms: unlit, additive, blue-white
# and very bright so bloom haloes it against a storm sky.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_lightning_material.py"

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadLightning"
VERSION_TAG = "MadFallLightningVersion"
MATERIAL_VERSION = "1"


def main():
    editing = unreal.MaterialEditingLibrary
    library = unreal.EditorAssetLibrary
    full_path = "{}/{}".format(PACKAGE_PATH, ASSET_NAME)
    if library.does_asset_exist(full_path):
        material = library.load_asset(full_path)
        if library.get_metadata_tag(material, VERSION_TAG) == MATERIAL_VERSION:
            unreal.log("[MadFall] {} is already version {}.".format(full_path, MATERIAL_VERSION))
            return 0
        editing.delete_all_material_expressions(material)
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        material = tools.create_asset(ASSET_NAME, PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew())
        if material is None:
            unreal.log_error("[MadFall] Could not create {}".format(full_path))
            return 1

    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("used_with_instanced_static_meshes", True)

    glow = editing.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -400, 0)
    glow.set_editor_property("constant", unreal.LinearColor(40.0, 44.0, 60.0, 0.0))
    if not editing.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return 1

    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return 0


sys.exit(main())

# Copyright MadFall. All Rights Reserved.
#
# Builds the materials for imported photo-scanned models (Poly Haven, CC0):
#   M_MadModel         opaque: base colour, OpenGL normal (flipped on import),
#                      roughness and metallic maps through the model's own UVs.
#   M_MadModelFoliage  the same, alpha-masked and lit through from behind, for
#                      leaves and fronds.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/make_model_material.py"
#
# WHY UV-MAPPED, NOT PROJECTED LIKE THE VOXEL MATERIALS
# Voxel faces have no UVs, so the voxel materials project textures by position.
# A scanned model is authored with UVs and a texture made for them; projecting
# would smear a barrel's bands and a crate's slats across its curves.
#
# WEATHER
# Wet darkening and gloss from MPC_MadWeather, scaled by a Weather parameter
# (1 outdoors), like the voxel materials. No snow cover: a model's small upward
# faces would need a mask per model to look right, and a white-dusted bush with
# its sides bare read as a bug in the voxel material's early days.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
VERSION_TAG = "MadFallModelMaterialVersion"
MATERIAL_VERSION = "1"


def build(material, editing, library, foliage):
    ok = True
    y = [-500]

    def place():
        y[0] += 140
        return y[0]

    def texture_parameter(name, default_path, sampler):
        node = editing.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -900, place())
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("texture", library.load_asset(default_path))
        node.set_editor_property("sampler_type", sampler)
        return node

    colour = texture_parameter("BaseColor", "/Engine/EngineMaterials/DefaultDiffuse", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    normal = texture_parameter("Normal", "/Engine/EngineMaterials/FlatNormal", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    rough = texture_parameter("Roughness", "/Engine/EngineMaterials/DefaultDiffuse_TC_Masks", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
    metal = texture_parameter("Metallic", "/Engine/EngineMaterials/DefaultDiffuse_TC_Masks", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)

    metal_scale = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, place())
    metal_scale.set_editor_property("parameter_name", "MetallicScale")
    metal_scale.set_editor_property("default_value", 0.0)
    weather = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, place())
    weather.set_editor_property("parameter_name", "Weather")
    weather.set_editor_property("default_value", 1.0)
    wet_param = editing.create_material_expression(material, unreal.MaterialExpressionCollectionParameter, -900, place())
    wet_param.set_editor_property("collection", library.load_asset("/Game/Materials/MPC_MadWeather"))
    wet_param.set_editor_property("parameter_name", "Wetness")

    wet = editing.create_material_expression(material, unreal.MaterialExpressionMultiply, -650, place())
    ok &= editing.connect_material_expressions(wet_param, "", wet, "A")
    ok &= editing.connect_material_expressions(weather, "", wet, "B")

    # Wet: darker, glossier.
    darkening = editing.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -450, -300)
    darkening.set_editor_property("const_a", 1.0)
    darkening.set_editor_property("const_b", 0.62)
    ok &= editing.connect_material_expressions(wet, "", darkening, "Alpha")
    albedo = editing.create_material_expression(material, unreal.MaterialExpressionMultiply, -250, -300)
    ok &= editing.connect_material_expressions(colour, "RGB", albedo, "A")
    ok &= editing.connect_material_expressions(darkening, "", albedo, "B")
    ok &= editing.connect_material_property(albedo, "", unreal.MaterialProperty.MP_BASE_COLOR)

    glossy = editing.create_material_expression(material, unreal.MaterialExpressionMin, -450, 100)
    glossy.set_editor_property("const_b", 0.2)
    ok &= editing.connect_material_expressions(rough, "R", glossy, "A")
    roughness = editing.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -250, 100)
    ok &= editing.connect_material_expressions(rough, "R", roughness, "A")
    ok &= editing.connect_material_expressions(glossy, "", roughness, "B")
    ok &= editing.connect_material_expressions(wet, "", roughness, "Alpha")
    ok &= editing.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    metallic = editing.create_material_expression(material, unreal.MaterialExpressionMultiply, -250, 250)
    ok &= editing.connect_material_expressions(metal, "R", metallic, "A")
    ok &= editing.connect_material_expressions(metal_scale, "", metallic, "B")
    ok &= editing.connect_material_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)
    ok &= editing.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)

    if foliage:
        alpha = texture_parameter("Alpha", "/Engine/EngineMaterials/DefaultDiffuse_TC_Masks", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
        ok &= editing.connect_material_property(alpha, "R", unreal.MaterialProperty.MP_OPACITY_MASK)
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        material.set_editor_property("two_sided", True)
        # Leaves transmit light arriving from behind, as the voxel foliage does.
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
        transmission = editing.create_material_expression(material, unreal.MaterialExpressionMultiply, -250, 450)
        transmission.set_editor_property("const_b", 0.6)
        ok &= editing.connect_material_expressions(albedo, "", transmission, "A")
        ok &= editing.connect_material_property(transmission, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    else:
        material.set_editor_property("two_sided", False)

    # Model blocks are drawn as instanced static meshes.
    material.set_editor_property("used_with_instanced_static_meshes", True)
    return ok


def make(editing, library, asset_name, foliage):
    full_path = "{}/{}".format(PACKAGE_PATH, asset_name)
    if library.does_asset_exist(full_path):
        material = library.load_asset(full_path)
        if library.get_metadata_tag(material, VERSION_TAG) == MATERIAL_VERSION:
            unreal.log("[MadFall] {} is already version {}.".format(full_path, MATERIAL_VERSION))
            return True
        editing.delete_all_material_expressions(material)
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        material = tools.create_asset(asset_name, PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew())
        if material is None:
            unreal.log_error("[MadFall] Could not create {}".format(full_path))
            return False
    if not build(material, editing, library, foliage):
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return False
    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return True


def main():
    editing = unreal.MaterialEditingLibrary
    library = unreal.EditorAssetLibrary
    ok = make(editing, library, "M_MadModel", foliage=False)
    ok &= make(editing, library, "M_MadModelFoliage", foliage=True)
    return 0 if ok else 1


sys.exit(main())

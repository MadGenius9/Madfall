# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadFlame - the flame UMadModelInstanceSubsystem
# draws on every light-giving block (torches, campfires), on the engine cone.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_flame_material.py"
#
# Additive and unlit: a flame gives light rather than taking it, and additive
# needs no sorting against the other translucency. Yellow-white at the base,
# orange-red and fading toward the tip, flickering on two sines with a phase
# per instance so a row of torches does not pulse in step. The point light
# itself stays steady: animating up to 16 lights a chunk from the game thread
# each frame would cost more than it adds.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadFlame"
VERSION_TAG = "MadFallFlameVersion"
MATERIAL_VERSION = "3"

FLAME_HLSL = """
float H = saturate(Local.z / 100.0 + 0.5);               // 0 at the cone's base, 1 at its tip
float Phase = Random * 37.0;
float Flicker = 0.78 + 0.14 * sin(Time * 13.0 + Phase) + 0.08 * sin(Time * 29.0 + Phase * 1.7);
float3 Core = float3(1.0, 0.62, 0.22);
float3 Edge = float3(0.9, 0.22, 0.03);
float3 C = lerp(Core, Edge, pow(H, 0.7));
// Soft silhouette: fade where the cone turns away from the eye, so it reads as
// a glow and not a hard-edged solid.
float Facing = pow(saturate(abs(dot(normalize(N), normalize(V)))), 1.5);
return C * (1.0 - 0.75 * H) * Flicker * Facing * Strength;
"""


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

    local = editing.create_material_expression(material, unreal.MaterialExpressionLocalPosition, -900, -100)
    time = editing.create_material_expression(material, unreal.MaterialExpressionTime, -900, 0)
    random = editing.create_material_expression(material, unreal.MaterialExpressionPerInstanceRandom, -900, 100)
    strength = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, 200)
    strength.set_editor_property("parameter_name", "Strength")
    strength.set_editor_property("default_value", 1.4)
    normal = editing.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -900, 300)
    camera_vector = editing.create_material_expression(material, unreal.MaterialExpressionCameraVectorWS, -900, 400)

    flame = editing.create_material_expression(material, unreal.MaterialExpressionCustom, -500, 0)
    flame.set_editor_property("code", FLAME_HLSL)
    flame.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    flame.set_editor_property("description", "MadFall flame")
    inputs = []
    for name in ["Local", "Time", "Random", "Strength", "N", "V"]:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    flame.set_editor_property("inputs", inputs)

    ok = True
    ok &= editing.connect_material_expressions(local, "", flame, "Local")
    ok &= editing.connect_material_expressions(time, "", flame, "Time")
    ok &= editing.connect_material_expressions(random, "", flame, "Random")
    ok &= editing.connect_material_expressions(strength, "", flame, "Strength")
    ok &= editing.connect_material_expressions(normal, "", flame, "N")
    ok &= editing.connect_material_expressions(camera_vector, "", flame, "V")
    ok &= editing.connect_material_property(flame, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if not ok:
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return 1

    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return 0


sys.exit(main())

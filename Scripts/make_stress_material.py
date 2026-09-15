# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadStressOverlay - the translucent boxes
# UMadStressOverlaySubsystem draws over blocks to shade structural stress.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_stress_material.py"
#
# Per-instance custom data: 0 = stress (0 sound .. 1 at the limit), 1 = failing.
# The colour ramp (blue, green at 0.5, yellow at 0.8, red at 1) matches
# MadFall::StressOverlay::RampColour. Unlit and translucent so the block's own
# pattern shows through the wash; brighter toward each box's edges so the
# outline of every block reads even where colours are close; a failing block
# pulses.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadStressOverlay"
VERSION_TAG = "MadFallStressVersion"
MATERIAL_VERSION = "1"

OVERLAY_HLSL = """
float S = max(Stress, 0.0);
float3 Blue = float3(0.05, 0.25, 1.0);
float3 Green = float3(0.1, 0.9, 0.15);
float3 Yellow = float3(1.0, 0.85, 0.05);
float3 Red = float3(1.0, 0.05, 0.02);
float3 C = S < 0.5 ? lerp(Blue, Green, S / 0.5) : (S < 0.8 ? lerp(Green, Yellow, (S - 0.5) / 0.3) : lerp(Yellow, Red, min(1.0, (S - 0.8) / 0.2)));

// Near a box edge two local axes are near 50: on a face one axis is 50, so the
// middle one of the three says how close the point is to that face's edge.
float3 A = abs(Local) / 50.0;
float Middle = (A.x + A.y + A.z) - max(A.x, max(A.y, A.z)) - min(A.x, min(A.y, A.z));
float Outline = saturate((Middle - 0.86) / 0.1);
float Pulse = Failing > 0.5 ? 0.5 + 0.5 * sin(Time * 9.0) : 0.0;
float Alpha = 0.18 + 0.5 * Outline + 0.3 * Pulse;
return float4(C * (0.8 + 1.2 * Outline + Pulse), Alpha);
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
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("used_with_instanced_static_meshes", True)

    ok = True
    local = editing.create_material_expression(material, unreal.MaterialExpressionLocalPosition, -1000, -200)
    stress = editing.create_material_expression(material, unreal.MaterialExpressionPerInstanceCustomData, -1000, -80)
    stress.set_editor_property("data_index", 0)
    failing = editing.create_material_expression(material, unreal.MaterialExpressionPerInstanceCustomData, -1000, 40)
    failing.set_editor_property("data_index", 1)
    time = editing.create_material_expression(material, unreal.MaterialExpressionTime, -1000, 160)

    node = editing.create_material_expression(material, unreal.MaterialExpressionCustom, -600, -60)
    node.set_editor_property("code", OVERLAY_HLSL)
    node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    node.set_editor_property("description", "MadFall stress overlay")
    inputs = []
    for name in ["Local", "Stress", "Failing", "Time"]:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    node.set_editor_property("inputs", inputs)
    ok &= editing.connect_material_expressions(local, "", node, "Local")
    ok &= editing.connect_material_expressions(stress, "", node, "Stress")
    ok &= editing.connect_material_expressions(failing, "", node, "Failing")
    ok &= editing.connect_material_expressions(time, "", node, "Time")

    rgb = editing.create_material_expression(material, unreal.MaterialExpressionComponentMask, -300, -100)
    for channel, on in (("r", True), ("g", True), ("b", True), ("a", False)):
        rgb.set_editor_property(channel, on)
    alpha = editing.create_material_expression(material, unreal.MaterialExpressionComponentMask, -300, 20)
    for channel, on in (("r", False), ("g", False), ("b", False), ("a", True)):
        alpha.set_editor_property(channel, on)
    ok &= editing.connect_material_expressions(node, "", rgb, "")
    ok &= editing.connect_material_expressions(node, "", alpha, "")
    ok &= editing.connect_material_property(rgb, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    ok &= editing.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
    if not ok:
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return 1

    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return 0


sys.exit(main())

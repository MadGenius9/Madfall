# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadZombieOverlay - rot, grime and blood painted over
# the mannequin a zombie is drawn with (UMadHumanoidRigComponent).
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/make_zombie_overlay_material.py"
#
# WHY AN OVERLAY: a mannequin tinted green read as a clean painted robot, not a
# corpse. Replacing its material would lose the panel normals and roughness
# that make it read as a body, and a material sampling the mannequin's own
# textures would reference Epic assets this repository cannot contain (they are
# copied from the engine, see Scripts/copy_mannequin.ps1). A mesh's overlay
# material draws a second translucent pass over whatever the base looks like,
# so this adds decay on top and references nothing but itself.
#
# The patterns come from the pre-skinned position - the mannequin's reference
# pose in centimetres, Z up from the feet, facing +Y - so a blotch stays on the
# same patch of skin however the body moves, and differs per zombie by Seed.
#
# PARAMETERS
# - Seed: shifts every pattern, so no two zombies wear the same wounds.
# - Rot: 0..1, grey-green blotches. Blood: 0..1, around the mouth, down the
#   chest and on the hands and forearms. Grime is always on, heavier low down.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadZombieOverlay"
VERSION_TAG = "MadFallZombieOverlayVersion"
MATERIAL_VERSION = "5"

OVERLAY_HLSL = """
struct FMadDecay
{
	float Hash(float3 p)
	{
		p = frac(p * 0.3183099 + 0.1);
		p *= 17.0;
		return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
	}
	float Noise(float3 x)
	{
		float3 i = floor(x);
		float3 f = frac(x);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(
			lerp(lerp(Hash(i), Hash(i + float3(1, 0, 0)), f.x), lerp(Hash(i + float3(0, 1, 0)), Hash(i + float3(1, 1, 0)), f.x), f.y),
			lerp(lerp(Hash(i + float3(0, 0, 1)), Hash(i + float3(1, 0, 1)), f.x), lerp(Hash(i + float3(0, 1, 1)), Hash(i + float3(1, 1, 1)), f.x), f.y),
			f.z);
	}
	float Fbm(float3 x)
	{
		return Noise(x) * 0.55 + Noise(x * 2.13) * 0.3 + Noise(x * 4.71) * 0.15;
	}
};
FMadDecay D;

float3 Q = Position + Seed * float3(13.1, 7.7, 3.3);
float Front = saturate(Normal.y);

// Grey-green bruising in broad blotches.
float RotMask = smoothstep(0.42, 0.62, D.Fbm(Q / 12.0)) * Rot;

// Dirt, heavier towards the feet.
float Grime = saturate(D.Fbm(Q / 6.0 + 5.0) * 1.6 - 0.45) * saturate(1.5 - Position.z / 150.0);

// Blood: a smear round the mouth, drips down the chest, and the hands and
// forearms (far out to the sides in the reference pose).
float Mouth = exp(-pow((Position.z - 150.0) / 9.0, 2.0)) * exp(-pow(Position.x / 8.0, 2.0)) * Front;
float Chest = saturate(1.0 - abs(Position.z - 122.0) / 28.0) * Front * smoothstep(0.55, 0.62, D.Fbm(float3(Q.x / 3.0, Q.y / 3.0, Q.z / 12.0) + 11.0));
float Hands = saturate((abs(Position.x) - 35.0) / 12.0) * smoothstep(0.38, 0.46, D.Fbm(Q / 4.0 + 23.0));
float BloodMask = saturate(Mouth * 1.4 + Chest + Hands) * Blood;

float3 C = float3(0.07, 0.055, 0.04);
float A = Grime * 0.75;
C = lerp(C, float3(0.13, 0.17, 0.07), RotMask);
A = max(A, RotMask * 0.9);
C = lerp(C, float3(0.16, 0.01, 0.008), BloodMask);
A = max(A, BloodMask * 0.92);
return float4(C, A);
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

    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    material.set_editor_property("used_with_skeletal_mesh", True)

    ok = True

    def scalar_param(name, default, y):
        node = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", default)
        return node

    # Pre-skinned attributes exist only in the vertex shader; a vertex interpolator
    # carries them to the pixel shader (connecting them directly fails to compile).
    def interpolated(expression_class, y):
        source = editing.create_material_expression(material, expression_class, -1150, y)
        interpolator = editing.create_material_expression(material, unreal.MaterialExpressionVertexInterpolator, -900, y)
        editing.connect_material_expressions(source, "", interpolator, "")
        return interpolator

    inputs = {
        "Position": interpolated(unreal.MaterialExpressionPreSkinnedPosition, -300),
        "Normal": interpolated(unreal.MaterialExpressionPreSkinnedNormal, -180),
        "Seed": scalar_param("Seed", 0.0, -60),
        "Rot": scalar_param("Rot", 1.0, 60),
        "Blood": scalar_param("Blood", 1.0, 180),
    }

    node = editing.create_material_expression(material, unreal.MaterialExpressionCustom, -450, 0)
    node.set_editor_property("code", OVERLAY_HLSL)
    node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    node.set_editor_property("description", "MadFall zombie decay")
    custom_inputs = []
    for name in inputs:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        custom_inputs.append(custom_input)
    node.set_editor_property("inputs", custom_inputs)
    for name, source in inputs.items():
        ok &= editing.connect_material_expressions(source, "", node, name)

    colour = editing.create_material_expression(material, unreal.MaterialExpressionComponentMask, -200, -60)
    colour.set_editor_property("r", True)
    colour.set_editor_property("g", True)
    colour.set_editor_property("b", True)
    colour.set_editor_property("a", False)
    opacity = editing.create_material_expression(material, unreal.MaterialExpressionComponentMask, -200, 60)
    for channel in ("r", "g", "b"):
        opacity.set_editor_property(channel, False)
    opacity.set_editor_property("a", True)
    ok &= editing.connect_material_expressions(node, "", colour, "")
    ok &= editing.connect_material_expressions(node, "", opacity, "")

    roughness = editing.create_material_expression(material, unreal.MaterialExpressionConstant, -200, 180)
    roughness.set_editor_property("r", 0.55)
    ok &= editing.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    ok &= editing.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)
    ok &= editing.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    if not ok:
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return 1

    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return 0


sys.exit(main())

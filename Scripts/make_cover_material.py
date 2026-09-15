# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadGroundCover - grass tufts and wildflowers drawn
# by UMadGroundCoverSubsystem on the engine plane, stood upright.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_cover_material.py"
#
# WHAT IT DRAWS
# Blades cut out of the quad by an opacity mask: seven per card, each with its
# own height, lean and shade (from the card's per-instance random), tapering
# to a point, darker at the root. With Flower = 1 the card is two thin stems
# under a round flower head in one of four colours. Nothing is textured.
#
# WHY THESE CHOICES
# - Masked, not translucent: thousands of cards need depth sorting to be free.
# - The normal is world up: a vertical card lit by its own facing flickers
#   light and dark as it turns; lit like the ground under it, a tuft reads as
#   part of the grass.
# - A world-position offset sways the tips (height squared, so roots stay put).
#   Shadows are off on the component, so the sway invalidates no shadow maps.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadGroundCover"
VERSION_TAG = "MadFallCoverVersion"
MATERIAL_VERSION = "4"

CARD_HLSL = """
struct FMadCover
{
	float H(float x) { return frac(sin(x * 127.1 + 311.7) * 43758.5453); }
};
FMadCover F;

float U = saturate(Local.x / 100.0 + 0.5);
float V = saturate(Local.y / 100.0 + 0.5);
float Seed = Random * 97.0;
float3 C = Color.rgb;
float Alpha = 0.0;

if (Flower < 0.5)
{
	const float Blades = 7.0;
	float B = min(floor(U * Blades), Blades - 1.0);
	float Top = 0.5 + 0.5 * F.H(B * 3.1 + Seed);
	float Centre = (B + 0.5 + (F.H(B + Seed * 1.7) - 0.5) * 0.5) / Blades + (F.H(B * 5.3 + Seed) - 0.5) * 0.12 * V;
	float Half = (0.55 / Blades) * (1.0 - V / Top);
	if (V < Top && abs(U - Centre) < Half)
	{
		Alpha = 1.0;
		C *= lerp(0.42, 1.05, V / Top) * (0.82 + 0.3 * F.H(B * 7.7 + Seed));
	}
}
else
{
	float Head = 0.78 + 0.1 * F.H(Seed);
	for (int Stem = 0; Stem < 2; ++Stem)
	{
		float X = Stem == 0 ? 0.5 : 0.3 + 0.4 * F.H(Seed + 3.0);
		float Top = Stem == 0 ? Head : Head * 0.6;
		if (V < Top && abs(U - X) < 0.025)
		{
			Alpha = 1.0;
			C = Color.rgb * lerp(0.5, 1.0, V);
		}
	}
	float R = length(float2(U - 0.5, (V - Head) * 0.6));
	if (R < 0.2)
	{
		Alpha = 1.0;
		float Pick = floor(F.H(Seed + 11.0) * 4.0);
		float3 Petal = Pick == 0.0 ? float3(0.9, 0.88, 0.8) : (Pick == 1.0 ? float3(0.75, 0.08, 0.06) : (Pick == 2.0 ? float3(0.35, 0.3, 0.85) : float3(0.95, 0.72, 0.05)));
		C = R < 0.07 ? float3(0.9, 0.65, 0.1) : Petal;
	}
}
// Weather: wet grass darkens; snow buries the lower blades and dusts what sticks out.
C *= lerp(1.0, 0.7, saturate(Wet));
if (V < saturate(Snow) * 0.6) { Alpha = 0.0; }
C = lerp(C, float3(0.8, 0.82, 0.86), saturate(Snow) * 0.5 * saturate(V * 1.5));
return float4(C, Alpha);
"""

SWAY_HLSL = """
float V = saturate(Local.y / 100.0 + 0.5);
float Gust = sin(Time * 1.6 + World.x * 0.013 + World.y * 0.011) + 0.4 * sin(Time * 3.3 + World.x * 0.031);
// A breeze sways 5 cm at the tip; a gale lays it over by 30.
return float3(Gust, Gust * 0.6, 0.0) * V * V * lerp(3.0, 30.0, saturate(Wind));
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

    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("tangent_space_normal", False)
    material.set_editor_property("used_with_instanced_static_meshes", True)

    def custom(code, output_type, names, x, y, description):
        node = editing.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
        node.set_editor_property("code", code)
        node.set_editor_property("output_type", output_type)
        node.set_editor_property("description", description)
        inputs = []
        for name in names:
            custom_input = unreal.CustomInput()
            custom_input.set_editor_property("input_name", name)
            inputs.append(custom_input)
        node.set_editor_property("inputs", inputs)
        return node

    ok = True
    local = editing.create_material_expression(material, unreal.MaterialExpressionLocalPosition, -1000, -200)
    random = editing.create_material_expression(material, unreal.MaterialExpressionPerInstanceRandom, -1000, -80)
    colour = editing.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -1000, 40)
    colour.set_editor_property("parameter_name", "Color")
    colour.set_editor_property("default_value", unreal.LinearColor(0.1, 0.25, 0.05, 1.0))
    flower = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -1000, 160)
    flower.set_editor_property("parameter_name", "Flower")
    flower.set_editor_property("default_value", 0.0)

    collection = unreal.EditorAssetLibrary.load_asset("/Game/Materials/MPC_MadWeather")
    def weather(name, y):
        node = editing.create_material_expression(material, unreal.MaterialExpressionCollectionParameter, -1000, y)
        node.set_editor_property("collection", collection)
        node.set_editor_property("parameter_name", name)
        return node
    wet = weather("Wetness", 520)
    snow = weather("SnowCover", 620)
    wind = weather("Wind", 720)
    card = custom(CARD_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT4, ["Local", "Random", "Color", "Flower", "Wet", "Snow"], -600, -80, "MadFall grass card")
    ok &= editing.connect_material_expressions(wet, "", card, "Wet")
    ok &= editing.connect_material_expressions(snow, "", card, "Snow")
    ok &= editing.connect_material_expressions(local, "", card, "Local")
    ok &= editing.connect_material_expressions(random, "", card, "Random")
    ok &= editing.connect_material_expressions(colour, "", card, "Color")
    ok &= editing.connect_material_expressions(flower, "", card, "Flower")

    rgb = editing.create_material_expression(material, unreal.MaterialExpressionComponentMask, -300, -120)
    rgb.set_editor_property("r", True)
    rgb.set_editor_property("g", True)
    rgb.set_editor_property("b", True)
    rgb.set_editor_property("a", False)
    alpha = editing.create_material_expression(material, unreal.MaterialExpressionComponentMask, -300, 0)
    alpha.set_editor_property("r", False)
    alpha.set_editor_property("g", False)
    alpha.set_editor_property("b", False)
    alpha.set_editor_property("a", True)
    ok &= editing.connect_material_expressions(card, "", rgb, "")
    ok &= editing.connect_material_expressions(card, "", alpha, "")

    up = editing.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 120)
    up.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 1.0, 0.0))
    roughness = editing.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 220)
    roughness.set_editor_property("r", 0.9)

    world = editing.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -1000, 300)
    time = editing.create_material_expression(material, unreal.MaterialExpressionTime, -1000, 400)
    sway = custom(SWAY_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, ["Local", "World", "Time", "Wind"], -600, 320, "MadFall grass sway")
    ok &= editing.connect_material_expressions(wind, "", sway, "Wind")
    ok &= editing.connect_material_expressions(local, "", sway, "Local")
    ok &= editing.connect_material_expressions(world, "", sway, "World")
    ok &= editing.connect_material_expressions(time, "", sway, "Time")

    ok &= editing.connect_material_property(rgb, "", unreal.MaterialProperty.MP_BASE_COLOR)
    ok &= editing.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    ok &= editing.connect_material_property(up, "", unreal.MaterialProperty.MP_NORMAL)
    ok &= editing.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    ok &= editing.connect_material_property(sway, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    if not ok:
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return 1

    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return 0


sys.exit(main())

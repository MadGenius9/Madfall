# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadCharacter - the pixel-art look of the code-built
# characters (UMadHumanoidRigComponent, UMadQuadrupedRigComponent): skin, a
# face, hair, clothes and fur on box body parts, with no textures.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_character_material.py"
#
# WHY
# The rigs were tinted engine primitives: a flat green head on a black box
# read as a placeholder, not a zombie, and clashed with blocks drawn at a
# pixel-art scale. This draws the same kind of pattern as M_MadVoxel, from the
# part's own local position, so it rides with the animated part.
#
# PARAMETERS (set per part by the rigs)
# - Color: the part's base colour. Accent: hair on a head, the sleeve on an arm
#   (skin), the skin showing through torn clothes, the belly of fur.
# - Size: the part's size in cm, so noise texels are ~3 cm on any part.
# - Part: 0 skin, 1 shirt, 2 head, 3 trousers, 4 fur, 5 animal head.
# - Zombie: 1 rots the skin, reddens the eyes, bares the teeth and bloodies the
#   clothes; 0 is a living face and clean clothes (a trader).
# - Seed: varies the blotches between characters. Flash: the red hit flash.
#
# Faces are drawn on the part's +X face (the rigs face +X), on an 8x8 grid
# regardless of the part's size, so features stay in proportion.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadCharacter"
VERSION_TAG = "MadFallCharacterVersion"
MATERIAL_VERSION = "2"

CHARACTER_HLSL = """
struct FMadCharacterPattern
{
	float H(float3 p)
	{
		p = frac(p * float3(0.1031, 0.1030, 0.0973));
		p += dot(p, p.yzx + 33.33);
		return frac((p.x + p.y) * p.z);
	}
};
FMadCharacterPattern F;

float3 P = saturate(Local / 100.0 + 0.5);
float3 A = abs(N);
int Face = 0;
float2 UV = P.yz;
float2 Dim = Size.yz;
if (A.x >= A.y && A.x >= A.z) { Face = N.x > 0 ? 0 : 1; UV = P.yz; Dim = Size.yz; }
else if (A.y >= A.z) { Face = N.y > 0 ? 2 : 3; UV = P.xz; Dim = Size.xz; }
else { Face = N.z > 0 ? 4 : 5; UV = P.xy; Dim = Size.xy; }

float2 Texels = max(floor(Dim / 3.0), 2.0);
float2 T = min(floor(UV * Texels), Texels - 1.0);   // noise texel
float2 G = min(floor(UV * 8.0), 7.0);                // feature grid, 0 at the bottom
int Id = (int)round(Part);
bool bZombie = Zombie > 0.5;
float S = Seed * 17.0 + Id * 5.0 + Face * 3.0;
float N1 = F.H(float3(T, S));
float N2 = F.H(float3(floor(T / 2.0), S + 29.0));

float3 C = Color.rgb;
float Shade = 0.9 + 0.14 * N1;

// An arm is a sleeve down to the hand; a zombie's is torn off at the elbow.
bool bSleeve = Id == 0 && G.y >= (bZombie ? 4.0 : 2.0) && (!bZombie || N2 < 0.8 || G.y >= 6.0);

if ((Id == 0 && !bSleeve) || Id == 2)   // skin
{
	if (bZombie && N2 > 0.74) { C *= float3(0.78, 0.85, 0.62); Shade = 0.8 + 0.12 * N1; }
	if (bZombie && N1 > 0.965) { C = float3(0.12, 0.015, 0.012); Shade = 1.0; }
}

if (Id == 2)                     // head: hair, and a face on +X
{
	bool bHair = Face == 4 || (G.y >= 7.0 && Face != 5) || (Face == 1 && G.y >= 3.0);
	if (bZombie && N2 > 0.55) { bHair = bHair && Face != 4 ? G.y >= 7.0 && N1 > 0.4 : bHair && N1 > 0.3; }
	if (bHair) { C = Accent.rgb; Shade = 0.75 + 0.3 * N1; }

	if (Face == 0)
	{
		if (bZombie)
		{
			if (G.y == 5.0 && G.x >= 1.0 && G.x <= 6.0) { C = Color.rgb * 0.55; Shade = 1.0; }            // heavy brow
			if (G.y == 4.0 && (G.x == 2.0 || G.x == 5.0)) { C = float3(0.75, 0.05, 0.02); Shade = 1.0; } // eyes
			if (G.y == 4.0 && (G.x == 1.0 || G.x == 3.0 || G.x == 4.0 || G.x == 6.0)) { C = Color.rgb * 0.45; Shade = 1.0; }
			if (G.y == 2.0 && G.x >= 2.0 && G.x <= 5.0) { C = float3(0.03, 0.01, 0.01); Shade = 1.0; }   // open mouth
			if (G.y == 1.0 && G.x >= 2.0 && G.x <= 5.0) { C = fmod(G.x, 2.0) == 0.0 ? float3(0.7, 0.66, 0.5) : float3(0.03, 0.01, 0.01); Shade = 1.0; }
		}
		else
		{
			if (G.y == 5.0 && (G.x == 1.0 || G.x == 2.0 || G.x == 5.0 || G.x == 6.0)) { C = Accent.rgb; Shade = 1.0; } // brows
			if (G.y == 4.0 && (G.x == 1.0 || G.x == 6.0)) { C = float3(0.85, 0.85, 0.82); Shade = 1.0; }              // whites
			if (G.y == 4.0 && (G.x == 2.0 || G.x == 5.0)) { C = float3(0.05, 0.08, 0.12); Shade = 1.0; }              // irises
			if (G.y == 2.0 && G.x >= 3.0 && G.x <= 4.0) { C = Color.rgb * float3(0.7, 0.45, 0.45); Shade = 1.0; }     // mouth
			if (G.y == 3.0 && (G.x == 3.0 || G.x == 4.0)) { Shade = 0.85; }                                           // nose
		}
	}
}

if (Id == 1 || Id == 3 || bSleeve)   // clothes
{
	float3 Skin = Id == 0 ? Color.rgb : Accent.rgb;
	if (bSleeve) { C = Accent.rgb; }
	Shade = (fmod(T.x + T.y, 2.0) == 0.0 ? 0.93 : 1.03) * (0.88 + 0.16 * N2);
	if (Id == 1 && Face == 0 && G.y >= 6.0 && G.x >= 3.0 && G.x <= 4.0) { C = Accent.rgb; Shade = 0.95; }  // open collar
	if (Id == 1 && G.y < 1.0) { C *= 0.45; }                                                               // belt
	if (Id == 3 && Face == 0 && G.x >= 3.0 && G.x <= 4.0 && G.y >= 6.0) { Shade *= 0.8; }                  // fly seam
	if (bZombie)
	{
		float Stain = F.H(float3(floor(T / 3.0), S + 41.0));
		if (Stain > 0.87 && N1 > 0.3) { C = float3(0.11, 0.012, 0.01); Shade = 0.85 + 0.2 * N1; }           // blood, dried dark
		else if (N2 > 0.92 && N1 > 0.6) { C = Skin; Shade = 0.8; }                                          // tears
	}
}

if (Id == 4 || Id == 5)          // fur
{
	Shade = 0.82 + 0.3 * N1;
	if (Face == 5 || (Face != 4 && G.y <= 1.0)) { C = lerp(C, Accent.rgb, 0.6); }                         // belly
	if (Id == 5 && (Face == 2 || Face == 3) && G.x >= 5.0 && G.x <= 6.0 && G.y == 5.0)                    // eyes
	{
		C = G.x == 6.0 ? float3(0.02, 0.02, 0.02) : float3(0.15, 0.1, 0.06);
		Shade = 1.0;
	}
}

C = saturate(C * Shade);
return lerp(C, float3(0.9, 0.05, 0.03), saturate(Flash));
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

    ok = True

    def vector_param(name, default, y):
        node = editing.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -900, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", default)
        return node

    def scalar_param(name, default, y):
        node = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", default)
        return node

    local = editing.create_material_expression(material, unreal.MaterialExpressionLocalPosition, -1100, -300)
    world_normal = editing.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -1100, -180)
    normal = editing.create_material_expression(material, unreal.MaterialExpressionTransform, -900, -180)
    normal.set_editor_property("transform_source_type", unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
    normal.set_editor_property("transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_LOCAL)
    ok &= editing.connect_material_expressions(world_normal, "", normal, "")

    inputs = {
        "Local": local,
        "N": normal,
        "Color": vector_param("Color", unreal.LinearColor(0.3, 0.3, 0.3, 1.0), -60),
        "Accent": vector_param("Accent", unreal.LinearColor(0.05, 0.04, 0.03, 1.0), 60),
        "Size": vector_param("Size", unreal.LinearColor(30.0, 30.0, 30.0, 1.0), 180),
        "Part": scalar_param("Part", 0.0, 300),
        "Zombie": scalar_param("Zombie", 1.0, 400),
        "Seed": scalar_param("Seed", 0.0, 500),
        "Flash": scalar_param("Flash", 0.0, 600),
    }

    node = editing.create_material_expression(material, unreal.MaterialExpressionCustom, -450, 0)
    node.set_editor_property("code", CHARACTER_HLSL)
    node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    node.set_editor_property("description", "MadFall character pattern")
    custom_inputs = []
    for name in inputs:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        custom_inputs.append(custom_input)
    node.set_editor_property("inputs", custom_inputs)
    for name, source in inputs.items():
        ok &= editing.connect_material_expressions(source, "", node, name)

    roughness = editing.create_material_expression(material, unreal.MaterialExpressionConstant, -450, 300)
    roughness.set_editor_property("r", 0.85)
    ok &= editing.connect_material_property(node, "", unreal.MaterialProperty.MP_BASE_COLOR)
    ok &= editing.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    if not ok or editing.get_material_property_input_node(material, unreal.MaterialProperty.MP_BASE_COLOR) is None:
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return 1

    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return 0


sys.exit(main())

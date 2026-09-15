# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadVoxel - the material every generated chunk
# section, far terrain tile and default surface draws with - and its variants:
# M_MadVoxelHeld (the block in hand, and model blocks such as barrels and
# bushes, patterned from their own local position) and M_MadVoxelFoliage (leaf
# surfaces).
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_voxel_material.py"
#
# WHAT IT DRAWS
# Base colour is the surface's colour (vertex RGB) with a procedural pattern
# over it, chosen per surface by vertex alpha (see MadFall::Surfaces::PatternNames):
# stone, dirt, grass, sand, planks, bark, leaves, concrete, brick, metal, ore,
# farmland, cloth, water, gravel, or plain. Patterns are drawn in HLSL from world
# position at 16 texels per voxel, the pixel-art scale of a block game, so a
# surface looks the same in the chunk mesh, the far terrain and across chunk
# borders, and needs no texture assets or UVs.
#
# WHY PROCEDURAL AND NOT TEXTURES
# There are no texture assets, and a texture per material class would need an
# atlas or an array, UVs per face and an import pipeline before anything looked
# different. A pattern in the shader is one data field per surface, stays
# moddable (a mod names a pattern for its own surface), and leaves room for real
# textures later: a surface with its own "material" skips all of this.
#
# COSTS, STATED
# - A hash per texel aliases at a distance, so the pattern fades to its average
#   between 25 m and 65 m. Far terrain is plain colour in practice.
# - Smooth terrain faces are projected along their dominant axis, so a slope's
#   pattern steps where that axis changes.
#
# Corner ambient occlusion comes baked from the cubic mesher in UV1.x (see
# ComputeFaceOcclusion in MadChunkMesher.cpp) and darkens the base colour.
#
# RELIEF (version 5): each pattern also returns a height, evaluated at the four
# neighbouring texels to tilt the normal, so mortar is recessed, nails and
# rivets stand proud and stone is rough under a low sun; placed blocks (UV1.y
# = 1, from the cubic mesher) get a one-texel bevel round each face. Cost: four
# more pattern evaluations a pixel within 65 m, none past it.
#
# Idempotent and versioned: an asset stamped with the current MATERIAL_VERSION is
# left alone; an older one has its expressions rebuilt in place, so every
# reference to the asset stays valid.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadVoxel"
HELD_ASSET_NAME = "M_MadVoxelHeld"   # the same patterns on the block in the survivor's hand
FOLIAGE_ASSET_NAME = "M_MadVoxelFoliage"   # leaves: the same, lit through from behind
VERSION_TAG = "MadFallVoxelMaterialVersion"
MATERIAL_VERSION = "8"

# Pattern index from vertex alpha: alpha = 255 - index * 16.
PATTERN_ID = "int Id = (int)round((1.0 - VA) * 255.0 / 16.0);\n"

ALBEDO_HLSL = """
struct FMadVoxelPattern
{
	float H(float3 p)
	{
		p = frac(p * float3(0.1031, 0.1030, 0.0973));
		p += dot(p, p.yzx + 33.33);
		return frac((p.x + p.y) * p.z);
	}

	/**
	 * Colour (rgb) and height (a, 0 recessed .. 1 raised) of pattern Id at world
	 * texel T on a face. Height drives the bump: evaluated again at the four
	 * neighbouring texels, its slope tilts the normal, so mortar, seams, nails
	 * and grain catch light and shadow.
	 */
	float4 Eval(float2 T, int Id, int Face, float Seed, bool bTop, bool bBottom, float3 VC)
	{
		float2 Cell = floor(T / 16.0);
		float2 L = T - Cell * 16.0;   // texel inside the voxel face, 0..15
		float N1 = H(float3(T, Seed));
		float N2 = H(float3(floor(T / 4.0), Seed + 17.0));
		bool bEdge = L.x == 0.0 || L.y == 0.0 || L.x == 15.0 || L.y == 15.0;

		float Shade = 1.0;
		float Height = 0.5;
		float3 C = VC;

		if (Id == 1)        // stone
		{
			Shade = 0.78 + 0.16 * N1 + 0.14 * N2;
			Height = 0.3 + 0.4 * N2 + 0.2 * N1;
			if (H(float3(floor(T * 0.5), Seed + 5.0)) > 0.93) { Shade *= 0.72; Height = 0.1; }
		}
		else if (Id == 2)   // dirt
		{
			Shade = 0.76 + 0.26 * N1 + 0.08 * N2;
			Height = 0.35 + 0.3 * N1;
			if (N1 > 0.95) { Shade = 1.15; Height = 0.9; }
		}
		else if (Id == 3)   // grass: green on top, a ragged green fringe over dirt on the sides
		{
			float3 Dirt = VC * float3(1.55, 0.78, 0.62);
			float Fringe = 12.0 - floor(H(float3(T.x, Cell.x + Cell.y, Seed + 3.0)) * 3.0);
			if (bTop || (!bBottom && L.y >= Fringe)) { Shade = 0.8 + 0.28 * N1 + 0.08 * N2; Height = 0.4 + 0.4 * N1; }
			else { C = Dirt; Shade = 0.76 + 0.26 * N1; Height = 0.35 + 0.3 * N1; }
		}
		else if (Id == 4)   // sand
		{
			Shade = 0.9 + 0.12 * N1 + 0.04 * N2;
			Height = 0.45 + 0.1 * N1;
		}
		else if (Id == 5)   // planks: boards four texels wide, grain along them, nails at the ends
		{
			float Board = floor(L.y / 4.0);
			bool bSeam = fmod(L.y, 4.0) == 0.0;
			float Grain = H(float3(Board, floor((L.x + H(float3(Board, Cell.y, Seed)) * 16.0) / 5.0), Cell.x * 3.0 + Cell.y + Seed));
			Shade = (0.84 + 0.2 * Grain + 0.06 * N1) * (bSeam ? 0.72 : 1.0);
			Height = bSeam ? 0.0 : 0.6 + 0.15 * Grain;
			if (!bSeam && (L.x == 1.0 || L.x == 14.0) && fmod(L.y, 4.0) == 2.0) { Shade = 0.55; Height = 0.95; }
		}
		else if (Id == 6)   // bark: streaks on the sides, rings on the cut ends
		{
			if (Face == 0)
			{
				float R = length(L - 7.5);
				C = VC * float3(1.9, 1.7, 1.4);
				bool bRing = fmod(floor(R * 0.8), 2.0) == 0.0;
				Shade = (bRing ? 0.92 : 1.08) + 0.08 * N1;
				Height = bRing ? 0.4 : 0.6;
			}
			else
			{
				float Streak = H(float3(T.x, floor(T.y / 6.0), Seed));
				Shade = 0.72 + 0.36 * Streak + 0.06 * N1;
				Height = Streak;
			}
		}
		else if (Id == 7)   // leaves
		{
			Shade = N1 < 0.14 ? 0.4 : 0.62 + 0.55 * N1;
			Height = N1 < 0.14 ? 0.0 : 0.5 + 0.5 * N1;
		}
		else if (Id == 8)   // concrete: faint mottling, a seam at each block's edge
		{
			Shade = 0.9 + 0.06 * N1 + 0.06 * N2;
			Height = 0.55 + 0.1 * N2;
			if (bEdge) { Shade *= 0.82; Height = 0.2; }
			if (H(float3(floor(T / 2.0), Seed + 9.0)) > 0.96) { Shade *= 0.8; Height = 0.25; }
		}
		else if (Id == 9)   // brick: running bond, eight by four, light mortar
		{
			float Row = floor(L.y / 4.0);
			float Offset = fmod(abs(Row + Cell.y), 2.0) * 4.0;
			bool bMortar = fmod(L.y, 4.0) == 3.0 || fmod(L.x + Offset, 8.0) == 7.0;
			if (bMortar) { C = float3(0.42, 0.40, 0.37); Shade = 0.9 + 0.1 * N1; Height = 0.0; }
			else { Shade = 0.8 + 0.22 * H(float3(Row, floor((L.x + Offset) / 8.0), Cell.x * 5.0 + Cell.y + Seed)) + 0.06 * N1; Height = 0.75 + 0.1 * N1; }
		}
		else if (Id == 10)  // metal: brushed, a dark panel edge, rivets
		{
			Shade = 0.86 + 0.08 * H(float3(T.y, Cell.x, Seed)) + 0.03 * N1;
			Height = 0.5;
			if (bEdge) { Shade *= 0.72; Height = 0.2; }
			if ((L.x == 2.0 || L.x == 13.0) && (L.y == 2.0 || L.y == 13.0)) { Shade = 1.25; Height = 1.0; }
		}
		else if (Id == 11)  // ore: the surface colour in flecks through grey stone
		{
			if (H(float3(floor(T / 2.0), Seed + 21.0)) > 0.78) { Shade = 1.1 + 0.15 * N1; Height = 0.8; }
			else { C = float3(0.14, 0.14, 0.14); Shade = 0.78 + 0.16 * N1 + 0.14 * N2; Height = 0.3 + 0.3 * N2; }
		}
		else if (Id == 12)  // farmland: furrows
		{
			bool bFurrow = fmod(L.x, 4.0) < 2.0;
			Shade = (bFurrow ? 0.72 : 1.0) * (0.85 + 0.2 * N1);
			Height = bFurrow ? 0.1 : 0.8;
		}
		else if (Id == 13)  // cloth: a weave
		{
			bool bOver = fmod(T.x + T.y, 2.0) == 0.0;
			Shade = (bOver ? 0.9 : 1.06) * (0.93 + 0.08 * N2);
			Height = bOver ? 0.35 : 0.6;
		}
		else if (Id == 14)  // water
		{
			Shade = 0.92 + 0.1 * N2;
			Height = 0.5;
		}
		else if (Id == 15)  // gravel: pebbles two texels across
		{
			float Pebble = H(float3(floor(T / 2.0), Seed + 2.0));
			Shade = 0.6 + 0.6 * Pebble;
			Height = Pebble;
			if (N1 > 0.9) { Shade *= 0.7; Height *= 0.5; }
		}
		return float4(C * Shade, Height);
	}

	/** Smooth value noise over a unit grid, for puddle outlines. */
	float VNoise(float2 P, float S)
	{
		float2 I = floor(P);
		float2 Fr = frac(P);
		Fr = Fr * Fr * (3.0 - 2.0 * Fr);
		float A = H(float3(I, S));
		float B = H(float3(I + float2(1, 0), S));
		float C = H(float3(I + float2(0, 1), S));
		float D = H(float3(I + float2(1, 1), S));
		return lerp(lerp(A, B, Fr.x), lerp(C, D, Fr.x), Fr.y);
	}

	float BaseRough(int Id)
	{
		if (Id == 10) { return 0.42; }
		if (Id == 14) { return 0.08; }
		if (Id == 5 || Id == 6 || Id == 13) { return 0.8; }
		return 0.9;
	}

	float Average(int Id)
	{
		if (Id == 5) { return 0.9; }
		if (Id == 7 || Id == 15) { return 0.88; }
		if (Id == 12) { return 0.82; }
		return 0.95;
	}
};
FMadVoxelPattern F;

""" + PATTERN_ID + """
// Corner occlusion baked by the mesher (UV1.x, 0 open to 1 in a corner): the
// contact shadow Lumen cannot give procedural meshes. Down to half brightness.
float AO = 1.0 - 0.5 * saturate(Occ.x);
float3 NN = normalize(N);
BumpNormal = NN;
Rough = F.BaseRough(Id);

if (Id <= 0)
{
	Rough = lerp(0.9, 0.3, saturate(Wet));
	return VC.rgb * AO * lerp(1.0, 0.65, saturate(Wet));
}

// Project onto the face along its dominant axis.
float3 A = abs(NN);
int Face = 0;
float2 UV = WP.xy;
float Depth = WP.z;
float Facing = NN.z;
float3 AxisU = float3(1, 0, 0);
float3 AxisV = float3(0, 1, 0);
if (A.x > A.z && A.x >= A.y) { Face = 1; UV = WP.yz; Depth = WP.x; Facing = NN.x; AxisU = float3(0, 1, 0); AxisV = float3(0, 0, 1); }
else if (A.y > A.z && A.y > A.x) { Face = 2; UV = WP.xz; Depth = WP.y; Facing = NN.y; AxisU = float3(1, 0, 0); AxisV = float3(0, 0, 1); }
bool bTop = Face == 0 && Facing > 0.0;
bool bBottom = Face == 0 && Facing <= 0.0;

// A centimetre inside the surface, so a face reads its own voxel's layer.
float Layer = floor((Depth - sign(Facing) * 1.0) / 100.0);
float2 T = floor(UV / 100.0 * 16.0);   // world texel
float Seed = Layer * 7.0 + Face * 131.0;

float4 Centre = F.Eval(T, Id, Face, Seed, bTop, bBottom, VC.rgb);
// Texel noise aliases at a distance: fade to the pattern's average, and the
// bump with it (the four extra pattern evaluations stop there too).
float Fade = saturate((Dist - 2500.0) / 4000.0);
float3 C = lerp(Centre.rgb, VC.rgb * F.Average(Id), Fade);

if (Fade < 1.0)
{
	float HL = F.Eval(T - float2(1, 0), Id, Face, Seed, bTop, bBottom, VC.rgb).a;
	float HR = F.Eval(T + float2(1, 0), Id, Face, Seed, bTop, bBottom, VC.rgb).a;
	float HD = F.Eval(T - float2(0, 1), Id, Face, Seed, bTop, bBottom, VC.rgb).a;
	float HU = F.Eval(T + float2(0, 1), Id, Face, Seed, bTop, bBottom, VC.rgb).a;
	float2 Slope = float2(HR - HL, HU - HD) * 0.28;

	// A bevel round every placed block's face (UV1.y, set by the cubic mesher):
	// the edge texels lean outward and catch the light, so a wall reads as
	// blocks with rounded arrises rather than a painted flat. Terrain has none.
	if (Occ.y > 0.5)
	{
		float2 L = T - floor(T / 16.0) * 16.0;
		if (L.x < 1.0) { Slope.x += 0.28; }
		if (L.x > 14.0) { Slope.x -= 0.28; }
		if (L.y < 1.0) { Slope.y += 0.28; }
		if (L.y > 14.0) { Slope.y -= 0.28; }
	}
	BumpNormal = normalize(NN - (Slope.x * AxisU + Slope.y * AxisV) * (1.0 - Fade));
}

// WEATHER (MPC_MadWeather, written by UMadWeatherSubsystem).
float Up = saturate(NN.z);
// Snow settles on whatever faces up, in a grainy edge as it thins out.
float Grain = F.H(float3(T, Seed + 91.0));
float SnowMask = saturate((saturate(Snow) * smoothstep(0.45, 0.85, Up) * 1.35 - Grain * 0.35) * 4.0);
// Rain soaks porous surfaces darker and glossier; metal and water just get wet.
float Porous = (Id == 10 || Id == 14) ? 0.35 : 1.0;
float WetAmt = saturate(Wet) * (1.0 - SnowMask);
C *= lerp(1.0, 0.6, WetAmt * Porous);
Rough = lerp(Rough, 0.22, WetAmt);
// Puddles gather on open, flat natural ground (not placed blocks) once it is wet
// through: dark, mirror-smooth and flat.
if (Occ.y < 0.5 && Up > 0.93 && WetAmt > 0.3)
{
	float Blob = F.VNoise(WP.xy / 260.0, 41.0) * 0.7 + F.VNoise(WP.xy / 90.0, 43.0) * 0.3;
	float Puddle = saturate((Blob - (1.05 - 0.45 * WetAmt)) * 10.0);
	C *= lerp(1.0, 0.5, Puddle);
	Rough = lerp(Rough, 0.03, Puddle);
	BumpNormal = normalize(lerp(BumpNormal, NN, Puddle));
}
C = lerp(C, float3(0.86, 0.88, 0.92) * (0.94 + 0.08 * Grain), SnowMask);
Rough = lerp(Rough, 0.65, SnowMask);
BumpNormal = normalize(lerp(BumpNormal, NN, SnowMask * 0.8));
return saturate(C) * AO;
"""

ROUGHNESS_HLSL = PATTERN_ID + """
if (Id == 10) { return 0.42; }
if (Id == 14) { return 0.08; }
if (Id == 5 || Id == 6 || Id == 13) { return 0.8; }
return 0.9;
"""

METALLIC_HLSL = PATTERN_ID + """
return Id == 10 ? 0.55 : 0.0;
"""


def custom_node(material, editing, code, output_type, inputs, x, y, description, extra_outputs=()):
    node = editing.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property("code", code)
    node.set_editor_property("output_type", output_type)
    node.set_editor_property("description", description)
    custom_inputs = []
    for name in inputs:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        custom_inputs.append(custom_input)
    node.set_editor_property("inputs", custom_inputs)
    outputs = []
    for name, kind in extra_outputs:
        output = unreal.CustomOutput()
        output.set_editor_property("output_name", name)
        output.set_editor_property("output_type", kind)
        outputs.append(output)
    if outputs:
        node.set_editor_property("additional_outputs", outputs)
    return node


def connect(editing, source, source_pin, target, target_pin):
    if not editing.connect_material_expressions(source, source_pin, target, target_pin):
        unreal.log_error("[MadFall] Could not connect {} -> {}".format(source_pin or "output", target_pin))
        return False
    return True


def build(material, editing, held, foliage=False):
    editing.delete_all_material_expressions(material)

    albedo = custom_node(material, editing, ALBEDO_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                         ["WP", "N", "VC", "VA", "Dist", "Occ", "Wet", "Snow"], -450, -100, "MadFall surface pattern",
                         extra_outputs=[("BumpNormal", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
                                        ("Rough", unreal.CustomMaterialOutputType.CMOT_FLOAT1)])
    collection = unreal.EditorAssetLibrary.load_asset("/Game/Materials/MPC_MadWeather")
    wet = editing.create_material_expression(material, unreal.MaterialExpressionCollectionParameter, -900, 460)
    wet.set_editor_property("collection", collection)
    wet.set_editor_property("parameter_name", "Wetness")
    snow = editing.create_material_expression(material, unreal.MaterialExpressionCollectionParameter, -900, 560)
    snow.set_editor_property("collection", collection)
    snow.set_editor_property("parameter_name", "SnowCover")
    roughness = custom_node(material, editing, ROUGHNESS_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT1,
                            ["VA"], -450, 200, "MadFall roughness")
    metallic = custom_node(material, editing, METALLIC_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT1,
                           ["VA"], -450, 320, "MadFall metallic")
    ok = True

    if held:
        # The block in the hand: one voxel in its own space, so the pattern rides
        # with the mesh instead of swimming through world space as the camera
        # moves. The engine cube spans -50..50, so +50 puts it on a voxel's
        # 0..100 grid. Colour and pattern come from parameters (the cube has no
        # vertex colours) and there is no distance fade, the block being at arm's length.
        position = editing.create_material_expression(material, unreal.MaterialExpressionLocalPosition, -1100, -200)
        offset = editing.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -1100, -300)
        offset.set_editor_property("constant", unreal.LinearColor(50.0, 50.0, 50.0, 0.0))
        voxel_position = editing.create_material_expression(material, unreal.MaterialExpressionAdd, -900, -200)
        ok &= connect(editing, position, "", voxel_position, "A")
        ok &= connect(editing, offset, "", voxel_position, "B")
        world_normal = editing.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -1100, -80)
        normal = editing.create_material_expression(material, unreal.MaterialExpressionTransform, -900, -80)
        normal.set_editor_property("transform_source_type", unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
        normal.set_editor_property("transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_LOCAL)
        ok &= connect(editing, world_normal, "", normal, "")
        colour = editing.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -900, 40)
        colour.set_editor_property("parameter_name", "Color")
        colour.set_editor_property("default_value", unreal.LinearColor(0.3, 0.3, 0.3, 1.0))
        pattern = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, 160)
        pattern.set_editor_property("parameter_name", "PatternAlpha")
        pattern.set_editor_property("default_value", 1.0)
        depth = editing.create_material_expression(material, unreal.MaterialExpressionConstant, -900, 260)
        # No corner occlusion, and bevelled like a placed block.
        occlusion = editing.create_material_expression(material, unreal.MaterialExpressionConstant2Vector, -900, 360)
        occlusion.set_editor_property("r", 0.0)
        occlusion.set_editor_property("g", 1.0)
        ok &= connect(editing, voxel_position, "", albedo, "WP")
        # The held pattern works in the cube's own space, so its normal comes back
        # to world space for lighting.
        ok &= connect(editing, normal, "", albedo, "N")
        ok &= connect(editing, colour, "", albedo, "VC")
        ok &= connect(editing, pattern, "", albedo, "VA")
        ok &= connect(editing, depth, "", albedo, "Dist")
        ok &= connect(editing, occlusion, "", albedo, "Occ")
        ok &= connect(editing, pattern, "", roughness, "VA")
        ok &= connect(editing, pattern, "", metallic, "VA")
    else:
        world_position = editing.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -900, -200)
        normal = editing.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -900, -80)
        vertex_color = editing.create_material_expression(material, unreal.MaterialExpressionVertexColor, -900, 40)
        depth = editing.create_material_expression(material, unreal.MaterialExpressionPixelDepth, -900, 200)
        occlusion = editing.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -900, 320)
        occlusion.set_editor_property("coordinate_index", 1)
        ok &= connect(editing, world_position, "", albedo, "WP")
        ok &= connect(editing, normal, "", albedo, "N")
        ok &= connect(editing, vertex_color, "", albedo, "VC")
        ok &= connect(editing, vertex_color, "A", albedo, "VA")
        ok &= connect(editing, depth, "", albedo, "Dist")
        ok &= connect(editing, occlusion, "", albedo, "Occ")
        ok &= connect(editing, vertex_color, "A", roughness, "VA")
        ok &= connect(editing, vertex_color, "A", metallic, "VA")

    if held:
        # Model blocks in the world (barrels, bushes) take the weather; the block
        # in the survivor's hand sets Weather to 0 and stays dry.
        exposure = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, 660)
        exposure.set_editor_property("parameter_name", "Weather")
        exposure.set_editor_property("default_value", 1.0)
        wet_scaled = editing.create_material_expression(material, unreal.MaterialExpressionMultiply, -700, 460)
        snow_scaled = editing.create_material_expression(material, unreal.MaterialExpressionMultiply, -700, 560)
        ok &= connect(editing, wet, "", wet_scaled, "A")
        ok &= connect(editing, exposure, "", wet_scaled, "B")
        ok &= connect(editing, snow, "", snow_scaled, "A")
        ok &= connect(editing, exposure, "", snow_scaled, "B")
        ok &= connect(editing, wet_scaled, "", albedo, "Wet")
        ok &= connect(editing, snow_scaled, "", albedo, "Snow")
    else:
        ok &= connect(editing, wet, "", albedo, "Wet")
        ok &= connect(editing, snow, "", albedo, "Snow")
    ok &= editing.connect_material_property(albedo, "", unreal.MaterialProperty.MP_BASE_COLOR)
    if held:
        to_world = editing.create_material_expression(material, unreal.MaterialExpressionTransform, -150, -220)
        to_world.set_editor_property("transform_source_type", unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL)
        to_world.set_editor_property("transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
        ok &= connect(editing, albedo, "BumpNormal", to_world, "")
        ok &= editing.connect_material_property(to_world, "", unreal.MaterialProperty.MP_NORMAL)
    else:
        ok &= editing.connect_material_property(albedo, "BumpNormal", unreal.MaterialProperty.MP_NORMAL)
    # The pattern's normal is in world space (held: brought back to it).
    material.set_editor_property("tangent_space_normal", False)
    # Roughness now comes from the pattern node, which knows about wet and snow.
    ok &= editing.connect_material_property(albedo, "Rough", unreal.MaterialProperty.MP_ROUGHNESS)
    ok &= editing.connect_material_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)
    if foliage:
        # Two-sided foliage transmits light arriving at the back of a face, so
        # the underside of a canopy glows green where the sun is on its top
        # instead of going black: chunk meshes are invisible to Lumen's
        # software tracing off screen, so there is little bounce to fill it.
        # Cost: the transmission term, and leaves draw as their own section.
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
        transmission = editing.create_material_expression(material, unreal.MaterialExpressionMultiply, -150, 60)
        transmission.set_editor_property("const_b", 0.6)
        ok &= connect(editing, albedo, "", transmission, "A")
        ok &= editing.connect_material_property(transmission, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    if not ok or editing.get_material_property_input_node(material, unreal.MaterialProperty.MP_BASE_COLOR) is None:
        unreal.log_error("[MadFall] {} is incompletely connected".format(material.get_name()))
        return False

    # Chunk meshes only ever emit outward faces: single-sided makes a bad winding
    # show up as a hole instead of being silently hidden.
    material.set_editor_property("two_sided", False)
    # The held variant also draws model blocks (UMadModelInstanceSubsystem), which
    # are instanced: a cooked build cannot compile that usage on demand.
    material.set_editor_property("used_with_instanced_static_meshes", True)
    return True


def make(editing, library, asset_name, held, foliage=False):
    full_path = "{}/{}".format(PACKAGE_PATH, asset_name)
    if library.does_asset_exist(full_path):
        material = library.load_asset(full_path)
        if library.get_metadata_tag(material, VERSION_TAG) == MATERIAL_VERSION:
            unreal.log("[MadFall] {} is already version {}.".format(full_path, MATERIAL_VERSION))
            return True
        unreal.log("[MadFall] Rebuilding {} as version {}.".format(full_path, MATERIAL_VERSION))
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        material = tools.create_asset(asset_name, PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew())
        if material is None:
            unreal.log_error("[MadFall] Could not create {}".format(full_path))
            return False

    if not build(material, editing, held, foliage):
        return False

    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return True


def main():
    editing = unreal.MaterialEditingLibrary
    library = unreal.EditorAssetLibrary
    ok = make(editing, library, ASSET_NAME, held=False)
    ok &= make(editing, library, HELD_ASSET_NAME, held=True)
    ok &= make(editing, library, FOLIAGE_ASSET_NAME, held=False, foliage=True)
    return 0 if ok else 1


sys.exit(main())

# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadVoxelPBR - the photo-textured voxel material.
# Each surface with a texture set gets an instance (import_surface_textures.py)
# and names it as its "material"; surfaces without one keep M_MadVoxel's
# procedural patterns.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_pbr_material.py"
#
# WHAT IT DOES
# - Projects colour, normal and roughness maps onto each face along its
#   dominant axis in world space, one repeat every TileVoxels voxels, exactly
#   like the procedural patterns: no UVs, seamless across chunks and greedy
#   merged quads, the same on smooth terrain.
# - Grass-like sets (UseSides) put a second set on the sides and underside
#   (dirt under a grass top), with a ragged fringe of the top set down the
#   upper edge of each side.
# - Tint pulls the texture toward the surface's own colour (vertex RGB), so one
#   rock scan serves granite and bedrock, and mods can recolour a set.
# - Keeps everything the procedural material does around the texture: corner
#   occlusion (UV1.x), a bevel on placed blocks (UV1.y), large-scale brightness
#   variation against tiling, and the weather in MPC_MadWeather (wet darkening
#   and gloss, puddles, snow).
#
# COSTS, STATED
# Three texture samples a pixel (six on a grass side), plus the procedural
# weather math. A dominant-axis projection steps at slope changes on smooth
# terrain - the price of one sample set instead of three blended ones.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadVoxelPBR"
VERSION_TAG = "MadFallPBRVersion"
MATERIAL_VERSION = "5"

PBR_HLSL = """
struct FMadPBR
{
	float H(float3 p)
	{
		p = frac(p * float3(0.1031, 0.1030, 0.0973));
		p += dot(p, p.yzx + 33.33);
		return frac((p.x + p.y) * p.z);
	}
	float VNoise(float2 P, float S)
	{
		float2 I = floor(P);
		float2 Fr = frac(P);
		Fr = Fr * Fr * (3.0 - 2.0 * Fr);
		return lerp(lerp(H(float3(I, S)), H(float3(I + float2(1, 0), S)), Fr.x),
			lerp(H(float3(I + float2(0, 1), S)), H(float3(I + float2(1, 1), S)), Fr.x), Fr.y);
	}
};
FMadPBR F;

float3 NN = normalize(N);
float3 A = abs(NN);
// Face frame: U across, V up the face (or along +Y on horizontal faces), N out.
float3 AxisU = float3(1, 0, 0);
float3 AxisV = float3(0, sign(NN.z + 1e-4), 0);
bool bSide = false;
if (A.x > A.z && A.x >= A.y) { AxisU = float3(0, sign(NN.x), 0); AxisV = float3(0, 0, 1); bSide = true; }
else if (A.y > A.z && A.y > A.x) { AxisU = float3(-sign(NN.y), 0, 0); AxisV = float3(0, 0, 1); bSide = true; }
bool bUnder = !bSide && NN.z < 0.0;

float Tile = max(TileVoxels, 0.25) * 100.0;
float2 UV = float2(dot(WP, AxisU), -dot(WP, AxisV)) / Tile;

// Grass-like sets: the side set below a ragged fringe of the top set.
bool bUseSide = UseSides > 0.5 && (bSide || bUnder);
if (bUseSide && bSide)
{
	float InVoxel = frac(WP.z / 100.0);
	float Ragged = 0.8 + 0.08 * F.H(float3(floor(dot(WP, AxisU) / 6.25), floor(WP.z / 100.0), 5.0));
	if (InVoxel > Ragged) { bUseSide = false; }
}

// Ray tracing hit shaders have no screen derivatives, so implicit-mip Sample
// does not compile there; they read mip 0 (reflections are low-detail anyway).
#if RAYHITGROUPSHADER
#define MAD_SAMPLE(Tex, UVs) Tex.SampleLevel(Tex##Sampler, UVs, 0)
#else
#define MAD_SAMPLE(Tex, UVs) Tex.Sample(Tex##Sampler, UVs)
#endif
float3 Col = bUseSide ? MAD_SAMPLE(SideBaseColor, UV).rgb : MAD_SAMPLE(BaseColor, UV).rgb;
float3 TN = bUseSide ? MAD_SAMPLE(SideNormal, UV).rgb : MAD_SAMPLE(Normal, UV).rgb;
Rough = bUseSide ? MAD_SAMPLE(SideRoughness, UV).r : MAD_SAMPLE(Roughness, UV).r;
#undef MAD_SAMPLE
// Normal maps import as BC5 (TC_Normalmap), which keeps only X and Y; the
// engine's own sampler node rebuilds Z, a raw Sample here does not. Left at the
// stored 0, every normal pointed into the surface and lit ground rendered black.
TN.xy = TN.xy * 2.0 - 1.0;
TN.z = sqrt(saturate(1.0 - dot(TN.xy, TN.xy)));

// Tint toward the surface colour without darkening: the hue of VC at the texture's brightness.
float Mean = max((VC.r + VC.g + VC.b) / 3.0, 0.02);
Col *= lerp(float3(1, 1, 1), VC.rgb / Mean, saturate(Tint));
// Large, slow brightness variation so a repeating scan does not read as tiles.
Col *= 0.88 + 0.24 * F.VNoise(WP.xy / 1700.0, 7.0);

float3 Tangent = AxisU;
float3 Bitangent = -AxisV;   // texture V runs down the image
float3 WN = normalize(TN.x * Tangent + TN.y * Bitangent + TN.z * NN);

// A bevel on placed blocks: the outer 3% of each voxel face leans outward. Kept
// narrow and soft on photo textures, which carry their own relief: at 6% and
// 0.35 every tree trunk (cubic blocks) showed a dark band at each block joint.
if (Occ.y > 0.5)
{
	float LU = frac(dot(WP, AxisU) / 100.0);
	float LV = frac(dot(WP, AxisV) / 100.0);
	float3 Lean = float3(0, 0, 0);
	if (LU < 0.03) { Lean -= AxisU; }
	if (LU > 0.97) { Lean += AxisU; }
	if (LV < 0.03) { Lean -= AxisV; }
	if (LV > 0.97) { Lean += AxisV; }
	WN = normalize(WN + Lean * 0.15);
}

// Weather (MPC_MadWeather).
float Up = saturate(NN.z);
float Grain = F.H(float3(floor(UV * 256.0), 91.0));
float SnowMask = saturate((saturate(Snow) * smoothstep(0.45, 0.85, Up) * 1.35 - Grain * 0.35) * 4.0);
float WetAmt = saturate(Wet) * (1.0 - SnowMask);
Col *= lerp(1.0, 0.62, WetAmt * (1.0 - Metallic));
Rough = lerp(Rough, min(Rough, 0.2), WetAmt);
if (Occ.y < 0.5 && Up > 0.93 && WetAmt > 0.3)
{
	float Blob = F.VNoise(WP.xy / 260.0, 41.0) * 0.7 + F.VNoise(WP.xy / 90.0, 43.0) * 0.3;
	float Puddle = saturate((Blob - (1.05 - 0.45 * WetAmt)) * 10.0);
	Col *= lerp(1.0, 0.5, Puddle);
	Rough = lerp(Rough, 0.03, Puddle);
	WN = normalize(lerp(WN, NN, Puddle));
}
Col = lerp(Col, float3(0.86, 0.88, 0.92) * (0.94 + 0.08 * Grain), SnowMask);
Rough = lerp(Rough, 0.65, SnowMask);
WN = normalize(lerp(WN, NN, SnowMask * 0.8));

BumpNormal = WN;
return saturate(Col) * (1.0 - 0.5 * saturate(Occ.x));
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
    y = [-600]

    def place():
        y[0] += 110
        return y[0]

    inputs = {}
    inputs["WP"] = editing.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -1100, place())
    inputs["N"] = editing.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -1100, place())
    inputs["VC"] = editing.create_material_expression(material, unreal.MaterialExpressionVertexColor, -1100, place())
    occlusion = editing.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -1100, place())
    occlusion.set_editor_property("coordinate_index", 1)
    inputs["Occ"] = occlusion

    collection = library.load_asset("/Game/Materials/MPC_MadWeather")
    for pin, name in (("Wet", "Wetness"), ("Snow", "SnowCover")):
        node = editing.create_material_expression(material, unreal.MaterialExpressionCollectionParameter, -1100, place())
        node.set_editor_property("collection", collection)
        node.set_editor_property("parameter_name", name)
        inputs[pin] = node

    default_texture = library.load_asset("/Engine/EngineMaterials/DefaultDiffuse")
    default_normal = library.load_asset("/Engine/EngineMaterials/FlatNormal")
    default_grey = library.load_asset("/Engine/EngineMaterials/DefaultDiffuse_TC_Masks")
    for name, default, sampler in (
            ("BaseColor", default_texture, unreal.MaterialSamplerType.SAMPLERTYPE_COLOR),
            ("Normal", default_normal, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL),
            ("Roughness", default_grey, unreal.MaterialSamplerType.SAMPLERTYPE_MASKS),
            ("SideBaseColor", default_texture, unreal.MaterialSamplerType.SAMPLERTYPE_COLOR),
            ("SideNormal", default_normal, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL),
            ("SideRoughness", default_grey, unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)):
        node = editing.create_material_expression(material, unreal.MaterialExpressionTextureObjectParameter, -1100, place())
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("texture", default)
        node.set_editor_property("sampler_type", sampler)
        inputs[name] = node

    for name, default in (("TileVoxels", 2.0), ("Tint", 0.15), ("UseSides", 0.0), ("Metallic", 0.0)):
        node = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -1100, place())
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", default)
        inputs[name] = node

    pbr = editing.create_material_expression(material, unreal.MaterialExpressionCustom, -500, 0)
    pbr.set_editor_property("code", PBR_HLSL)
    pbr.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    pbr.set_editor_property("description", "MadFall textured surface")
    custom_inputs = []
    for name in inputs:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        custom_inputs.append(custom_input)
    pbr.set_editor_property("inputs", custom_inputs)
    outputs = []
    for name, kind in (("BumpNormal", unreal.CustomMaterialOutputType.CMOT_FLOAT3), ("Rough", unreal.CustomMaterialOutputType.CMOT_FLOAT1)):
        output = unreal.CustomOutput()
        output.set_editor_property("output_name", name)
        output.set_editor_property("output_type", kind)
        outputs.append(output)
    pbr.set_editor_property("additional_outputs", outputs)
    for name, node in inputs.items():
        ok &= editing.connect_material_expressions(node, "", pbr, name)

    ok &= editing.connect_material_property(pbr, "", unreal.MaterialProperty.MP_BASE_COLOR)
    ok &= editing.connect_material_property(pbr, "BumpNormal", unreal.MaterialProperty.MP_NORMAL)
    ok &= editing.connect_material_property(pbr, "Rough", unreal.MaterialProperty.MP_ROUGHNESS)
    ok &= editing.connect_material_property(inputs["Metallic"], "", unreal.MaterialProperty.MP_METALLIC)
    if not ok:
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return 1

    material.set_editor_property("tangent_space_normal", False)
    material.set_editor_property("two_sided", False)
    material.set_editor_property("used_with_instanced_static_meshes", True)
    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return 0


sys.exit(main())

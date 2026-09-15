# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/M_MadNightSky - the night sky UMadSkySubsystem draws
# on a sphere around the camera: a deep blue that lightens toward the horizon,
# and stars.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_sky_material.py"
#
# WHY A DOME AND NOT THE ATMOSPHERE
# The sky atmosphere only scatters its lights. With the sun down and the moon
# kept out of it (a moon in the atmosphere turned midnight a daytime blue once
# auto exposure lifted it), nothing lit the sky and night was a black ceiling
# over moonlit ground. The dome is marked as sky, so the atmosphere still
# composites over it by day.
#
# PARAMETERS
# - Night (0..1): the subsystem fades it in as the sun goes down.
# - Brightness: overall emissive scale, tuned against night exposure.
#
# Stars are a hash per cell of a cube-mapped direction, so there are no
# textures, and they stay fixed in the sky as the camera turns. Cost: an
# unlit full-screen sky, a few hash evaluations a pixel.

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "M_MadNightSky"
VERSION_TAG = "MadFallNightSkyVersion"
MATERIAL_VERSION = "1"

SKY_HLSL = """
struct FMadSky
{
	float H(float3 p)
	{
		p = frac(p * float3(0.1031, 0.1030, 0.0973));
		p += dot(p, p.yzx + 33.33);
		return frac((p.x + p.y) * p.z);
	}
};
FMadSky S;

float3 D = normalize(Dir);
float Up = saturate(D.z);

// Deep blue overhead, lighter and greyer toward the horizon, black below it.
float3 Zenith = float3(0.004, 0.008, 0.028);
float3 Horizon = float3(0.020, 0.030, 0.055);
float3 C = lerp(Horizon, Zenith, pow(Up, 0.6));
C *= saturate(D.z * 8.0 + 1.0);

// Stars: the direction on a cube face, split into cells, one candidate star a cell.
float3 A = abs(D);
float2 Face = A.z >= A.x && A.z >= A.y ? D.xy / A.z : (A.x >= A.y ? D.yz / A.x : D.xz / A.y);
float FaceId = A.z >= A.x && A.z >= A.y ? (D.z > 0 ? 1.0 : 2.0) : (A.x >= A.y ? (D.x > 0 ? 3.0 : 4.0) : (D.y > 0 ? 5.0 : 6.0));
float Cells = 180.0;
float2 Cell = floor(Face * Cells);
float2 InCell = frac(Face * Cells) - 0.5;
float Pick = S.H(float3(Cell, FaceId * 17.0));
if (Pick > 0.985 && D.z > 0.02)
{
	float2 Offset = float2(S.H(float3(Cell, FaceId + 3.0)), S.H(float3(Cell, FaceId + 7.0))) - 0.5;
	float R = length(InCell - Offset * 0.6);
	float Size = 0.08 + 0.12 * S.H(float3(Cell, FaceId + 11.0));
	float Star = saturate(1.0 - R / Size);
	float Warmth = S.H(float3(Cell, FaceId + 13.0));
	float3 Tint = lerp(float3(0.75, 0.85, 1.0), float3(1.0, 0.9, 0.75), Warmth);
	C += Tint * Star * Star * (0.25 + 0.75 * (Pick - 0.985) / 0.015) * saturate(D.z * 6.0);
}

return C * Night * Brightness;
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
    material.set_editor_property("two_sided", True)
    material.set_editor_property("is_sky", True)

    position = editing.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -900, -100)
    camera = editing.create_material_expression(material, unreal.MaterialExpressionCameraPositionWS, -900, 40)
    direction = editing.create_material_expression(material, unreal.MaterialExpressionSubtract, -700, -40)
    night = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, 120)
    night.set_editor_property("parameter_name", "Night")
    night.set_editor_property("default_value", 1.0)
    brightness = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, 220)
    brightness.set_editor_property("parameter_name", "Brightness")
    brightness.set_editor_property("default_value", 1.0)

    sky = editing.create_material_expression(material, unreal.MaterialExpressionCustom, -400, 0)
    sky.set_editor_property("code", SKY_HLSL)
    sky.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    sky.set_editor_property("description", "MadFall night sky")
    inputs = []
    for name in ["Dir", "Night", "Brightness"]:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    sky.set_editor_property("inputs", inputs)

    ok = True
    ok &= editing.connect_material_expressions(position, "", direction, "A")
    ok &= editing.connect_material_expressions(camera, "", direction, "B")
    ok &= editing.connect_material_expressions(direction, "", sky, "Dir")
    ok &= editing.connect_material_expressions(night, "", sky, "Night")
    ok &= editing.connect_material_expressions(brightness, "", sky, "Brightness")
    ok &= editing.connect_material_property(sky, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if not ok:
        unreal.log_error("[MadFall] {} is incompletely connected".format(full_path))
        return 1

    editing.recompile_material(material)
    library.set_metadata_tag(material, VERSION_TAG, MATERIAL_VERSION)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} version {}".format(full_path, MATERIAL_VERSION))
    return 0


sys.exit(main())

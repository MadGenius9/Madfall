# Copyright MadFall. All Rights Reserved.
#
# Builds Content/Materials/MPC_MadWeather - the material parameter collection
# UMadWeatherSubsystem writes and the world's materials read, so weather shows
# on every surface at once without touching a material instance:
#   Wetness    0 dry .. 1 soaked: darker, glossier surfaces; puddles on open ground
#   SnowCover  0 bare .. 1 deep: snow settling on everything that faces up
#   Wind       0 still .. 1 gale: how hard grass sways
#
# Run it before make_voxel_material.py and make_cover_material.py, which
# reference the collection.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_weather_collection.py"

import sys

import unreal

PACKAGE_PATH = "/Game/Materials"
ASSET_NAME = "MPC_MadWeather"
PARAMETERS = [("Wetness", 0.0), ("SnowCover", 0.0), ("Wind", 0.2)]


def main():
    library = unreal.EditorAssetLibrary
    full_path = "{}/{}".format(PACKAGE_PATH, ASSET_NAME)
    if library.does_asset_exist(full_path):
        collection = library.load_asset(full_path)
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        collection = tools.create_asset(ASSET_NAME, PACKAGE_PATH, unreal.MaterialParameterCollection, unreal.MaterialParameterCollectionFactoryNew())
        if collection is None:
            unreal.log_error("[MadFall] Could not create {}".format(full_path))
            return 1

    existing = [p.get_editor_property("parameter_name") for p in collection.get_editor_property("scalar_parameters")]
    scalars = list(collection.get_editor_property("scalar_parameters"))
    for name, default in PARAMETERS:
        if unreal.Name(name) in existing or name in [str(e) for e in existing]:
            continue
        parameter = unreal.CollectionScalarParameter()
        parameter.set_editor_property("parameter_name", name)
        parameter.set_editor_property("default_value", default)
        scalars.append(parameter)
    collection.set_editor_property("scalar_parameters", scalars)
    library.save_asset(full_path, only_if_is_dirty=False)
    unreal.log("[MadFall] Built {} with {}".format(full_path, ", ".join(str(p.get_editor_property("parameter_name")) for p in scalars)))
    return 0


sys.exit(main())

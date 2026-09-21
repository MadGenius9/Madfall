# Copyright MadFall. All Rights Reserved.
#
# Imports the animated animal models (Quaternius, Ultimate Animated Animal
# Pack, CC0) that animal definitions draw through their "model".
#
#   1. Scripts/fetch_animals.ps1 downloads them into SourceArt/animals/, and
#      python Scripts/prepare_animals.py normalises them into SourceArt/animals/prepared/
#   2. UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/import_animals.py"
#
# Each <Name>.glb lands in /Game/Animals/<Name>/: the skeletal mesh, skeleton,
# physics asset, flat-coloured materials and one animation sequence per clip.
# The script logs every asset it produced, the mesh bounds and each clip's
# length, which is what a definition's model block is written from.

import os
import sys

import unreal

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SOURCE = os.path.join(PROJECT, "SourceArt", "animals", "prepared")
DEST = "/Game/Animals"
ANIMALS = ["Deer", "Wolf", "Fox", "Stag", "Donkey", "Snake", "Crab"]


def main():
    library = unreal.EditorAssetLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    failures = 0
    # MADFALL_ANIMALS=Donkey,Snake imports just those, leaving the committed
    # assets of the rest untouched (a reimport rewrites every .uasset).
    only = [n.strip() for n in os.environ.get("MADFALL_ANIMALS", "").split(",") if n.strip()]
    names = [n for n in ANIMALS if not only or n in only]
    for name in names:
        path = os.path.join(SOURCE, name + ".glb")
        if not os.path.isfile(path):
            unreal.log_error("[MadFall] {} is not downloaded: {}".format(name, path))
            failures += 1
            continue
        destination = "{}/{}".format(DEST, name)
        if library.does_directory_exist(destination):
            library.delete_directory(destination)

        task = unreal.AssetImportTask()
        task.set_editor_property("filename", path)
        task.set_editor_property("destination_path", destination)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("automated", True)
        task.set_editor_property("save", True)
        tools.import_asset_tasks([task])

        # The animal pack's glTF files carry every clip twice, once as "<Clip>" and
        # once as "AnimalArmature|<Clip>" (the Blender action and its NLA copy);
        # keep one. The monster-rig models (snake, crab) carry only the prefixed
        # copy, so a prefixed clip goes only when its plain twin exists.
        imported = [a.split(".")[0] for a in library.list_assets(destination, recursive=True)]
        for asset_path in imported:
            folder, _, leaf = asset_path.rpartition("/")
            if "Armature_" in leaf:
                # "DonkeyAnimalArmature_Walk" is the twin of "DonkeyWalk".
                twin = folder + "/" + name + leaf.split("Armature_", 1)[1]
                if twin in imported:
                    library.delete_asset(asset_path)

        assets = library.list_assets(destination, recursive=True)
        if not assets:
            unreal.log_error("[MadFall] Importing {} produced nothing".format(path))
            failures += 1
            continue
        for asset_path in assets:
            asset = library.load_asset(asset_path.split(".")[0])
            detail = ""
            if isinstance(asset, unreal.SkeletalMesh):
                bounds = asset.get_bounds()
                detail = "bounds origin {} extent {}".format(bounds.origin, bounds.box_extent)
            elif isinstance(asset, unreal.AnimSequence):
                detail = "{:.2f} s".format(asset.get_play_length())
            unreal.log("[MadFall] {} {} {}".format(asset.get_class().get_name(), asset_path.split(".")[0], detail))
            library.save_asset(asset_path.split(".")[0], only_if_is_dirty=False)

    unreal.log("[MadFall] Animals: {} imported, {} failed".format(len(names) - failures, failures))
    return 0 if failures == 0 else 1


sys.exit(main())

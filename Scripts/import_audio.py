# Copyright MadFall. All Rights Reserved.
#
# Imports SourceArt/audio/prepared/<sound>/<sound>_NN.wav (Scripts/prepare_audio.py)
# into /Game/Audio/<sound>/SW_<sound>_NN, where UMadAudioSubsystem finds them by
# folder name. A folder's previous assets are deleted first, so a recording
# dropped from the mapping does not linger.
#
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/import_audio.py"

import os
import sys

import unreal

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SOURCE = os.path.join(PROJECT, "SourceArt", "audio", "prepared")
DEST = "/Game/Audio"
LOOPS = {"rain_loop", "wind_loop"}


def main():
    library = unreal.EditorAssetLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    if not os.path.isdir(SOURCE):
        unreal.log_error("[MadFall] No prepared audio at {} - run Scripts/prepare_audio.py".format(SOURCE))
        return 1

    imported = 0
    failures = 0
    # MADFALL_SOUNDS=step_dirt,step_foliage reimports just those (and no music):
    # a full import rewrites every .uasset even where nothing changed.
    only = [s.strip() for s in os.environ.get("MADFALL_SOUNDS", "").split(",") if s.strip()]
    for sound in sorted(os.listdir(SOURCE)):
        folder = os.path.join(SOURCE, sound)
        if not os.path.isdir(folder) or (only and sound not in only):
            continue
        destination = "{}/{}".format(DEST, sound)
        if library.does_directory_exist(destination):
            for existing in library.list_assets(destination, recursive=False):
                library.delete_asset(existing.split(".")[0])

        for file_name in sorted(os.listdir(folder)):
            if not file_name.lower().endswith(".wav"):
                continue
            name = "SW_" + os.path.splitext(file_name)[0]
            task = unreal.AssetImportTask()
            task.set_editor_property("filename", os.path.join(folder, file_name))
            task.set_editor_property("destination_path", destination)
            task.set_editor_property("destination_name", name)
            task.set_editor_property("replace_existing", True)
            task.set_editor_property("automated", True)
            task.set_editor_property("save", False)
            tools.import_asset_tasks([task])
            wave = library.load_asset("{}/{}".format(destination, name))
            if not isinstance(wave, unreal.SoundWave):
                unreal.log_error("[MadFall] Could not import {}".format(file_name))
                failures += 1
                continue
            wave.set_editor_property("looping", sound in LOOPS)
            library.save_asset("{}/{}".format(destination, name), only_if_is_dirty=False)
            imported += 1
        # The delete above can fail silently (a loaded asset): when step_dirt
        # went from ten recordings to five, the old 06-10 survived and half the
        # footsteps kept the sound being replaced. Say so rather than ship it.
        wavs = {"SW_" + os.path.splitext(f)[0] for f in os.listdir(folder) if f.lower().endswith(".wav")}
        stale = [a for a in library.list_assets(destination, recursive=False) if a.split(".")[-1] not in wavs]
        if stale:
            unreal.log_error("[MadFall] {}: stale recording(s) left behind, delete them: {}".format(sound, stale))
            failures += len(stale)
        unreal.log("[MadFall] {}: {} recording(s)".format(sound, len(library.list_assets(destination, recursive=False))))

    # Music: /Game/Music/SW_Music_<track>, played by UMadMusicSubsystem. Only the
    # horde track loops; day and night tracks play through and leave a silence.
    music_source = os.path.join(PROJECT, "SourceArt", "audio", "prepared_music")
    if os.path.isdir(music_source) and not only:
        for file_name in sorted(os.listdir(music_source)):
            if not file_name.lower().endswith(".wav"):
                continue
            name = "SW_" + os.path.splitext(file_name)[0]
            task = unreal.AssetImportTask()
            task.set_editor_property("filename", os.path.join(music_source, file_name))
            task.set_editor_property("destination_path", "/Game/Music")
            task.set_editor_property("destination_name", name)
            task.set_editor_property("replace_existing", True)
            task.set_editor_property("automated", True)
            task.set_editor_property("save", False)
            tools.import_asset_tasks([task])
            wave = library.load_asset("/Game/Music/" + name)
            if not isinstance(wave, unreal.SoundWave):
                unreal.log_error("[MadFall] Could not import {}".format(file_name))
                failures += 1
                continue
            wave.set_editor_property("looping", "horde" in name)
            library.save_asset("/Game/Music/" + name, only_if_is_dirty=False)
            imported += 1
            unreal.log("[MadFall] music {}: {:.0f} s".format(name, wave.get_editor_property("duration")))

    unreal.log("[MadFall] Audio: {} imported, {} failed".format(imported, failures))
    return 0 if failures == 0 else 1


sys.exit(main())

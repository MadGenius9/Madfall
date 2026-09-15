# Copyright MadFall. All Rights Reserved.
#
# Creates Content/Maps/L_MadFall_Test.umap - the Phase 0 "empty test map".
#
# Run headless via:
#   UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="Scripts/make_test_map.py"
#
# Idempotent: if the map already exists the script does nothing and exits 0, so
# it is safe to call from CI on every run.
#
# WHY A SCRIPT AND NOT A COMMITTED .umap:
# A .umap is an opaque binary in Git LFS. The starting map for the project is
# scaffolding, not art, and regenerating it from 40 lines of readable Python
# beats reviewing a binary diff every time someone nudges the sun angle.

import sys

import unreal

MAP_PACKAGE = "/Game/Maps/L_MadFall_Test"


def log(message):
    unreal.log("[MadFall] {}".format(message))


def main():
    if unreal.EditorAssetLibrary.does_asset_exist(MAP_PACKAGE):
        log("{} already exists - nothing to do.".format(MAP_PACKAGE))
        return 0

    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_editor.new_level(MAP_PACKAGE):
        unreal.log_error("[MadFall] Failed to create level {}".format(MAP_PACKAGE))
        return 1

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    def spawn(actor_class, location, rotation, label):
        actor = actor_subsystem.spawn_actor_from_class(
            actor_class, unreal.Vector(*location), unreal.Rotator(*rotation)
        )
        if actor is None:
            unreal.log_error("[MadFall] Failed to spawn {}".format(label))
            return None
        actor.set_actor_label(label)
        return actor

    # Deliberately minimal. Enough lighting to see that the map opened, and a
    # PlayerStart so PIE works - nothing that would later be mistaken for
    # authored content. Terrain arrives in Phase 3.
    sun = spawn(unreal.DirectionalLight, (0.0, 0.0, 1000.0), (-42.0, -60.0, 0.0), "Sun")
    if sun is not None:
        # Movable: the day cycle (UMadSkySubsystem) rotates it at runtime.
        sun.get_editor_property("light_component").set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    sky_light = spawn(unreal.SkyLight, (0.0, 0.0, 1000.0), (0.0, 0.0, 0.0), "SkyLight")
    spawn(unreal.SkyAtmosphere, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "SkyAtmosphere")
    spawn(unreal.ExponentialHeightFog, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "HeightFog")
    spawn(unreal.VolumetricCloud, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "VolumetricCloud")
    spawn(unreal.PlayerStart, (0.0, 0.0, 200.0), (0.0, 0.0, 0.0), "PlayerStart")

    # A SkyLight with no captured cubemap contributes nothing, which leaves
    # every surface facing away from the sun pure black. Real-time capture also
    # matters because voxel terrain appears and changes at runtime - a cubemap
    # baked at map-save time would have captured an empty world.
    if sky_light is not None:
        component = sky_light.get_editor_property("light_component")
        component.set_editor_property("real_time_capture", True)
        component.set_editor_property("intensity", 1.0)
        component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)

    if not level_editor.save_current_level():
        unreal.log_error("[MadFall] Failed to save {}".format(MAP_PACKAGE))
        return 1

    log("Created and saved {}".format(MAP_PACKAGE))
    return 0


sys.exit(main())

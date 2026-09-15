# Copyright MadFall. All Rights Reserved.
#
# Turns the downloaded CC0 sound packs into one folder of WAVs per game sound,
# ready for Scripts/import_audio.py.
#
#   1. Scripts/fetch_audio.ps1 downloads the packs into SourceArt/audio/
#   2. python Scripts/prepare_audio.py        (needs ffmpeg on PATH)
#   3. UnrealEditor-Cmd.exe MadFall.uproject -ExecutePythonScript="<abs>/Scripts/import_audio.py"
#
# Output: SourceArt/audio/prepared/<sound name>/<sound name>_NN.wav, mono,
# 44.1 kHz, 16-bit. The folder names are EMadSound's names (MadFall::Synth::
# GetName); UMadAudioSubsystem plays a sound's recordings when it has any and
# its synthesised voice otherwise, so a sound missing here still makes a noise.
#
# WHY PEAK-NORMALISED TO -2 dBFS: the synthesised sounds are normalised to 80%
# of full scale, and every volume in game code was tuned against them. Matching
# that keeps the mix: a recorded footstep is as loud as the synthesised one it
# replaces. Silence is trimmed from both ends of one-shots (a late start reads
# as lag on a pick swing); loops are left whole so they stay seamless.
#
# MUSIC goes to SourceArt/audio/prepared_music/Music_<track>.wav for
# /Game/Music (UMadMusicSubsystem): stereo at 32 kHz, loudness-normalised to
# -20 LUFS so it sits under the effects, and the long night track cut to two
# and a half minutes with a fade at each end. Three tracks at full length and
# rate were 95 MB of PCM for a repository with a 1 GB LFS allowance; ambient
# music loses nothing audible at 32 kHz, and the cut loops as well as the whole.
#
# Sources (all CC0):
#   Kenney Impact Sounds, Kenney RPG Audio - https://kenney.nl
#   Zombies Sound Pack (Summoning Wars), 100 CC0 SFX #2, 30 CC0 SFX loops,
#   Wind Whoosh Loop - https://opengameart.org
#   Music: EmptyCity and Zombies' March by yd, Cold Silence by Eponasoft -
#   https://opengameart.org

import os
import shutil
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SourceArt", "audio")
OUT = os.path.join(ROOT, "prepared")

IMPACT = "kenney_impact-sounds/Audio/"
RPG = "kenney_rpg-audio/Audio/"
SFX = "sfx_100_v2/sfx100v2_"
ZOMBIE = "zombies/zombies/zombie-"


def numbered(prefix, count=5, start=0, width=3, suffix=".ogg"):
    return ["{}{:0{}d}{}".format(prefix, start + i, width, suffix) for i in range(count)]


def zombies(*numbers):
    return ["{}{}.wav".format(ZOMBIE, n) for n in numbers]


# Sound name -> source files. Sounds not listed keep their synthesised voice:
# the horde horn, the collapse rumble, the survivor's hurt voice, the bow and
# the spit have no good CC0 recording in these packs, and stone, dirt and
# foliage creaks are not the wooden creaks the RPG pack has.
#
# The zombie pack's 24 files are unnamed; they were sorted by length and
# spectral centroid: the longest, lowest (1-1.6 s) are groans, the short loud
# ones (0.3-0.7 s) attack grunts, and the brightest screams.
SOUNDS = {
    "hit_stone": numbered(IMPACT + "impactMining_"),
    "hit_wood": numbered(IMPACT + "impactWood_medium_"),
    "hit_dirt": numbered(IMPACT + "impactSoft_medium_"),
    "hit_metal": numbered(IMPACT + "impactMetal_medium_"),
    "hit_foliage": [RPG + "cloth1.ogg", RPG + "cloth2.ogg", RPG + "cloth3.ogg", RPG + "cloth4.ogg"],
    "break_stone": [SFX + "stones_01.ogg", SFX + "stones_02.ogg", SFX + "stones_03.ogg"],
    "break_wood": numbered(IMPACT + "impactWood_heavy_"),
    "break_dirt": numbered(IMPACT + "impactSoft_heavy_"),
    "break_metal": numbered(IMPACT + "impactMetal_heavy_"),
    "break_foliage": [RPG + "chop.ogg", RPG + "knifeSlice.ogg", RPG + "knifeSlice2.ogg"],
    "step_stone": numbered(IMPACT + "footstep_concrete_"),
    "step_wood": numbered(IMPACT + "footstep_wood_"),
    "step_dirt": ["{}footstep{:02d}.ogg".format(RPG, i) for i in range(10)],
    "step_metal": numbered(IMPACT + "impactPlate_light_"),
    "step_foliage": numbered(IMPACT + "footstep_grass_"),
    "place": numbered(IMPACT + "impactPlank_medium_"),
    "flesh_hit": numbered(IMPACT + "impactPunch_medium_"),
    "zombie_groan": zombies(16, 17, 18, 21, 20, 15),
    "zombie_attack": zombies(3, 4, 6, 7, 24, 5),
    "zombie_scream": zombies(8, 9, 12, 10, 11, 13),
    "pickup": [RPG + "handleSmallLeather.ogg", RPG + "handleSmallLeather2.ogg", RPG + "beltHandle1.ogg", RPG + "beltHandle2.ogg"],
    "craft_done": [RPG + "metalLatch.ogg", RPG + "metalClick.ogg"],
    "ui_click": [SFX + "switch_01.ogg", SFX + "switch_02.ogg"],
    "rain_loop": ["sfx_loops/rain.ogg"],
    "wind_loop": ["wind_woosh_loop.ogg"],
    "thunder": [SFX + "thunder_01.ogg"],
    "creak_wood": [RPG + "creak1.ogg", RPG + "creak2.ogg", RPG + "creak3.ogg"],
}

LOOPS = {"rain_loop", "wind_loop"}

MUSIC_OUT = os.path.join(ROOT, "prepared_music")

# Track -> (source, seconds kept or None for all).
MUSIC = {
    "day": ("music/EmptyCity.ogg", None),
    "night": ("music/cold_silence.ogg", 150.0),
    "horde": ("music/ZombiesAreComing.ogg", None),
}

PEAK_DB = -2.0


def run(args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("{} failed:\n{}".format(args[0], result.stderr[-2000:]))
    return result.stderr


def max_volume(path):
    for line in run(["ffmpeg", "-hide_banner", "-i", path, "-af", "volumedetect", "-f", "null", "-"]).splitlines():
        if "max_volume" in line:
            return float(line.split(":")[1].strip().split()[0])
    raise RuntimeError("no volume for " + path)


def prepare(sound, index, source, loop):
    target_dir = os.path.join(OUT, sound)
    target = os.path.join(target_dir, "{}_{:02d}.wav".format(sound, index + 1))
    temp = target + ".tmp.wav"
    filters = ["aformat=channel_layouts=mono"]
    if not loop:
        # Trim leading silence, then trailing silence by trimming the reversed clip.
        trim = "silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.01"
        filters += [trim, "areverse", trim, "areverse"]
    run(["ffmpeg", "-hide_banner", "-y", "-i", source, "-af", ",".join(filters), "-ar", "44100", "-sample_fmt", "s16", temp])
    gain = PEAK_DB - max_volume(temp)
    run(["ffmpeg", "-hide_banner", "-y", "-i", temp, "-af", "volume={:.2f}dB".format(gain), "-ar", "44100", "-sample_fmt", "s16", target])
    os.remove(temp)
    return target


def prepare_music():
    if os.path.isdir(MUSIC_OUT):
        shutil.rmtree(MUSIC_OUT)
    os.makedirs(MUSIC_OUT)
    for track, (relative, seconds) in MUSIC.items():
        source = os.path.join(ROOT, relative)
        if not os.path.isfile(source):
            print("missing " + relative)
            return 1
        filters = ["loudnorm=I=-20:TP=-1.5:LRA=11"]
        args = ["ffmpeg", "-hide_banner", "-y", "-i", source]
        if seconds is not None:
            args += ["-t", str(seconds)]
            filters += ["afade=t=in:d=2", "afade=t=out:st={}:d=3".format(seconds - 3.0)]
        run(args + ["-af", ",".join(filters), "-ac", "2", "-ar", "32000", "-sample_fmt", "s16",
                    os.path.join(MUSIC_OUT, "Music_{}.wav".format(track))])
    print("Prepared {} music tracks".format(len(MUSIC)))
    return 0


def main():
    if shutil.which("ffmpeg") is None:
        print("ffmpeg is not on PATH")
        return 1
    if prepare_music() != 0:
        return 1
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    missing = 0
    written = 0
    for sound, sources in SOUNDS.items():
        os.makedirs(os.path.join(OUT, sound), exist_ok=True)
        for index, relative in enumerate(sources):
            source = os.path.join(ROOT, relative)
            if not os.path.isfile(source):
                print("missing " + relative)
                missing += 1
                continue
            prepare(sound, index, source, sound in LOOPS)
            written += 1
    print("Prepared {} recordings for {} sounds, {} missing".format(written, len(SOUNDS), missing))
    return 0 if missing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

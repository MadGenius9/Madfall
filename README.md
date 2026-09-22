# MadFall

Photoreal voxel survival sandbox for Unreal Engine 5.8. Minecraft's build/mine
loop, 7 Days to Die's structural integrity and horde pressure, AAA fidelity,
mod-first from the ground up.

**Status: playable single-player survival game.** Title screen and saved worlds; structural integrity with stress shading while building; survival,
crafting, loot, zombies and horde nights; biomes with trees and POIs; day/night lighting; doors, ladders, beds and
water; farming; hunting and bows; clothing and armour; a tutorial quest journal, compass and world map; a trader outpost near every spawn; far terrain; traps and zombie variants; weather; synthesized sound. Tier-1 data mods cover every definition kind, with patches and load order;
Tier-2 content mods add assets; Tier-3 script mods run sandboxed Lua
(see [`docs/MODDING.md`](docs/MODDING.md)). Scope is single-player — multiplayer and the dedicated server are deferred to Phase 6. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the plan of record, the
module graph, the voxel layout, the block definition schema and the save format.

## Requirements

- Unreal Engine **5.8**. A Launcher install is sufficient for the current
  single-player scope; the dedicated server target would need a source build,
  see [Dedicated server](docs/ARCHITECTURE.md#dedicated-server).
- Visual Studio 2022 with the C++ game development workload, Windows 10/11 SDK
- Git and Git LFS

Set `MADFALL_UE_ROOT` to your engine root, or the scripts fall back to
`C:\Program Files\Epic Games\UE_5.8`.

## Build

```powershell
.\Scripts\Build.ps1 -Target MadFallEditor
```

## Surface textures

The ground and building surfaces use photo-scanned textures from
[ambientCG](https://ambientcg.com) (CC0, public domain). The imported assets in
`Content/Surfaces` are committed; to re-import from source, run
`Scripts/fetch_surface_textures.ps1` (about 357 MB of downloads into the
uncommitted `SourceArt/`), then `Scripts/make_pbr_material.py`,
`Scripts/import_surface_textures.py` and `Scripts/make_pbr_material.py` again
(the texture-array materials) through `UnrealEditor-Cmd -ExecutePythonScript`.
The storage barrel and loot crate are photo-scanned models from
[Poly Haven](https://polyhaven.com) (CC0): `Scripts/fetch_models.ps1`, then
`Scripts/make_model_material.py` and `Scripts/import_models.py`. The campfire,
torch, door, bedroll and ladder are built from primitives by
`Scripts/build_prop_meshes.py` (no downloads).

## Sound

Most sound effects are recordings from CC0 packs - [Kenney](https://kenney.nl)'s
Impact Sounds and RPG Audio, and from [OpenGameArt](https://opengameart.org) the
Zombies Sound Pack, 100 CC0 SFX #2, 30 CC0 SFX loops and Wind Whoosh Loop. The
imported assets in `Content/Audio` are committed; to rebuild them run
`Scripts/fetch_audio.ps1`, `python Scripts/prepare_audio.py` (needs ffmpeg) and
`Scripts/import_audio.py` through `UnrealEditor-Cmd -ExecutePythonScript`. Sounds
without a recording are synthesised in code. Music, also CC0 from OpenGameArt:
"EmptyCity" and "Zombies' March" by yd, "Cold Silence" by Eponasoft.

## Animals

The deer and the wolf are Quaternius' animated models (Ultimate Animated Animal
Pack, CC0). The imported assets in `Content/Animals` are committed; to rebuild
them run `Scripts/fetch_animals.ps1`, `python Scripts/prepare_animals.py` and
`Scripts/import_animals.py` through `UnrealEditor-Cmd -ExecutePythonScript`.

## Characters

Zombies and traders are drawn as Epic's UE5 mannequins when they are installed.
They come with the engine but may not be published in this repository, so copy
them from your engine install once (nothing is downloaded):

```powershell
.\Scripts\copy_mannequin.ps1
```

Without them the game draws its box figures instead.

## Package

```powershell
.\Scripts\Package.ps1
```

Writes a Windows build to `Saved/Packaged/Windows/MadFall.exe` with the example
mods installed, and records `Releases/0.1` for building content mods
(`.\Scripts\PackageMod.ps1 -Mod <id>`; see [`docs/MODDING.md`](docs/MODDING.md)).

## Play

Run the game (`-game`, or the packaged exe) and it starts at the title screen:
**New World** (name, an optional seed - a number, or any text - and a difficulty:
easy, normal or hard), **Load
World** (every saved world with its day and seed; delete from there too),
**Settings** (look sensitivity, invert look, field of view, view distance,
graphics quality low to epic, HUD size, volume, language, key bindings; saved to `Saved/Config/MadFallSettings.json`), **How to Play**, and
**Continue** for the last world played. Pressing Play in the editor, `-MadWorld=<name>` or
`-unattended` skips the title and plays a world directly. The survivor spawns
once the ground under them has streamed in.

| Input | Action |
|---|---|
| WASD, mouse, Space, Left Shift | Move, look, jump, sprint |
| Left mouse (hold) | Swing the held tool or weapon at the targeted block, zombie or animal; shoot a held bow (uses arrows) |
| Right mouse | Place the held block or plant seeds on farmland, eat/drink the held consumable, put on held clothing, fill an empty bottle at water, or till dirt and grass with a hoe |
| E | Open the targeted crate, open or close a door, sleep in a bedroll to make it your respawn point, trade with a trader |
| I | Backpack, worn clothing, crafting and skills: click to pick up and put down, right-click for half, shift-click to quick-move or put clothing on, put an item mod down on a tool to install it, ctrl-click a tool to take its last mod off, wheel while holding a stack to take more or fewer, Sort to tidy the backpack or a crate, hover for an item's stats, Take to spend a level-up point, wheel to scroll the crafting or skills list |
| Tab | Crafting: the inventory screen's crafting column (category tabs, a craftable-only filter, click a recipe, then Craft 1, 5 or all) |
| M | World map of everywhere you have been (wheel to zoom) |
| 1-9, mouse wheel | Select hotbar slot |
| R, Q | Repair the held tool, drop the held stack |
| Esc or P | Close the open screen, or the pause menu (resume, settings, save and quit) |

**Creative mode** is chosen on the New World page (Mode: Survival / Creative)
and kept with the world. In a creative world: double-tap Space to fly (hold
Space to rise, Left Ctrl to sink - rebindable - and double-tap again to land),
nothing hurts you and hunger, thirst and cold do not apply, placed blocks are
never used up, any block breaks in one hit, and the inventory (I) has a
**Creative** tab listing every item - click one for a full stack.

Each time a world opens, the game backs it up first (the newest three are kept); **Restore** in the world list undoes the last session.

Every keyboard action above can be rebound in **Settings > Controls** (the mouse, Escape, P and 1-9 stay fixed).

Wooden and iron spikes and barbed wire hurt and slow zombies that walk through
them; watch for spitters, which attack from range, and screamers, which call
the horde down on you. Beyond the streamed chunks the landscape continues to the horizon as far
terrain (`mad.far.Enabled 0` to compare). Weather moves through in fronts: rain and storms darken the sky, thicken the fog
and chill the air, and turn to snow in the cold (`mad.weather.set storm` to see
one). The journal at the top right is the tutorial: it starts with crafting a stone axe
and leads through shelter, water, hunting, farming and clothes to the seventh
night (`mad.quests` lists every active quest). The compass along the top shows your heading and the way to your bedroll, your
last backpack and any POI within 300 m. The crosshair label names the targeted block and, for construction, how loaded
it is (`load 72%`): the block nearest 100% is the one that goes first. Crops
show how far they have grown instead. Berry bushes and cabins give corn and
potato seeds; till with a stone hoe, plant, and harvest a day later. Rabbits and deer run
from you (walk, don't sprint, to get close); boars fight back when hurt; wolves
hunt at night. Cook meat at a campfire, and turn hides into cloth or a fur coat
against the cold.
`mad.Language de` switches text to the bundled German example translation.

Every action also has a `mad.player.*` or `mad.menu.*` console command, which
is how CI plays the game headless. `mad.player.tpbiome madfall:forest` travels
to the nearest forest.

## Run the full CI gate

```powershell
.\Scripts\CI.ps1
```

The dedicated-server build is skipped by default. When multiplayer work starts,
`-RequireServer` makes it a hard gate (and requires a source engine build).

## Layout

```
Config/          project ini
Content/Maps/    L_MadFall_Test.umap - generated by Scripts/make_test_map.py
Definitions/     first-party blocks, biomes and prefabs, loaded through the mod JSON path
Mods/            Tier-1/2/3 mods are dropped here (Phase 5)
Scripts/         Build.ps1, CI.ps1, make_test_map.py
Source/
  MadFallModAPI/    stable public surface - loads at PostConfigInit
  MadFallCore/      voxel volume, chunks, serialization, block registry
  MadFallMesher/    Dual Contouring, greedy cubic meshing, LOD, collision
  MadFallGameplay/  survival, GAS, structural integrity, AI - primary game module
  MadFallEditor/    editor-only tooling
docs/ARCHITECTURE.md
```

Dependency direction is strictly one way:
`MadFallModAPI <- MadFallCore <- MadFallMesher <- MadFallGameplay <- MadFallEditor`.
`MadFallModAPI` must never gain an `Engine` dependency — that is what lets it
load early enough to mount mod paks, and what keeps internal types out of the
mod surface.

## MadVoxel (separate project)

[MadGenius9/MadVoxel](https://github.com/MadGenius9/MadVoxel) is an independent
**Unity 6 / URP** single-player survival sandbox in the same genre, developed in its
own repository. It shares no code, assets or engine with MadFall — the two are
unrelated builds of a similar idea.

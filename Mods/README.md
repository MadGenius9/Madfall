# Mods

Drop a mod folder here. Each mod is a directory containing a `mod.json`
manifest. Only this file and the shipped examples belong in the repository;
mods you install locally should not be committed.

Three tiers:

- **Tier 1 - data mods.** No code. JSON/XML definitions for blocks, items,
  recipes, loot tables, biomes, entity stats, progression and localization,
  plus path-based patch files so two mods can edit the same definition without
  conflicting.
- **Tier 2 - content mods.** `.pak` files mounted at runtime with custom meshes,
  textures, materials, sounds, animations and prefab POIs.
- **Tier 3 - script mods.** Sandboxed Lua against `MadFallModAPI`. Event hooks,
  command registration, a per-world key-value store, controlled world queries
  and spawns. No filesystem, no network, no process spawn.

Test mods, one per area of the mod system, all enabled by default (disable any
with `mad.mods.enable <id> 0` and restart):

| Mod | Exercises | Try it |
|---|---|---|
| `survival_tweaks/` | tuning and perk patches, a new perk | hunger drains 25% slower; `mad.perks` lists Iron Stomach |
| `zombie_variety/` | new zombies in existing spawn groups, their own loot | `mad.ai.spawn zombie_variety:crawler 5 0 1` (also `bloater`, `night_stalker`) |
| `builders_pack/` | blocks with new material classes and surfaces, a model block (fence post), items, recipes, loot patches | `mad.player.give builders_pack:brick_wall 10` - aim at a placed one for its load rating |
| `field_medic/` | consumables, a weapon and item mod, loot patches, English + German text | `mad.player.give field_medic:first_aid_kit 1`; `mad.Language de` |
| `example_scripted/` | Tier-3 Lua: events, commands, the per-world store, block edits | `mod.example_scripted.stats`, `mod.example_scripted.beacon 4`; `mad.scripts` |
| `fortifications/` | a dependency (requires `builders_pack`), cross-mod inheritance and a cross-mod recipe patch | disable `builders_pack` and `mad.mods` shows `fortifications` turned off too |

`MadFall.Mods.InstalledModsLoadCleanly` loads every installed mod through every
definition kind and fails on any error; `MadFall.Mods.TestModsBehave` checks
each test mod's feature took effect.

Shipped examples: `example_scavenger/` (Tier 1: new item, block, zombie and
loot patches; exercised by `MadFall.Mods.ExampleDataMod`), `example_german/`
(Tier 1: a translation pack; exercised by `MadFall.Mods.ShippedStrings` and the
CI reload session), and `example_paint/` (Tier 2: a content plugin with a
material, a surface using it and a block of that surface; built and verified in the packaged game by CI
gate 11). The full reference is
[`docs/MODDING.md`](../docs/MODDING.md), including the current status of
Tiers 2 and 3.

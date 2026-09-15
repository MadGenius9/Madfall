# Modding MadFall

MadFall is built so that the shipped game is a mod: every block, biome, POI,
item, recipe, loot table and zombie that ships lives in `Definitions/` and is
loaded through exactly the same code path a mod uses. If the base game can do
something, a mod can.

- **Tier 1 - data mods.** JSON only. New blocks, biomes, POIs, items, recipes,
  loot, zombies, and *patches* that edit anyone's definitions. **Available now.**
- **Tier 2 - content mods.** Cooked assets (materials today; meshes, textures and
  sounds as the game grows uses for them) in an Unreal plugin inside the mod,
  built into a pak with `Scripts/PackageMod.ps1` and referenced from Tier-1 JSON
  by content path. **Available now**, with `Mods/example_paint/` as the example -
  see [Content mods](#content-mods-tier-2).
- **Tier 3 - script mods.** Sandboxed Lua scripts: event handlers, console
  commands, a per-world store and controlled world edits. See "Script mods".
  **Not yet implemented** (see [Status](#status)).

The mod API version is **0.1**. Before 1.0 every minor version may break mods;
a mod declares the version it targets and a mismatch is refused with a message
naming both versions.

---

## Quick start

A complete, working example ships in `Mods/example_scavenger/`. It adds a
weapon, a block, a recipe and a zombie, and patches first-party loot tables so
the weapon can be found - with no code and no assets. Copy it, rename the
folder and the id, and start editing.

```
Mods/
  my_mod/
    mod.json
    definitions/
      blocks/     *.json   madfall.block/1
      biomes/     *.json   madfall.biome/1
      prefabs/    *.json   madfall.prefab/1
      items/      *.json   madfall.item/1
      recipes/    *.json   madfall.recipe/1
      loot/       *.json   madfall.loot/1
      zombies/    *.json   madfall.zombie/1
      animals/    *.json   madfall.animal/1
      quests/     *.json   madfall.quest/1
      surfaces/   *.json   madfall.surface/1
      strings/    *.json   madfall.strings/1
      patches/    *.json   madfall.patch/1
```

Every JSON file may contain one definition object or an array of them. Files
are read in sorted path order.

Useful console commands (`~` in game, or `-ExecCmds=` on the command line):

| Command | What it shows |
|---|---|
| `mad.mods` | Load order, versions, tiers, and every problem found |
| `mad.mods.enable <id> <0\|1>` | Disable or re-enable a mod from the next launch |
| `mad.blocks`, `mad.biomes`, `mad.prefabs`, `mad.items` | Everything registered, including your definitions |
| `mad.player.give <item> [count]` | Try your items |
| `mad.ai.spawn <zombie> <dx> <dy> [horde]` | Try your zombies |
| `mad.animals.spawn <animal> [count] [distance]` | Try your animals |

Validation messages go to the log as `LogMadFallRegistry` / `LogMadFallMods`,
each naming the file, the JSON pointer of the field, what was expected and
what was found. Nothing is silently coerced.

---

## `mod.json`

```json
{
  "schema": "madfall.mod/1",
  "id": "my_mod",
  "name": "My Mod",
  "version": "1.2.0",
  "api_version": "0.1",
  "description": "What it does.",
  "authors": ["You"],
  "dependencies": [
    { "id": "steel_pack", "version": "^1.0.0" },
    { "id": "better_lights", "optional": true }
  ],
  "load_after": ["ui_tweaks"],
  "load_before": ["late_balance_pass"],
  "incompatible": ["old_version_of_my_mod"],
  "paks": ["Content/my_mod.pak"],
  "scripts": ["scripts/main.lua"]
}
```

| Field | Required | Meaning |
|---|---|---|
| `schema` | yes | `madfall.mod/1` |
| `id` | yes | Lowercase `a-z`, `0-9`, `_`; 2-64 characters; not starting with a digit; not `madfall`. **This is your namespace**: every definition you add is `my_mod:<name>`. |
| `version` | yes | `major.minor.patch`. Suffixes like `-beta` are allowed and ignored for comparisons. |
| `api_version` | yes | The mod API version you built against, e.g. `"0.1"`. |
| `name`, `description`, `authors`, `homepage`, `license` | no | Shown in `mad.mods` and the future mod manager. |
| `dependencies` | no | Mods that must be present. `version` is a requirement (below); `optional: true` makes it an ordering hint that is not required. |
| `load_after` / `load_before` | no | Soft ordering against mods that may or may not be installed. |
| `incompatible` | no | Mods you refuse to run with. If both are enabled, **your** mod is the one that is turned off. |
| `paks` | no | Tier-2 content, paths relative to your folder. |
| `scripts` | no | Tier-3 scripts, paths relative to your folder. |

Paths must stay inside your mod folder. Unknown fields produce a warning but
do not stop the mod loading.

The folder name does not matter; `id` does. Players rename folders.

### Version requirements

| Requirement | Matches |
|---|---|
| `*` | anything |
| `1.4.2` | exactly 1.4.2 |
| `>=1.2`, `>1.2.0`, `<2`, `<=1.9.9` | comparisons; missing parts are 0 |
| `^1.2.0` | `>=1.2.0, <2.0.0` (for `^0.3.1`: `>=0.3.1, <0.4.0`) |
| `~1.2.0` | `>=1.2.0, <1.3.0` |
| `>=1.2, <1.5` | all comma-separated clauses |

### How load order is decided

First-party content always loads first. Then every enabled mod, ordered so that:

1. A dependency loads before the mod that needs it.
2. A `load_after` target loads before you; you load before a `load_before`
   target (when they are installed).
3. Where nothing constrains two mods, they load **alphabetically by id**. The
   order never depends on folder names, disk order or the machine.

A mod is turned off - with the reason in `mad.mods` and the log - when:

- its `mod.json` is invalid, or its `api_version` is not the running one;
- a required dependency is missing, disabled, failed, or the wrong version
  (and anything that requires *that* mod is turned off in turn);
- it declares an enabled mod `incompatible`;
- it is part of a load-order cycle (only the mods on the cycle; a mod that only
  softly orders itself after one loads normally);
- the same `id` is installed twice (the first folder, alphabetically, wins).

The resolved order has a fingerprint (shown by `mad.mods`) so a world can
record exactly which mods and versions it was played with.

Players' disabled mods are stored in `Saved/Config/MadFallMods.json`, never in
your folder, so an update of your mod does not re-enable it.

---

## Ids, namespaces and overriding

- Every id is `<namespace>:<name>`, lowercase `a-z`, `0-9`, `_` (loot tables
  and recipes may also use `/` in the name: `my_mod:loot/rare_crate`).
- **The namespace of anything you define must be your mod id.** Defining
  `madfall:stone` from `my_mod` is rejected.
- To change something that is not yours, **patch it** (below). Do not copy it.
- Defining the same id twice within your own mod (in two files) keeps the file
  that sorts later, with a warning naming both.
- `extends` may name a definition from any mod that loads earlier, including
  first-party: `"extends": "madfall:wood_frame"` inherits everything you do not
  override.

### Inheritance rules

A child's JSON is applied over its resolved parent:

- a field you do not mention is inherited;
- **objects merge** field by field (`"material": {"hardness": 300}` keeps the
  parent's `mass_kg`);
- **arrays replace** (`"tags": ["a"]` replaces the parent's tags entirely);
- maps like `resistances` and a tool's `damage` merge key by key.

Inheritance is available for blocks, biomes and items.

---

## Patches

A patch edits another definition by JSON pointer. It is the only safe way for
two mods to change the same thing.

```json
{
  "schema": "madfall.patch/1",
  "kind": "block",
  "target": "madfall:wood_frame",
  "ops": [
    { "op": "set",    "path": "/material/hardness",       "value": 140 },
    { "op": "set",    "path": "/damage_states/1/at",      "value": 180 },
    { "op": "append", "path": "/tags",                    "value": "block.sturdy" },
    { "op": "remove", "path": "/drops/on_collapse" }
  ]
}
```

| Field | Meaning |
|---|---|
| `kind` | `block`, `biome`, `item`, `recipe`, `loot`, `zombie`, `animal`, `quest`, `trader`, `tuning`, `perk` or `surface` |
| `target` | The id to patch |
| `ops[].op` | `set` writes a value (creating missing objects on the way; `-` as the last array index appends); `append` adds to an array (creating it if absent); `remove` deletes a field or array element |
| `ops[].path` | [RFC 6901](https://datatracker.ietf.org/doc/html/rfc6901) pointer into the definition as written in its JSON (`~1` for `/`, `~0` for `~`) |

How patches behave:

- They apply **after every mod's definitions are staged and before inheritance
  is resolved**, in load order. Patching a parent reaches every child that does
  not override that field.
- Two mods patching **different** paths of the same definition both apply.
- Two mods writing the **same** path: the later mod wins, with a warning naming
  both.
- A patched definition is validated exactly like a hand-written one. A patch
  that makes a recipe invalid disables that recipe, with the reason.
- A patch whose target does not exist is reported - usually a typo, or a patch
  for a mod that is not installed. It does nothing else.
- Prefabs cannot be patched; they are voxels, not fields. Ship a new prefab.

---

## Definition reference

Units: 1 voxel = 1 m. Mass in kg per voxel, hardness in hit points, support in
kg, speeds in m/s, time in seconds, temperatures in °C.

### Blocks - `madfall.block/1`

```json
{
  "schema": "madfall.block/1",
  "id": "my_mod:plank_wall",
  "extends": "madfall:wood_frame",
  "display_name": "Plank Wall",
  "tags": ["block.building"],
  "shape":     { "kind": "cubic", "rotation_mode": "facing_4", "variants": [], "collision": "mesh", "occludes_neighbors": true },
  "material":  { "class": "madfall:wood", "mass_kg": 250, "hardness": 120,
                 "resistances": { "madfall:fire": 2.5 },
                 "harvest": { "tool_tags": ["tool.axe"], "tier": 0 } },
  "structure": { "support_strength": 16000, "max_horizontal_span": 5, "is_anchor": false, "debris_on_collapse": "madfall:concrete_rubble" },
  "damage_states": [
    { "at": 0,   "support_multiplier": 1.0 },
    { "at": 160, "support_multiplier": 0.4, "decal_set": "madfall:cracks_light", "sound": "madfall:wood_crack" },
    { "at": 255, "downgrade_to": "madfall:wood_frame" }
  ],
  "drops":  { "table": "my_mod:loot/plank_wall", "on_collapse": "madfall:loot/wood_debris", "requires_tool_tags": [] },
  "render": { "mesh": "/MyMod/Meshes/SM_PlankWall", "material": "/MyMod/Materials/M_Plank", "nanite": true, "cast_shadow": true },
  "sounds": { "hit": "/MyMod/Audio/S_Hit" },
  "flags":  { "transparent": false, "liquid": false, "climbable": false, "flammable": true, "conductive": false }
}
```

- `shape.kind`: `cubic` (building grid, can collapse), `isosurface` (smooth
  terrain, never collapses), `model` (a static mesh in one voxel).
- **Model blocks** draw `render.mesh` (a static mesh path, from the base game,
  the engine, or your content plugin) centred in the voxel and turned with the
  block's orientation. `render.offset` moves it in voxels (`[0, 0, -0.5]` puts a
  100 uu mesh with a centred pivot on the floor) and `render.scale` resizes it
  (1 fits a 100 uu mesh to the voxel). Without `render.material` the mesh is
  drawn with its surface's colour and `pattern`, projected from the mesh's own
  space as if it were one voxel (so a 100 uu mesh gets 16 texels a side).
  A mesh made of several materials can instead name each material slot after a
  surface class (`madfall:stone`, `mymod:brass`): every slot so named is drawn
  with that surface's texture in the mesh's own space, no UVs needed, and slots
  with other names keep the mesh's own material (`render.material`, if set,
  still replaces slot 0 and turns this off). The shipped campfire, torch, door,
  bedroll and ladder work this way (`Scripts/build_prop_meshes.py`). The mesh is also the collision; the voxel
  still carries weight, damage, drops and tags, so a model block can be a
  container, a station or part of a structure. Neighbouring blocks never hide
  faces behind a model block. Example: `madfall:storage_barrel`.
- **Light**: `"render": { "light": { "color": [1.0, 0.62, 0.3], "lumens": 150,
  "radius": 9, "offset": [0, 0, 0.2] } }` makes any block give off light (sRGB
  colour, luminous power in lumens, reach in voxels up to 32, offset in voxels
  turned with the block). `flame` sizes the flickering flame drawn centred on
  the light (1 is 20 cm tall; 0 for none, say a lamp). Lights cast shadows (`mad.models.LightShadows`, off at low and medium
  graphics quality) and are capped per chunk (`mad.models.MaxLightsPerChunk`,
  16); every light block past the cap still draws its flame. Example:
  `madfall:torch`. For scale: the noon sun is 10 lux and the moon 0.35.
- `shape.rotation_mode`: `none`, `axis`, `facing_4`, `full_24`.
- **Structure**: a cubic block is supported through a path to the ground or an
  anchor. Stepping sideways costs `1 / max_horizontal_span` of the budget, so a
  span-5 block holds a 5-block cantilever and drops the 6th. Weight flows down
  the supporting path; a block carrying more than `support_strength` breaks.
  Damage stages multiply both.
- **Damage**: `hardness` is hit points; `resistances` multiply damage by type
  (`madfall:blunt`, `madfall:pierce`, `madfall:explosive`, `madfall:fire`,
  `madfall:crush`, or your own). `0` is immune. Crossing a stage with
  `downgrade_to` turns the block into that block, carrying the extra damage.
- **Harvest**: a tool sharing a `tool_tags` entry at or above `tier` harvests
  drops at full speed; otherwise the block breaks slowly and drops nothing.
- `drops.table` names a loot table. Without one, the block drops its own item.
- **Collapse**: when a cubic block falls, about one in three becomes its
  `structure.debris_on_collapse` block (which must exist - the shipped-content
  test checks). Every other fallen block rolls `drops.on_collapse` once, and the
  results are left as one bag on the pile. Falling blocks hurt anyone they land
  on.
- Every non-liquid block that is not tagged `block.indestructible` gets an item
  automatically. Define an item with the same id to customise it.
- Tags with special meaning: `block.container` (a lootable, storable crate),
  `block.station` plus a recipe `station` naming the block (crafting station),
  `block.indestructible`, `block.natural` (generated scenery: skipped by the
  structural check when its chunk loads; see Scatter below).
- **Interaction** (the E key): `"interact": { "toggle_to": "my_mod:gate_open" }`
  swaps the block for another one, keeping its orientation and damage, along
  with the same block stacked directly above and below (both halves of a door).
  Define the open state as its own block with `"collision": "none"` and a
  `toggle_to` back. `"interact": { "spawn_point": true }` makes the block a bed:
  the survivor respawns on it while it stands. See `madfall:wood_door`,
  `madfall:wood_door_open` and `madfall:bedroll`.
- `flags.climbable`: standing in the block climbs it - forward goes up, back
  goes down. Give a ladder `"collision": "none"` so the survivor can stand in
  its voxel (`madfall:ladder`).
- `shape.rotation_mode: "facing_4"` blocks turn to face away from the survivor
  when placed.
- **Crops.** `"grow": { "into": "my_mod:wheat_ripe", "hours": 8 }` turns the
  block into the next stage after that many in-game hours (a day is 24; 40 real
  minutes by default). Chain as many stages as you like, and give the last stage
  no `grow`. Growth continues while the player is away: a chunk that loads again
  catches up stage by stage. `"placement": { "on_tag": "block.farmland" }` only
  lets the block be placed on top of a block with that tag, so seeds go on
  farmland. Tag stages `block.crop` and the crosshair shows their growth instead
  of a structural load. Write the ripe stage in full and have the earlier stages
  `extends` it, since an `extends` child inherits a parent's `grow`. See
  `Definitions/blocks/madfall_farming.json`.
- **Traps.** `"trap": { "damage": 14, "seconds": 0.8, "wear": 12, "slow": 0.7 }` on
  a block with `"collision": "none"` hurts zombies and animals standing in it
  every `seconds`, damages the trap itself by `wear` each time, and scales
  their speed by `slow`. Creatures path straight into collision-none blocks
  (traps, open doors, ladders) instead of digging through them. See
  `Definitions/blocks/madfall_traps.json`.
- `block.tillable`: an item with `"till": "madfall:farmland"` (the stone hoe)
  turns this block into farmland when right-clicked with open air above.

### Biomes - `madfall.biome/1`

```json
{
  "schema": "madfall.biome/1",
  "id": "my_mod:ash_wastes",
  "extends": "madfall:base_land",
  "climate": { "temperature": [0.7, 1.0], "moisture": [0.0, 0.2], "continentalness": [0.5, 1.0], "weight": 1.0 },
  "terrain": { "base_height": 20, "height_variation": 12, "roughness": 0.6, "ridging": 0.2 },
  "blocks":  { "surface": "my_mod:ash", "subsurface": "madfall:gravel_path", "subsurface_depth": 3, "stone": "madfall:stone", "underwater_surface": "madfall:sand" },
  "ores": [ { "block": "madfall:iron_ore", "probability": 0.4, "cluster_size": 8, "min_z": -100, "max_z": 20, "attempts_per_chunk": 6 } ]
}
```

Climate values are 0-1. A biome wins where its climate ranges fit best, weighted
by `weight`; neighbouring biomes blend terrain height. Adding a biome changes
terrain that has **not been generated yet**; it never rewrites a saved world.

**Scatter** - trees, boulders and plants on the surface:

```json
"scatter": [
  { "feature": "tree",    "block": "madfall:oak_log", "leaves": "madfall:oak_leaves", "chance": 0.028, "height": [4, 7], "radius": [2, 3] },
  { "feature": "plant",   "block": "madfall:berry_bush", "chance": 0.006 },
  { "feature": "boulder", "block": "madfall:stone", "chance": 0.001, "radius": [1.2, 2.2] }
]
```

- Each surface column rolls once against the list: `chance` is the fraction of
  columns a feature grows from (0.01 is about ten per chunk), earlier entries
  first. Chances above a total of 1 are reported.
- `tree`: a trunk of `block`, `height` voxels tall, with a canopy of `leaves`
  of about `radius`. Leave out `leaves` for a bare trunk (the desert's cactus).
  Trunk and leaves are ordinary cubic blocks - chop the trunk and the tree
  falls. Canopy leaves that the leaves block's `max_horizontal_span` could not
  hold up are never generated, so give leaves a span of at least the radius
  plus one.
- `boulder`: a smooth sphere of `block` sunk into the ground, `radius` voxels.
  It is terrain, so it never collapses.
- `plant`: one `block` on the surface - use a `model` block for a bush.
- Features stay clear of POIs and roads, never grow underwater, and never
  replace construction. `radius` is capped at 6.
- Tag scatter blocks `block.natural`: generated sound, they are not re-checked
  structurally every time their chunk loads (a forest canopy is one structure
  thousands of blocks wide). `MadFall.Structural.GeneratedScatterStands` checks
  the shipped biomes' scatter holds up.
- Arrays replace on `extends`, so a child biome's `scatter` replaces its
  parent's; omit it to inherit.

### Prefabs (POIs) - `madfall.prefab/1`

A prefab is a block of voxels plus markers. Capture one from the world with
`mad.prefab.capture` (it writes this format), or generate it with a script
like `Scripts/make_sample_prefabs.ps1`.

```json
{
  "schema": "madfall.prefab/1",
  "id": "my_mod:gas_station",
  "display_name": "Gas Station",
  "tier": 2,
  "tags": ["poi.commercial"],
  "size": [15, 11, 6],
  "placement": { "rarity": 1.0, "conform": "base", "embed_depth": 1, "max_slope": 6,
                 "foundation": "madfall:concrete_frame", "max_foundation_depth": 10,
                 "biomes": ["madfall:plains"], "underwater": false },
  "palette": [ { "block": "*" }, { "block": "madfall:air" }, { "block": "madfall:concrete_frame", "orientation": 0, "variant": 0 } ],
  "voxels": [ 165, 2, 825, 1 ],
  "markers": [
    { "type": "entrance", "position": [7, 0, 1] },
    { "type": "loot",  "position": [3, 8, 1], "loot_table": "my_mod:loot/register" },
    { "type": "spawn", "position": [10, 5, 1], "spawn_group": "madfall:zombies/civilian", "count": 3 }
  ]
}
```

- `voxels` is run-length encoded pairs `[count, palette_index, ...]`, X fastest,
  then Y, then Z. `*` leaves the terrain alone; `madfall:air` carves it.
- Put a `block.container` block on each `loot` marker so players can open it.
  Loot is rolled the first time it is opened, at that moment's game stage.
- `spawn` markers wake their group once per game day when a player comes near.
- `trader` markers (`{ "type": "trader", "position": [7, 10, 1], "trader": "my_mod:rosa" }`)
  stand that trader there, on the voxel above the floor, whenever a player is near.
- `tier` 1-5: further from spawn allows higher tiers.
- `placement.near_spawn: true` places the prefab exactly once per world, in the
  nearest ring of cells around the spawn that has a site for it, and nowhere
  else whatever its `rarity` (give it `rarity: 0`). The shipped
  `madfall:trader_outpost` uses it, so every world has a trader a new player can
  find; the compass points to it.

### Items - `madfall.item/1`

```json
{
  "schema": "madfall.item/1",
  "id": "my_mod:machete",
  "extends": "madfall:base_tool",
  "display_name": "Machete",
  "tags": ["item.weapon", "tool.axe"],
  "kind": "weapon",
  "max_stack": 1,
  "weight_kg": 1.2,
  "icon": "/MyMod/Icons/T_Machete",
  "places_block": null,
  "tool": {
    "damage": { "madfall:pierce": 40 },
    "harvest_tags": ["tool.axe"], "tier": 1,
    "durability": 300, "use_seconds": 0.5, "stamina_cost": 4, "range": 2.5,
    "mod_slots": 2,
    "repair_with": [ { "item": "madfall:scrap_iron", "count": 2 } ], "repair_fraction": 0.3
  },
  "consumable": { "use_seconds": 1.0, "effects": { "health": 10, "food": 0, "water": 0, "stamina": 0, "infection": -5, "temperature": 0 } },
  "mod": { "applies_to_tags": ["item.weapon"], "multipliers": { "damage": 1.2, "durability": 1.5, "use_seconds": 0.9, "stamina_cost": 0.8, "range": 1.1 } }
}
```

`kind` is inferred when omitted (`places_block` → block, `tool` → tool,
`consumable` → consumable, `mod` → mod). Tools and weapons never stack.

`icon` is optional: a texture from your content mod. Without one the game
paints a pixel-art icon from the item's data - a cube in the block's surface
colour for a block item; for others a picture chosen by tags and wear slot
(`tool.pickaxe`, `tool.axe`, `tool.shovel`, `tool.hoe`, a ranged tool, `item.ammo`,
`item.currency`, `item.drink`, `item.medical`, `item.food`, `item.seed`,
`item.wood`, `item.stone`, `item.metal`, worn head/body/legs/feet), then by
words in the id (`can`, `meat`, `berr`, `potato`, `corn`, `bottle`, `cloth`,
`hide`, `helmet`). Tag your items and most need no icon at all.

- `value`: what a trader asks for one, in coins. Traders buy anything with a
  value for their `buy_factor` of it and will not buy what has none.

- `consumable.returns`: an item given back after use (a drink's empty bottle).
- `wear`: `{ "slot": "body", "cold": 12, "heat": 0, "armor": 0.1 }` makes the
  item clothing. `slot` is `head`, `body`, `legs` or `feet`. `cold` and `heat`
  are degrees of insulation: the survivor's comfortable air temperature band
  widens by that much. `armor` is the fraction of zombie and animal attack
  damage absorbed, 0 to 0.8; worn pieces add up to the same 0.8 cap.
  Right-clicking clothing puts it on. So does shift-clicking it in the backpack,
  and it sits in the inventory screen's Worn row. Give clothing
  `"max_stack": 1`. See `madfall:fur_coat` and `madfall:scrap_vest`.
- `tool.ranged`: `{ "ammo": "madfall:arrow", "speed": 45, "gravity": 0.5, "recover_chance": 0.5 }`
  makes the weapon shoot instead of swing: each use fires one `ammo` item as a
  projectile at `speed` voxels per second, falling with that fraction of
  gravity, dealing the tool's strongest `damage` to the first zombie or animal
  it meets. A shot that hits the world drops its ammo back with
  `recover_chance`. Shots make no noise. See `madfall:wooden_bow`.
- `till`: right-clicking a `block.tillable` block turns it into this block
  (`madfall:stone_hoe` makes `madfall:farmland`). Uses a point of durability.
- `fill`: right-clicking water with this item turns one of it into that item
  (`madfall:empty_bottle` fills into `madfall:murky_water`, which boils into a
  `madfall:water_bottle` at a campfire).

### Recipes - `madfall.recipe/1`

```json
{
  "schema": "madfall.recipe/1",
  "id": "my_mod:machete",
  "output": { "item": "my_mod:machete", "count": 1 },
  "ingredients": [ { "item": "madfall:scrap_iron", "count": 5 }, { "item": "madfall:wood_plank", "count": 1 } ],
  "station": "madfall:workbench",
  "craft_seconds": 8,
  "required_level": 4,
  "tags": ["recipe.weapon"]
}
```

`station` is a block id the player must be within 3 m of. Omit it to craft from
the backpack. A recipe naming an item nobody defines is disabled, with the file
and field.

### Loot tables - `madfall.loot/1`

```json
{
  "schema": "madfall.loot/1",
  "id": "my_mod:loot/register",
  "rolls": [1, 3],
  "rolls_per_tier": 0.5,
  "empty_weight": 1,
  "entries": [
    { "item": "madfall:scrap_iron", "count": [2, 5], "weight": 4 },
    { "table": "madfall:loot/food", "weight": 2 },
    { "item": "my_mod:machete", "weight": 1, "tier": [3, 5], "min_game_stage": 10, "durability": [0.3, 0.8] }
  ]
}
```

Each roll picks one entry by weight (or nothing, by `empty_weight`). `tier`
limits an entry to POI tiers; `min_game_stage` unlocks it later in the game
(game stage = days survived + player level). An entry with `"always": true` is
given every time the table is rolled, outside the weighted picks: a crop's
harvest always returns its seeds. Nested tables may not form cycles; a cycle is
broken at load with an error.

### Zombies - `madfall.zombie/1`

```json
{
  "schema": "madfall.zombie/1",
  "id": "my_mod:zombie_firefighter",
  "display_name": "Firefighter",
  "tags": ["zombie.armoured"],
  "spawn_groups": ["madfall:zombies/horde", "my_mod:zombies/station"],
  "spawn_weight": 2,
  "min_game_stage": 10,
  "stats": { "health": 300, "walk_speed": 1.2, "run_speed": 3.0, "attack_damage": 12,
             "block_damage": 40, "attack_seconds": 1.4, "damage_type": "madfall:blunt", "infection_per_hit": 3 },
  "senses": { "sight": 24, "hearing": 40 },
  "rewards": { "experience": 60, "loot_table": "madfall:loot/zombie_soldier" },
  "appearance": { "scale": 1.1, "tint": [0.7, 0.3, 0.2] }
}
```

Spawn groups are memberships: adding `madfall:zombies/horde` puts your zombie in
horde nights; a prefab `spawn` marker can name your own group. Zombies path
through the voxel world and dig through whatever is cheapest to break
(`block_damage` against block `hardness` and resistances).

Shipped groups: `madfall:zombies/civilian`, `madfall:zombies/soldier`,
`madfall:zombies/brute`, `madfall:zombies/horde`, `madfall:zombies/wander`.

Optional `abilities` add behaviour on top of chasing, digging and clawing:

```json
"abilities": {
  "ranged": { "damage": 9, "range": 16, "seconds": 2.6, "speed": 24 },
  "scream": { "radius": 60, "seconds": 40, "summons": 3 }
}
```

- `ranged`: a zombie that can see its target within `range` voxels (and is
  not already in claw reach) stands and spits every `seconds`. The spit is a
  lobbed projectile at `speed` voxels a second. It passes through other
  zombies, infects a little, and scales with difficulty.
- `scream`: on seeing a target, at most every `seconds`, it alerts every zombie
  within `radius` voxels and calls `summons` (up to 8) wanderers from out of
  sight, all hunting the survivor.

Shipped: `madfall:zombie_spitter` (from game stage 4) and
`madfall:zombie_screamer` (from 6).

Zombie `abilities` may also set `"climbs_walls": true`: the zombie climbs
straight up any wall toward its target instead of only digging and
undermining (see `madfall:zombie_climber`). Every zombie climbs ladders, and a
hunting zombie digs down through the floor toward a target below it.

`resistances` are damage multipliers by damage type, like a block's:
`{ "madfall:pierce": 0.5, "madfall:fire": 1.5 }` takes half from arrows and
half again more from fire; a type not listed is 1. They apply to every hit - a
swing, an arrow, a trap, falling debris. A weapon with several damage types
hits with whichever does the most after the victim's resistance. Shipped:
`madfall:zombie_soldier` (armour: pierce 0.5) and `madfall:zombie_brute`
(blunt 0.7, fire 1.3); `zombie_variety:bloater` shrugs off blunt and bursts on
pierce.

### Animals - `madfall.animal/1`

Folder: `definitions/animals/`. Wildlife to hunt, and wildlife that hunts back.

```json
{
  "schema": "madfall.animal/1",
  "id": "my_mod:elk",
  "display_name": "@my_mod.animals.elk",
  "tags": ["animal.game"],
  "behaviour": "defensive",
  "spawn": { "biomes": ["madfall:tundra", "madfall:forest"], "weight": 1.5, "herd": [2, 4], "active": "day" },
  "stats": { "health": 150, "walk_speed": 1.0, "run_speed": 6.0, "attack_damage": 15, "attack_seconds": 1.5, "damage_type": "madfall:pierce" },
  "senses": { "sight": 26, "hearing": 30, "flee": 12 },
  "rewards": { "experience": 40, "loot_table": "my_mod:loot/elk" },
  "appearance": { "body": [140, 40, 60], "legs": 80, "neck": 50, "scale": 1.0, "tint": [0.45, 0.33, 0.22] }
}
```

- `behaviour`: `skittish` animals run from a survivor they see within `flee`
  voxels, hear (sprinting, swinging, building) within `hearing`, or that has
  just hurt them. A `defensive` animal grazes until hurt, then fights until the
  survivor is well out of `sight`. An `aggressive` one hunts any survivor it
  sees. An animal with no `attack_damage` always runs, whatever it says.
- `spawn`: the director tries a herd every 25 seconds (`mad.animals.SpawnSeconds`)
  in a ring 30-56 voxels from the survivor, picking by `weight` among animals
  whose `biomes` include the biome of that spot and whose `active` time
  (`always`, `day` or `night`) matches the clock. At most 12 live at once
  (`mad.animals.Max`); animals are not saved.
- `appearance`: the four-legged rig is sized from `body` (length, width, height
  in cm), `legs` and `neck`; `tint` is sRGB.
- Animals never dig. Killing one drops `rewards.loot_table` where it falls; use
  `"always": true` entries so a carcass always yields meat.

Shipped: `madfall:rabbit` and `madfall:deer` (skittish), `madfall:boar`
(defensive), `madfall:wolf` (aggressive, at night). Try yours with
`mad.animals.spawn <id> [count] [distance]`, and see what they are doing with
`mad.animals.status`.

### Quests - `madfall.quest/1`

Folder: `definitions/quests/`. The shipped tutorial is `Definitions/quests/madfall_tutorial.json`.

```json
{
  "schema": "madfall.quest/1",
  "id": "my_mod:quest/radio_tower",
  "display_name": "@my_mod.quests.radio_tower",
  "description": "@my_mod.quests.radio_tower.description",
  "requires": ["madfall:quest/four_walls"],
  "order": 70,
  "objectives": [
    { "type": "craft", "target": "my_mod:radio" },
    { "type": "place", "tag": "block.building", "count": 40, "text": "@my_mod.quests.radio_tower.build" },
    { "type": "kill_zombie", "tag": "zombie.armoured", "count": 3 }
  ],
  "rewards": { "experience": 200, "items": [ { "item": "madfall:antibiotics", "count": 2 } ] }
}
```

- A quest starts as soon as every quest in `requires` is complete. There is
  no quest giver, and a quest completes the moment its last objective is met.
  The HUD lists the first two active quests by `order`; `mad.quests` lists
  them all.
- Objective `type`: `craft`, `place`, `break`, `kill_zombie`, `kill_animal`,
  `wear` and `trade` (a purchase or sale; `target` is the trader id) count events. `set_spawn` counts sleeping in a bed. `have`
  checks items carried right now (dropping them undoes it). `reach_day`
  checks the clock (`count` is the day). `target` is an exact item, block,
  zombie or animal id and `tag` a tag it must carry; leave both out to accept
  anything of that type.
- `text` is the journal line. Without it one is built from the type and target
  (`Craft Stone Axe`); give tag-based objectives text. The count is shown
  after it, so leave numbers out of the text.
- Ripe crops carry `block.ripe`, so a harvest objective is `{ "type": "break", "tag": "block.ripe" }`.
- A mod extends the tutorial by requiring one of its quests. Completion is
  saved by id, so removing a mod keeps its quests' progress for when it returns.
- `mad.quests.complete <id>` completes a quest and pays its rewards.

### Traders - `madfall.trader/1`

Folder: `definitions/traders/`. The shipped trader is `Definitions/traders/madfall_traders.json`.

```json
{
  "schema": "madfall.trader/1",
  "id": "my_mod:mechanic",
  "display_name": "@my_mod.traders.mechanic",
  "greeting": "@my_mod.traders.mechanic_greeting",
  "currency": "madfall:coin",
  "buy_factor": 0.25,
  "buys_tags": ["item.tool", "item.resource"],
  "restock_days": 3,
  "stock": [
    { "item": "madfall:scrap_iron", "count": [10, 30] },
    { "item": "my_mod:engine", "count": 1, "price": 400, "chance": 0.2 }
  ]
}
```

- A trader stands wherever a prefab has a `trader` marker naming it.
- Stock is rolled per trader position and rerolled every `restock_days`, the
  first time a player looks after that. `count` is a number or `[min, max]`,
  `chance` the odds the line is in stock at all. `price` overrides the item's
  `value`; a line with neither is an error. The shelves hold 24 stacks.
- The trader buys any item with a `value` (only items carrying one of
  `buys_tags`, if given) for `buy_factor` of it, at least 1, and between 0 and
  1 so nothing can be bought and sold back at a profit. A worn tool fetches that
  scaled by the durability left. What a player sells goes onto the shelf.
- `currency` defaults to `madfall:coin`, which zombies and loot containers drop.
- Patch kind: `trader`.

### Survival tuning - `madfall.tuning/1`

Folder: `definitions/tuning/`. The survival rules read the definition with id
`madfall:survival`, so a balance mod **patches** it rather than shipping its own:

```json
{
  "schema": "madfall.patch/1",
  "kind": "tuning",
  "target": "madfall:survival",
  "ops": [
    { "op": "set", "path": "/values/food_per_minute", "value": 1.0 },
    { "op": "set", "path": "/values/hypothermia_below", "value": 34.0 }
  ]
}
```

Every value is a number; keys not mentioned keep the built-in default. An
unrecognised key is logged as a warning and ignored.

Difficulty works the same way. `madfall:difficulty_easy`, `_normal` and `_hard`
(`Definitions/tuning/madfall_difficulty.json`) hold multipliers the game reads
for the world's chosen difficulty: `zombie_damage`, `zombie_health`,
`horde_size`, `animal_damage` and `survival_drain`. A key a level does not
list is 1. Patch one to rebalance a level.

| Key | Default | Meaning |
|---|---|---|
| `food_per_minute`, `water_per_minute` | 0.5, 0.8 | Drain at rest |
| `sprint_drain_multiplier` | 2.5 | Food/water drain factor while sprinting |
| `stamina_regen_per_second`, `sprint_stamina_per_second` | 15, 10 | Stamina in and out |
| `weak_threshold` | 20 | Food or water below this halves stamina regeneration |
| `starvation_damage_per_second`, `dehydration_damage_per_second` | 0.5, 0.8 | Health loss at 0 food / water |
| `health_regen_per_second`, `well_fed_threshold` | 0.15, 60 | Regeneration while food and water are both above the threshold and nothing is doing damage |
| `comfort_min`, `comfort_max` | 5, 30 | Air temperature band (°C) with no core drift |
| `core_drift_per_second_per_degree` | 0.0005 | Core temperature change per degree outside the band |
| `core_recovery_per_second` | 0.05 | Return to 37 °C inside the band |
| `hypothermia_below`, `hyperthermia_above` | 35, 39 | Core temperatures that start exposure damage |
| `exposure_damage_per_second_per_degree` | 0.4 | Health loss per degree past a threshold |
| `hyperthermia_water_per_second` | 0.05 | Extra water loss when overheating |
| `infection_growth_per_second` | 0.02 | Infection rise once infected |
| `infection_damage_above`, `infection_damage_per_second_at_full` | 50, 1 | Infection (0-100) starts doing damage above this, ramping linearly to the full rate at 100 |

### Perks - `madfall.perk/1`

Folder: `definitions/perks/`. Survivors earn one perk point per level after the
first and spend it on the next rank of a perk (`mad.perks` lists them,
`mad.player.perk <id>` buys one).

```json
{
  "schema": "madfall.perk/1",
  "id": "my_mod:scavenger",
  "display_name": "Scavenger",
  "description": "Works longer on less food.",
  "tags": ["perk.fortitude"],
  "ranks": [
    { "level": 3, "modifiers": { "food_drain": 0.9, "mining_damage": 1.1 } },
    { "level": 8, "modifiers": { "food_drain": 0.75, "mining_damage": 1.25 },
      "requires": { "madfall:tough": 1 } }
  ]
}
```

- `ranks` are in rising `level` order; `level` is the survivor level needed to
  buy that rank.
- A rank **replaces** the previous rank's modifiers - write each rank's total,
  not the increment. Modifiers from different perks multiply.
- Modifiers are multipliers on these stats: `mining_damage`, `melee_damage`,
  `stamina_cost`, `craft_time`, `max_health`, `max_stamina`, `food_drain`,
  `water_drain`.
- `requires` (optional, per rank) names other perks and the rank of each the
  survivor must own before buying this rank. Gate only the top ranks to make a
  branch: the perk opens freely and deepens once its root is taken. The skills
  screen groups perks by their first tag (`perk.fortitude` shows under
  FORTITUDE), indents a perk under a prerequisite in the same group, and says
  "needs Tough 1" in place of the Take button. A requirement on a perk nobody
  defines is dropped, a rank beyond what that perk has is lowered to its top
  rank, a perk requiring itself is an error, and a cycle is broken where it
  closes - each reported with the file - so a rank is never locked forever.
  Ranks already owned keep working if a patch adds a requirement later.
- Ranks are saved by perk id. A save that owns ranks of a perk from a removed
  mod keeps them (and the points stay spent) but they have no effect until the
  mod is back.

### Text and translations - `madfall.strings/1`

Folder: `definitions/strings/`. Any `display_name` (and a perk's `description`)
that starts with `@` is a key into these tables; anything else is shown as
written.

```json
{
  "schema": "madfall.strings/1",
  "language": "en",
  "strings": {
    "my_mod.items.machete": "Machete",
    "my_mod.zombies.firefighter": "Firefighter"
  }
}
```

```json
{ "schema": "madfall.item/1", "id": "my_mod:machete", "display_name": "@my_mod.items.machete" }
```

- `language` is a two-letter lower-case code. Keys are lower-case letters,
  digits and `_`, in `.`-separated segments. Prefix your own keys with your mod
  id so they cannot collide with another mod's.
- Lookup is: the current language, then English, then a name made from the
  key's last segment (`@my_mod.items.machete` reads "Machete"). A missing
  translation never shows a raw key.
- Later mods in load order override earlier ones **key by key**. That is how a
  translation pack works: `Mods/example_german/` is a complete example, a
  `mod.json` and one strings file with `"language": "de"` translating base-game
  keys. Anything it leaves out stays English.
- The language follows the `mad.Language` console variable (`mad.Language de`),
  or the operating system when that is empty. `mad.strings` lists loaded
  languages; `mad.strings @some.key` shows what a key resolves to.

### Surfaces - `madfall.surface/1`

Folder: `definitions/surfaces/`. How a material class looks. Every block names a
class in `material.class`; a surface is what re-skins every block of that class
at once (classes that share a material share one mesh section).

```json
{ "schema": "madfall.surface/1", "id": "my_mod:marble", "color": [0.82, 0.80, 0.76], "material": "/MyMod/M_Marble.M_Marble" }
```

- `color` is the base colour `[r, g, b]`, each 0..1, **sRGB** - the numbers a
  colour picker shows divided by 255. It is converted to linear on load and used
  as vertex colour by the default voxel material (and as the tint of model
  blocks without a material). Real materials are darker than they look: grass
  around `[0.30, 0.46, 0.20]`, sand `[0.76, 0.70, 0.50]`.
- `impact` is how every block of the class sounds struck, broken, walked on and
  strained near its structural limit (wood creaks, metal groans, stone grinds):
  `stone` (default), `wood`, `dirt`, `metal` or `foliage`.
- `material` is optional: a content path to a material, usually from your
  content mod's plugin. If it does not load (pak not installed) the log warns
  once and the default material is used. Most shipped surfaces name
  `/Game/Materials/M_MadVoxelPBRArray` with a `texture_layer` (0 Rock030, 1
  Ground048, 2 Grass004 over Ground048 sides, 3 Ground080 sand, 4 Planks021,
  5 Bark012, 6 Metal041B, 7 Gravel022, 8 Fabric066), so a data-only mod can
  re-skin a surface by pointing it at another layer; every layered surface
  draws in one mesh section, which is cheaper than a material of its own.
  `/Game/Surfaces/MI_<Set>` instances also exist for each set. A content mod
  can make its own instance of `/Game/Materials/M_MadVoxelPBR` with
  `BaseColor`, `Normal` (DirectX), `Roughness`, `TileVoxels`, `Tint`,
  `Metallic`, and optional `SideBaseColor`/`SideNormal`/`SideRoughness` with
  `UseSides` 1 for a different texture on sides and undersides.
- `texture_layer` (0-15) selects a layer of the surface texture arrays for
  `M_MadVoxelPBRArray`; it travels in vertex alpha in place of `pattern`, which
  still sets the item icon and the procedural fallback.
- `pattern` picks the procedural pattern the default voxel material draws over
  `color` at 16 texels a voxel: `plain` (default), `stone`, `dirt`, `grass`,
  `sand`, `planks`, `bark`, `leaves`, `concrete`, `brick`, `metal`, `ore`,
  `farmland`, `cloth`, `water` or `gravel`. An unknown name is an error. A
  surface with its own `material` ignores it.
- `cover` grows grass tufts and wildflowers on exposed ground of this surface
  near the player: `{ "density": 0.45, "flowers": 0.02, "height": [0.3, 0.6],
  "color": [0.3, 0.46, 0.2] }`. `density` and `flowers` are chances per column
  (0..1), `height` is in voxels, `color` (sRGB) defaults to the surface's. It is
  decoration only: no collision, not mined, not saved. Shipped on
  `madfall:grass`.
- To re-skin a first-party class, patch it (`"kind": "surface"`, `"op": "set",
  "path": "/material"`). Every block of that class changes, in every world, so
  prefer a new class for a new look: `Mods/example_paint/` adds a
  `example_paint:paint` surface and a Painted Concrete block that uses it.

---

## Content mods (Tier 2)

A content mod is a data mod plus one Unreal plugin holding its assets:

```
Mods/
  my_mod/
    mod.json
    definitions/ ...                 JSON that references the assets by path
    MyMod/
      MyMod.uplugin                  "CanContainContent": true
      Content/                       your .uasset files (source; never shipped)
```

1. **Author** in the MadFall editor. Unreal discovers plugins under the
   project's `Mods/` folder automatically; assets in `MyMod/Content/` have
   content paths starting `/MyMod/`. In the editor they load from source, so
   you can test without packaging.
2. **Reference** them from your definitions by content path, e.g. a surface's
   `"material": "/MyMod/M_Marble.M_Marble"`.
3. **Build** against the game release players run:

   ```powershell
   .\Scripts\PackageMod.ps1 -Mod my_mod -ReleaseVersion 0.1
   ```

   This DLC-cooks only your plugin against `Releases/0.1` (written by
   `Scripts/Package.ps1`) and writes `Saved/ModBuilds/my_mod/`: `mod.json`,
   `definitions/`, and `MyMod/` containing the `.uplugin` and
   `Content/Paks/Windows/*.pak|.utoc|.ucas`. Your source assets are not included.
4. **Install** by copying `Saved/ModBuilds/my_mod/` into the packaged game's
   `MadFall/Mods/`. The game mounts the container, registers `/MyMod/` and loads
   its shader library itself.

Rules and limits:

- The plugin name is your content root and must be unique across mods. The mod
  folder name is baked into the cooked paths, so install it under its own id.
- A mod is cooked against one release. A game packaged with a new
  `-ReleaseVersion` needs mods rebuilt.
- Only Windows builds are packaged today.
- `mod.json`'s `paks` list is for raw paks you build yourself; a plugin content
  mod does not need it.

`Mods/example_paint/` is complete: a plugin with one material (`/ExamplePaint/M_Paint`)
used by its own `example_paint:paint` surface and a Painted Concrete block
(`mad.player.give example_paint:painted_concrete 10`). CI packages the game,
builds the mod, runs the packaged exe and checks the surface renders with the
mod's material.

## Script mods (Tier 3)

A script mod is Lua 5.4. List the files in `mod.json`; they load in that order,
after the world and its save:

```json
"scripts": ["scripts/main.lua"]
```

A script's top level runs once. Use it to register event handlers and commands;
do world work inside handlers. `Mods/example_scripted/` is a complete example
(kill bounties, horde-night supplies, a beacon command, a sandbox self-test).

```lua
madfall.on("zombie_killed", function(event)
	local kills = (madfall.store_get("kills") or 0) + 1
	madfall.store_set("kills", kills)
	if kills % 25 == 0 then
		madfall.give("madfall:arrow", 10)
		madfall.message("Bounty: " .. kills .. " kills.")
	end
end)

madfall.command("home", function(args)
	local p = madfall.player()
	if p then madfall.log("standing at " .. p.x .. " " .. p.y .. " " .. p.z) end
end)
```

### The `madfall` table

| Function | Does |
|---|---|
| `madfall.on(event, fn)` | Calls `fn(event)` for every event of that name. An unknown name is an error. |
| `madfall.command(name, fn)` | Adds the console command `mod.<your id>.<name>`; `fn(args)` gets the words after it. Names are `a-z`, `0-9`, `_`. |
| `madfall.log(...)`, `print(...)` | A line in the game log, tagged with your mod id. |
| `madfall.message(text)` | A line on the survivor's HUD. |
| `madfall.get_block(x, y, z)` | Block id at a voxel, or `nil` if the chunk is not loaded. |
| `madfall.set_block(x, y, z, id)` | Writes a block, as if placed (`madfall:air` clears). `false` for an unknown id or an unloaded chunk. The structural solver treats it like any placed block. |
| `madfall.player()` | `nil` while there is no survivor, else `{x, y, z, health, max_health, food, water, core_temperature, level, alive}` (x, y, z: the voxel the feet are in). |
| `madfall.give(item, count)` | Gives the survivor items (1-999); what does not fit drops. Returns how many, 0 for an unknown item. |
| `madfall.spawn_zombie(id, x, y, z)` | A zombie standing near a voxel. `false` if none could be placed or the zombie cap is reached. |
| `madfall.time()` | `day, hour` (day 1-based, hour 0-24). |
| `madfall.store_get(key)`, `madfall.store_set(key, value)` | Your mod's per-world store, saved in `gameplay.json`. Values: `nil` (deletes), booleans, numbers, strings up to 1024 bytes. Keys 1-64 bytes, 256 per mod. |
| `madfall.mod_id`, `madfall.api_version` | Your id; the scripting API version (1). |

### Events

Every event table has `name`. Whole numbers arrive as integers.

| Event | Fields | When |
|---|---|---|
| `world_loaded` | `day`, `hour` | Once, after every script mod has loaded. |
| `second` | `day`, `hour` | Every real second. |
| `dawn`, `dusk` | `day`; dusk also `horde` | 06:00 and 22:00. |
| `horde_night` | `day` | A horde night begins. |
| `player_spawned` | `x`, `y`, `z` | The survivor stands in the world (first spawn, respawn, travel). |
| `player_died` | | The survivor dies. |
| `block_placed`, `block_broken` | `block`, `count`, `x`, `y`, `z` | By the survivor (not by collapses or scripts). |
| `bed_set` | `block`, `x`, `y`, `z` | A bed becomes the respawn point. |
| `item_crafted` | `item`, `count` | A craft finishes. |
| `item_worn` | `item` | Clothing put on. |
| `zombie_killed`, `animal_killed` | `zombie` / `animal` | By the survivor. |
| `quest_completed` | `quest` | |

Handlers run on the game thread, up to a frame after the event: events are
queued and run inside the frame budget, so a script never runs in the middle
of the game's own update. Test handlers with
`mad.scripts.event <name> key=value ...`.

### The sandbox

- Each mod has its own Lua state: mods cannot see each other's globals.
- Available: `string`, `table`, `math`, `utf8`, `coroutine` and the base
  library without `load`, `loadfile` and `dofile`. Not available: `io`, `os`,
  `package`/`require`, `debug`, `string.dump`, precompiled chunks. A script
  cannot touch files, the network or other programs.
- A handler or command gets 1 ms. Past that it is stopped with an error, even if
  it catches errors with `pcall`.
- 16 MB of memory per mod. Past that, allocations fail with "not enough memory".
- 256 `set_block` and 4 `spawn_zombie` per handler call.
- After 20 errors a mod's scripts are disabled until the game restarts.
  `mad.scripts` shows each mod's handlers, errors, memory and slowest call.

Script errors go to the log as `Script error [your_id]: ...` with a traceback.

---

## Saves and removing mods

- Blocks and items are saved by id, never by number. A save loads with a mod
  removed: its blocks stay in the world as inert placeholders that do not
  collapse and cannot be destroyed, and its items stay in their slots. Reinstall
  the mod and everything is back, byte for byte.
- Adding a biome or prefab changes only terrain generated after it is installed.
- A script mod's store is kept when the mod is removed and is there again when it returns.

---

## Status

| Tier | State |
|---|---|
| 1 - data | **Usable**: manifests, dependency and load-order resolution, blocks, biomes, prefabs, items, recipes, loot, zombies, animals, quests, survival tuning, perks, text and translations, patches, the `example_scavenger` and `example_german` examples, automation tests (`MadFall.Mods.*`, `MadFall.Progression.*`). Translations are plain key/text: no plural or gender rules yet. |
| 2 - content | **Usable**: content plugins inside mods, DLC-cooked by `Scripts/PackageMod.ps1` against a release from `Scripts/Package.ps1`, mounted by the packaged game from `Mods/`; surfaces let JSON point material classes at mod materials; `example_paint` is built and verified in the packaged exe by CI gate 11. What assets can *do*: materials (surfaces) and static meshes (`model` blocks); not yet sounds, or textures on their own. Windows only. |
| 3 - script | **Usable**: Lua 5.4 per-mod sandboxes with time, memory and world-edit caps; 15 events, console commands, block queries and edits, items, zombie spawns, a saved per-mod store; the `example_scripted` example; tests `MadFall.Scripts.*` and a CI gate that plays the example in a real game. Not yet: script access to definitions, UI beyond HUD lines, per-block `mod_data`. |

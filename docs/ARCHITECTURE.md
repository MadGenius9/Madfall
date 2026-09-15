# MadFall — Architecture

Photoreal voxel survival sandbox. Minecraft build/mine loop, 7 Days to Die
structural integrity and horde pressure, AAA fidelity, mod-first.

This document is the plan of record. It is written as systems are built, not
after. If the code and this document disagree, that is a bug in one of them.

---

## Status

| Phase | Scope | State |
|---|---|---|
| 0 | Repo scaffold, modules, server target, CI, empty test map | **Complete** |
| 1 | Voxel volume + moddable block registry | **Complete** |
| 2 | Meshing + rendering | **Meshing complete; rendering fidelity partial** |
| 3 | World generation | **Complete** (surface scatter pending art) |
| 4 | Survival + structural integrity | **Playable loop complete** — structural integrity, damage, debris, items/crafting/loot, survival (GAS), player, zombies, horde nights; polish gaps listed below |
| 5 | Mod API (three tiers) | **Tier 1 complete; Tier 2 content plugins; Tier 3 Lua scripts** - see docs/MODDING.md |
| 6 | Multiplayer + dedicated server | **Deferred — single-player scope, see below** |
| 7 | Polish | Not started |

### Phase 0 delivered

- `MadFall.uproject` with five C++ modules, Engine 5.8 association.
- `MadFall`, `MadFallEditor`, `MadFallServer` targets.
- Git repository with Git LFS configured for every binary asset type.
- `Scripts/Build.ps1`, `Scripts/CI.ps1`, `.github/workflows/ci.yml`.
- `Content/Maps/L_MadFall_Test.umap`, generated headlessly by
  `Scripts/make_test_map.py`.
- Two automation tests covering the voxel struct layout and the mod API version
  contract; both green.
- Editor and client targets build with zero warnings.

### Phase 1 delivered

- `FMadChunkStorage` — SoA + palette chunk storage with lazily allocated side
  arrays, bit-tier re-packing and save-path compaction.
- `FMadBitPackedArray` — 1/2/4/8/16-bit index packing.
- `FMadBlockRegistry` — namespaced-id registry with reserved ids, `extends`
  inheritance, load-order override warnings, deterministic runtime-id
  assignment, and stable placeholders for absent mods.
- JSON block definition loader with per-field typed errors (file + JSON pointer
  + expected type + actual value) and unknown-field reporting.
- `UMadBlockDefinition` data asset carrying the identical struct.
- `FMadChunkSerializer` — TLV sections, run-length encoding, transient-flag
  stripping, deterministic output.
- `FMadRegionFile` — 4096-slot region files, memory-mapped reads, copy-on-write
  sector allocation, CRC-verified payloads, LZ4 compression, region string table.
- `UMadVoxelWorldSubsystem` — loaded chunk map, region cache, the single
  `SetVoxel` edit funnel, synchronous and asynchronous load paths, save/flush.
- 16 first-party block definitions in `Definitions/blocks/`, loaded through
  exactly the same JSON path a mod uses.
- 11 console commands (`mad.blocks`, `mad.chunk.*`, `mad.voxel.*`,
  `mad.world.*`, `mad.region.info`).
- 11 automation tests, all green, including the mod-removal round trip.

Verified end to end on 2026-09-12: filled chunk (0,0,0) with `madfall:stone`,
placed a `madfall:rebar_concrete` at orientation 19 / variant 5 and a
`madfall:steel_beam` at orientation 3 / variant 2, saved, unloaded, reloaded,
and read back both blocks with orientation and variant intact. The 32768-voxel
chunk wrote **111 payload bytes**. Memory was 73,928 bytes: 32 KiB rotation +
32 KiB flags + 8 KiB indices — the density array was never allocated, because
every voxel matched the chunk default.

### Phase 2 delivered

The hybrid geometry pillar, working:

- **Surface Nets** for natural terrain. One vertex per cell the surface crosses,
  placed at the average of the cell's edge crossings. Normals come from the
  density gradient over the cell's own 8 corners rather than from surrounding
  triangles — a gradient is identical whichever chunk computes it, so two
  chunks agree on the normal at a shared seam and no crease appears.
- **Greedy cubic meshing** for player construction. Rectangles merge along both
  slice axes; a 32×32×1 slab comes out as 12 triangles instead of 4096.
- **One bit separates them.** `EMadVoxelFlags::Cubic` routes a voxel to one
  path or the other. The isosurface treats cubic voxels as empty, so a placed
  block does not bulge the terrain around itself.
- `FMadChunkSampleGrid` — a 34³ snapshot of a chunk plus one voxel of margin
  from each neighbour, taken on the game thread so the worker touches nothing
  shared. The margin is what makes chunk boundaries watertight.
- `UMadChunkMeshSubsystem` — dirty-set coalescing, bounded concurrency,
  per-frame budgets for the two game-thread steps, and neighbour invalidation
  on boundary edits.
- `UMadChunkMeshComponent` over `UProceduralMeshComponent` with
  `bUseAsyncCooking`, sections keyed by block **material class** so twenty
  concrete variants share one draw call.
- 4 automation tests, plus `mad.mesh.stats`, `mad.mesh.inspect`,
  `mad.mesh.rebuild`, `mad.mesh.rebuildall`.

**Measured**, on the RTX 3080 / i7-12700F development machine:

| | |
|---|---|
| Worst-case chunk mesh (surface through most of it) | 2.0–2.4 ms, **worker thread** |
| Sample-grid snapshot | 0.24–0.50 ms, game thread |
| Component apply, steady state | 0.47 ms, game thread |
| Component apply, first creation of a component | 3.9–6.7 ms, game thread — **over the 2 ms budget** |
| A typical terrain chunk | ~1,600 verts / ~3,000 tris |

#### Winding: proved, not assumed

Unreal computes a triangle's face normal as `(P1-P2) × (P0-P2)`
(`KismetProceduralMeshLibrary::CalculateTangentsForMesh`). Expanding that shows
it is the **negation** of the conventional right-handed CCW normal, because
Unreal is left-handed. Every quad the mesher builds is CCW by the right-hand
rule, so the mesher flips winding in exactly one place — `AddOrientedQuad` —
and `MadFall.Mesher.GreedyCubic` asserts against Unreal's own formula that no
triangle ends up backfacing. Getting this wrong renders solid geometry as
invisible holes, and "it looked right in one screenshot" is not evidence.

### Phase 3 delivered

Layered, deterministic, seed-based terrain, infinite in X/Y.

```
continental shape  ->  climate (temperature, moisture)
                   ->  biome selection and blending
                   ->  terrain height
                   ->  3D density field with domain warp
                   ->  caves
                   ->  surface composition
                   ->  ores
```

- **`MadFall::Noise`** — a seeded integer-hash gradient noise with fBm and
  ridged variants, plus a per-chunk RNG. Not `FMath::PerlinNoise3D`: the
  engine's has a fixed permutation table and no seed, so every MadFall world
  would have had the same terrain viewed from a different offset.
- **Biome definitions are data**, in `Definitions/biomes/*.json`, loaded through
  the *same* loader, the same validation and the same `extends` inheritance as
  blocks. Eight ship; `madfall:base_land` is a weight-0 template the rest
  inherit from. A mod adding a biome writes JSON and nothing else.
- **Biome blending.** Each column scores every biome's climate fit, takes the
  top three, and interpolates their finished heights — a plains/mountain border
  is a slope, not a cliff.
- **3D domain warp** displaces the surface by a 3D noise field, which is the
  only way a heightfield can fold back over itself into an overhang. A pure
  heightmap is a function of (x, y) and mathematically cannot make one.
- **Ridged 3D noise caves**, faded in over the threshold so cave walls are
  smooth surfaces rather than voxel steps, and stopped a margin below the
  surface so they do not open the terrain skin everywhere.
- **Ore veins** placed by a random walk seeded from the chunk coordinate alone,
  so a chunk's ore is identical whether it is the first or the ten-thousandth
  generated, and only ever replacing that biome's stone.
- **Bedrock is unconditional** below `BedrockTop`, so no cave or overhang can
  open a hole out of the bottom of the world.
- Console tooling: `mad.biomes`, `mad.world.seed`, `mad.world.loadarea`,
  `mad.worldgen.probe`, `mad.worldgen.survey`.
- 5 automation tests: noise properties, determinism, terrain shape, ores,
  performance.

**Measured**, seed 20260912:

| | |
|---|---|
| Chunk generation | **3.4–4.1 ms**, worker thread |
| 507 chunks generated | 2.05 s (4.05 ms each) |
| 507 chunks meshed | 832 ms (1.64 ms each), 316,828 verts / 564,088 tris |
| Voxel memory, 363 chunks | 18.0 MiB (~50 KiB/chunk, matching the storage table) |
| Surface height over 4096² | −9.0 … 62.4 |
| Underground carved by caves | 8.99% |
| Biome mix over 3072² | ocean 25.7, tundra 21.4, plains 17.5, highlands 16.2, beach 11.6, forest 5.3, desert 2.1 |

#### Two bugs the survey tooling caught that a screenshot never would

**1. Unreachable biomes.** fBm sums octaves of decreasing amplitude and divides
by their total, which clusters the output tightly around its mean. Measured
over 37,249 columns, continentalness spanned only **0.31 … 0.74** and
temperature **0.15 … 0.64**. Any biome whose climate range fell outside that
band could never be selected: `highlands` (continentalness ≥ 0.78) and `desert`
(temperature ≥ 0.70) covered *exactly 0%* of the world, and nothing in the
biome histogram said why — they simply were not listed. Fixed by
`FMadWorldGenSettings::ClimateContrast`, which expands the signed field before
clamping. `MadFall.WorldGen.Terrain` now asserts each field spans most of
[0, 1], and that every selectable biome appears somewhere in a 12288² area.

**2. Biome identity washed out by blending.** Linear weight normalisation turned
scores of 1.0 / 0.9 / 0.8 into a 37/33/30 blend — barely a preference. Every
column became an average of three biomes and no biome's distinctive terrain ever
appeared: mountains topped out at 38 when the highlands base height alone was
40. Fixed by cubing scores before normalising (51/37/26), which still blends
smoothly across a border but lets a biome be itself in its middle. Mountains now
reach 62.

Also: `Score()`'s falloff is now proportional to the range's own width. A flat
0.25 falloff made a beach claiming a 0.12-wide continentalness band score above
zero across 0.09 … 0.71 — most of the world — and beach covered 20% of the map.

### Scope decision, 2026-09-12: single-player first

**MadFall is single-player for now.** The dedicated server is deferred to Phase
6 along with the rest of multiplayer. This supersedes the brief's "dedicated
server target from day one".

Consequences, deliberately chosen:

- `Source/MadFallServer.Target.cs` **stays in the repo**. It is 30 lines, it
  costs nothing to keep, and deleting it means re-deriving it later. It is not
  built and not gated.
- `Scripts/CI.ps1` reports the server build as SKIPPED. `-RequireServer` turns
  it back into a hard gate, and at that point the runner needs a **source**
  engine build.
- No source engine build is needed for Phases 1–5. The Launcher install is
  sufficient.
- **The protection that actually mattered is not lost.** The value of building
  the server early was catching editor-only includes leaking into runtime code.
  The client target (`MadFall`) already excludes `MadFallEditor`, so that leak
  is still a compile error in CI on every commit. What is deferred is coverage
  of `WITH_SERVER_CODE` / `!WITH_EDITOR` paths specifically.
- Phase 1 still routes voxel edits through a single validated apply path rather
  than mutating chunks from arbitrary call sites. That shape costs nothing in
  single-player and is what makes Phase 6 server authority an addition rather
  than a rewrite. It is *not* a replication implementation — none is built.

### Phase 3 delivered, part 2: POIs and roads

```
world  ->  POI cells (8x8 chunks each, at most one POI per cell)
       ->  per cell, from hash(seed, cell): prefab choice, site, yaw
       ->  each chunk stamps only the slice of its own cell's POI it contains
       ->  roads: each cell owns a road to its +X and +Y neighbour's entrance
```

- **`madfall.prefab/1`**, JSON, in `Definitions/prefabs/` and
  `Mods/<id>/definitions/prefabs/`. JSON rather than an editor asset is the
  canonical form, deliberately: a modder must be able to ship a ruin without the
  editor, and a captured building must be reviewable in a diff. Voxels are
  run-length encoded; four shipped prefabs are 1.6–3.3 KB each.
- **Void vs air.** Palette token `"*"` leaves terrain alone; `madfall:air`
  carves. Without the distinction every tower would stand in a square pit dug
  to the size of its widest floor.
- **Markers** are typed by `FName`, not an enum: `loot`, `spawn` and `entrance`
  are documented, and a mod can add a `trader` marker without recompiling.
  Out-of-bounds markers are dropped with a located error.
- **The 24 cube orientations are derived, not typed.** Six base matrices, spin
  applied in the block's local frame, and a 24×4 yaw composition table built by
  matrix multiplication at startup. A hand-typed table has 96 entries in which
  one wrong digit silently mis-rotates one block type in one direction.
  `MadFall.WorldGen.Orientation` asserts the 24 are distinct proper rotations,
  the up-face labels are what they claim, the table agrees with actual matrix
  multiplication, and — the one that would otherwise ship broken — that
  footprint rotation turns the *same way* as block yaw.
- **Cell-based placement.** A building spans chunks that generate independently,
  on any thread, in any order. The only way to stamp it consistently is for
  every chunk to re-derive the same plan from the seed, so placement is never
  stored. Cells align with chunk boundaries and a POI never leaves its cell, so
  a chunk only ever asks its own cell. Plans are cached behind a reader-writer
  lock because every chunk in a cell asks the same question.
- **Site selection** samples a 3×3 height grid across the footprint, rejects
  sites steeper than the prefab's `max_slope`, and uses the median so one spike
  cannot lift the whole building. Five attempts per cell, so a mostly-hillside
  cell still finds its shelf.
- **Difficulty tier rises with distance from spawn.** A cell's tier caps which
  prefabs it may host and is stamped onto every marker, so a tier-1 cabin found
  far out still carries far-out loot.
- **Foundations** fill down to the ground under every solid column of a
  prefab's bottom layer, using `IsTerrainSolid` — a pure function of the
  coordinate that shares its implementation with `GenerateChunk` — so every
  chunk a foundation spans agrees where it stops.
- **Roads** connect neighbouring POI entrances with a sine-envelope meander
  (zero at both doorways), follow smoothed terrain, blend into each building's
  floor over the last 16 voxels, and skip any route that would cross open water
  rather than faking a causeway.
- `mad.prefabs`, `mad.poi.cell`, `mad.poi.near`, and **`mad.prefab.capture`**
  — build something in the world, capture a box of voxels to prefab JSON.
- 4 more automation tests: orientation, prefab format, placement/stamping, roads.

**Measured**, seed 20260912: 25 POIs in the 49 cells around spawn; chunk
generation unchanged at **3.43 ms** mean with POIs and roads enabled.

#### Three bugs found by making a test stronger

**1. One sample proved almost nothing.** The first stamping test verified a
single POI — which happened to be a 7×7 watchtower spanning two chunks. That
exercised neither the large prefabs, nor all four yaws, nor a building cut by a
chunk corner. The test now sweeps three 5×5-cell areas at increasing distance
(tier rises with distance, and the big prefabs only appear far out) and asserts
it covered all four yaws and at least three prefabs: **31 POIs, 15,686 voxels,
all 4 prefabs, all 4 yaws**, every voxel at its rotated position with its
rotated orientation.

**2. Floating foundations.** The stronger test immediately found **4 floating
bottom columns** across 11 POIs. The foundation asked the pure terrain function
whether ground existed under the base — and that function knows nothing about
caves or road cuts. Warp can lift the local surface far enough that the cave
margin no longer protects the voxel under a building, and a road's clearance can
cut beneath a doorway. Fixed by always considering the voxel directly under the
base and filling whatever is genuinely empty *in that chunk*: the range comes
from the pure function, the emptiness test is local to each voxel's own chunk,
so independently generated chunks still agree.

**3. Roads rendered as notched trenches.** Written as a hard 255-over-0 density
step at an integer height, a road produced a stair every time the terrain
crossed a voxel boundary. Roads now use the terrain's own continuous density
band (`MadFall::DistanceToDensity`, extracted so both share one implementation)
and a float height.

### Phase 4 delivered, part 1: structural integrity, block damage, debris

All in `MadFallGameplay`. The solver is pure (`FMadStructuralJob` over an
`IMadStructuralWorld`), so the tests run it against an in-memory world and the
live subsystem runs the identical code against the voxel world.

```
SetVoxel ──OnVoxelChanged(pos, before, after)──▶ UMadStructuralSubsystem
                                                   │ filter: role / block / damage-STAGE changed?
                                                   ▼
                                     one FMadStructuralJob, time-sliced (mad.si.BudgetMs = 1.0)
                                     Gather (reads world) → Solve (Dijkstra) → Load (reverse sweep)
                                                   │ failures, re-verified, removed via SetVoxel
                          OnStructureCollapsed ◀───┘  (which fires OnVoxelChanged → cascade)
                                   ▼
                        UMadDebrisSubsystem: 6-connected clusters fall on the grid,
                        land → crush damage (ApplyBlockDamage) + rubble
```

#### The model

| Voxel | Role |
|---|---|
| Solid + `Cubic` + definition not `is_anchor` | **member** |
| Solid, not `Cubic` (natural terrain, rubble) | anchor |
| `is_anchor` definition, unresolved block from a missing mod, unloaded chunk, below the world | anchor |
| Air, liquid, density < 128 | empty |

**Span.** Each member's *support distance* is the cheapest path from any anchor.
Resting on the block below costs 0; receiving support from the side costs
`720720 / effective_span(receiver)`; hanging below costs the same times
`HangingCostMultiplier` (1.0). `effective_span = max_horizontal_span × stage
support_multiplier`. Distance > 720720 is unsupported. 720720 = lcm(1..16), so
every shipped span divides exactly: concrete (span 6) holds a 6-block cantilever
and drops the 7th, with no rounding drift. Costs add along the path, so wood hung
off a steel cantilever reaches less far than wood off a column.

**Load.** Load flows back down the shortest-path DAG: each member splits its own
mass plus what it carries equally among its *tight* supporters (`dist(u) +
cost(u→v) == dist(v)`). A member fails when it carries more than `support_strength
× stage multiplier`. Dijkstra settles in `(distance, z)` order; zero-cost edges
only point up, so that order is topological and a single reverse sweep is exact.

**Progressive failure.** A solve removes what fails *now*. The removal is itself
a voxel change, so the next solve runs on the new structure. A buckled column
drops the floor above on the next pass, not the same one — cheaper, and it reads
as a collapse.

**Tradeoffs, stated.**

- *Not FEM.* A stiffness solve gives real stress at O(n^1.5)+ with a linear solver
  that has to converge on degenerate voxel graphs, and its failures are hard to
  explain. This is O(n log n), integer-exact, resumable mid-solve, and every
  outcome is one sentence a player can build against.
- *Terrain never collapses.* Caves and overhangs are anchors. Making terrain
  structural turns every mining trip into a physics job over millions of voxels;
  the pillar is that *bases* take damage.
- *Unloaded chunks are anchors.* The alternative collapses half a building when
  its other half streams out.
- *Game thread, time-sliced, not a worker.* The result must be applied against
  the world as it is when the job finishes; a worker would need a snapshot of an
  arbitrarily large structure, itself an O(n) game-thread copy. Solve and Load
  never touch the world, so moving them to a worker later is local.
- *Damage-byte noise is filtered.* A zombie chewing a wall changes `Damage` every
  hit; only a change of damage **stage** re-solves.
- *Structures over 65,536 members are left standing* and reported, rather than
  spending seconds on one edit.

#### Block damage

`hardness` is hit points; `FMadVoxel::Damage` is the fraction lost (/255),
rounded *up* so a landed hit is never discarded. `resistances` multiply by damage
type (missing = 1.0, 0 = immune). Crossing a stage with `downgrade_to` turns the
voxel into that block at zero damage **with the overflow carried in**, so a big
explosion is never less effective against rebar than two small ones. Unresolved
blocks take no damage — removing a mod must not make a base destructible.

#### Falling debris

Collapsed voxels are grouped into 6-connected clusters that fall straight down on
the grid (columns over open air shear off a landing cluster and fall on; gap 9) (semi-implicit Euler, 1 voxel = 1 m, landing speed
rewound to the exact contact). On landing, `½mv²` is split over every bottom face
resting on something and dealt as `madfall:crush` damage at
`mad.debris.DamagePerKJ` (10 HP/kJ: one 1800 kg block dropped a storey is ~700 HP,
which breaks a concrete frame). Crush damage goes through the same damage path,
so debris that breaks a support starts the next collapse. Roughly one block in
`mad.debris.RubbleKeepOneIn` (3) becomes its `debris_on_collapse` block, compacted
per column onto the landing surface. Visuals are an instanced cube per voxel on a
transient actor, capped at 64 falling clusters.

*Added in Phase 5:* a cluster whose bottom blocks sweep through a character's
voxels (checked over the whole interval fallen that tick, so fast debris cannot
skip over anyone) hits it once with `½mv²` of the (x, y) column above the contact
at `mad.debris.PawnDamagePerKJ` (3 HP/kJ: a wood frame falling 3 m costs ~22
health, a concrete block from a storey kills). Every collapsed block that did not
become placed rubble rolls its `drops.on_collapse` table once; results are
merged and left as one pickup bag on top of the pile, seeded from the cluster's
origin so the same collapse always leaves the same salvage. Writing this found
that `wood_debris` and `steel_debris` were referenced but never defined, so wood
and steel had silently never left rubble; `MadFall.Items.ShippedContent` now
checks every `debris_on_collapse` reference. Tests:
`MadFall.Structural.DebrisPawnsAndDrops`; the survival gate drops a slab on the
survivor (`mad.player.overhead`).

*Checked on load (Phase 5):* every chunk that arrives has its structural members
seeded (`mad.si.CheckOnLoad`, on by default), so a building that is unsound when
it loads falls instead of standing until someone touches it.
`MadFall::Structural::FindMembersInChunk` skips a chunk in O(palette) when no
palette entry can be a member (terrain and road blocks are `is_anchor`), and in
O(1) when the chunk has no Cubic flag at all; a chunk with construction scans in
0.016 ms. The first run of it in a fresh-ish world dropped 131 blocks of a
watchtower near spawn: its **wood-frame foundation** was overloaded by its own
posts (16.8 t on a 16 t rating) wherever the ground sloped. The shipped-prefab
test only stood POIs on flat ground, where no foundation is generated, so it
never saw one loaded. `MadFall.Structural.ShippedPrefabsStand` now also stands
every prefab on slopes along X and Y at its `max_slope` and on stilts at its
`max_foundation_depth`, with foundation filled by the planner's rule; the
watchtower's foundation is now `wood_reinforced`. A fresh world then loads with
0 collapses and a worst structural frame of 0.15 ms. Test:
`MadFall.Structural.ChunkSeeds`.

`-MadWorld=<name>` (letters, digits, `-`, `_`) runs against
`Saved/MadFallWorlds/<name>` instead of `DevWorld`, which is how probes like this
one avoid mutating the world CI uses.

*Why kinematic and not Chaos:* rigid bodies tumble nicely but are
non-deterministic, cost a body per piece, and land at transforms that must be
snapped back to the grid before they can damage anything. Gameplay outcome is
computed here; cosmetic tumbling can be layered on the visual actor.

#### Content retune the solver forced

The first run of `MadFall.Structural.ShippedPrefabsStand` failed **every** shipped
POI: `support_strength` values (wood 800 kg, concrete 5,000 kg) could not carry
the buildings' own weight — the bunker's corner columns carry 107 t. They were
never exercised before a solver existed. Retuned with ~3–4× margin over the worst
shipped load, so damage stages (0.2–0.6) are what bring buildings down:

| Block | support_strength | span |
|---|---|---|
| wood_frame | 800 → **16,000** | 2 → **5** |
| wood_reinforced | 1,800 → **40,000** | 3 → **6** |
| concrete_frame | 5,000 → **400,000** | 6 |
| rebar_concrete | 9,000 → **900,000** | 9 |
| steel_beam | 22,000 → **2,500,000** | 14 |

Wood span went up because a 9×11 cabin roof over a window lintel needs 5, and
7DTD wood reaches about that far. The watchtower's roof was only held by rim
corner posts 6+ steps from the centre; `make_sample_prefabs.ps1` now continues the
main posts up to the roof.

#### Measured

| | |
|---|---|
| Solve, 4,104-member concrete lattice, one edit | **1.5–1.9 ms total**, 0.4–0.5 µs/member |
| Worst 256-unit slice | 0.11–0.15 ms |
| Shipped POIs | cabin 410, outpost 1,025, bunker 466, watchtower 216 members — 0 failures |

#### Tests and gates

`MadFall.Structural.*`: `Span` (exact span, mixed materials, bridge losing a
column), `Support` (removal, neighbour seeding, hanging, unloaded = anchor,
unknown id = anchor), `DamageStages`, `Load` (overload at the base only, even
split across two columns), `Determinism` (2,164-member random scaffold solved
whole vs in 7-unit slices: identical failures in identical order),
`Performance`, `ShippedPrefabsStand`, `BlockDamage` (rounding, stages, immunity,
downgrade with overflow, through-destroy), `Debris` (clustering, fall speed,
self-support, landing on a pillar, rubble determinism/compaction).
CI gate 8 drives the live chain: build a cantilever by console, check the 6th
block stands and the 7th fell, blow out the column, check everything above came
down and debris landed with impact energy.

Console: `mad.si.status|flush|enable|inspect x y z|check x y z [r]`,
`mad.damage x y z amount [type]`, `mad.debris.status|flush`.

### Phase 4 delivered, part 2: the survival loop

The brief's Phase 4 deliverable - *gather, craft, build, defend a night horde,
and have the base actually take structural damage* - runs end to end, and CI
plays it headless (gates 9 and 10).

```
MadGameMode ── AMadPlayerCharacter (ACharacter + GAS)
                 │ verbs: UsePrimary / UseSecondary / Interact / CraftRecipe
                 │   (input bindings, console commands and CI all call these)
                 ├─ UMadInventoryComponent  (FMadInventory, 36 slots, 9 hotbar)
                 ├─ UMadSurvivalComponent   (MadFall::Survival::Step over UMadSurvivalAttributeSet)
                 └─ voxel raycast ──▶ Harvest rules ──▶ UMadStructuralSubsystem::ApplyBlockDamage
UMadChunkStreamingSubsystem   loads around players, unloads past hysteresis
UMadWorldClockSubsystem       time of day, day count, horde every 7th night, sun
UMadContainerSubsystem        block.container voxels; loot rolled on first open
UMadHordeSubsystem            horde waves, POI sleepers, night wanderers, live cap
AMadZombie (ACharacter + GAS) senses → voxel A* (digs) → attack / break blocks
```

#### Definitions: items, recipes, loot, zombies

All four are JSON through the shared reader, loaded from `Definitions/<kind>/`
and every mod's `definitions/<kind>/` by `MadFall::Definitions::ForEachSource`
(the one place Phase 5's load order will plug in), into one registry
(`FMadGameplayDefinitions`, MadFallCore) because they are validated against
each other.

| Schema | Notes |
|---|---|
| `madfall.item/1` | kinds resource/block/tool/weapon/consumable/mod; `extends` supported (tool families); tools carry damage map, harvest tags, tier, durability, use time, stamina, reach, mod slots, repair cost; consumables carry stat effects; mods carry multipliers |
| `madfall.recipe/1` | output, ingredients, station (a block id within 3 voxels), craft seconds, required level |
| `madfall.loot/1` | ids may contain `/`; weighted entries of items or nested tables; tier window, min game stage, count and durability ranges, empty weight, extra rolls per tier |
| `madfall.zombie/1` | stats, senses, rewards, appearance, and the spawn groups it belongs to |

Load-time validation, each with a file and JSON pointer: unknown cross-references
(recipe ingredients, loot entries, repair items, zombie loot tables) disable the
entry; loot-table nesting cycles are broken; stackable tools are forced to
`max_stack` 1 when explicitly set otherwise; unknown fields are reported.
**Every placeable block gets an item automatically** (except liquids and
`block.indestructible`), so a mod adding a block gets a craftable, placeable,
lootable item with no item file.

#### The rules, as pure functions with tests

- **Inventory** (`FMadInventory`): all-or-nothing removal, stacks merge only when
  nothing per-item (durability, mods) would be lost, removal takes from the end
  so the hotbar is eaten last.
- **Crafting**: removes ingredients and adds output on a copy, commits only if the
  output fits - a craft never eats ingredients and drops the result.
- **Loot**: deterministic for a stream seed. Containers are seeded from world seed
  and position, and rolled at the game stage of first opening (days + level), so
  a crate opened on day 20 beats day 1 and nothing is stored until opened.
- **Harvest**: matching tool tag and tier = full damage + drops; tier too low =
  half, no drops; wrong tool = quarter, no drops. Damage type is the tool's best
  after block resistance. Durability mods scale the chance a use costs a point.
- **Survival** (`MadFall::Survival::Step`): food/water are satiety, core
  temperature drifts toward ambient discomfort (a countdown, not an instant
  penalty), infection grows once contracted and damages above 50, natural
  healing only when well fed. Frame-rate independent (sub-stepped to 1 s).
- **Pathfinding** (`MadFall::Pathfinding::FindPath`): A* for a 2-tall walker on
  voxels - walk, step up, drop up to 3, and **dig** as an ordinary edge costing
  break-seconds. A zombie walks round a wall if that is shorter than digging and
  otherwise digs the *weakest* section. Budgeted; returns the best partial path.

**Why GAS holds the numbers but not the simulation.** Survival attributes live
in `UMadSurvivalAttributeSet` so buffs, armour and perks are Gameplay Effects a
mod can ship. The per-second simulation is not a periodic effect: it is the pure
`Step` above, which keeps balance maths testable without an ability system.

**Why voxel A\* and not navmesh.** Recast rebuilds lag edits by seconds and have
no notion of "this wall is wood, that one concrete", which is the whole point of
a horde night.

**Why no Behavior Tree yet.** Zombie AI is four states in the pawn tick with a
staggered think. A BT asset would add an editor dependency for no expressive
gain at this size; the verbs are what BT tasks would call when variants grow
tactics.

#### Headless play: `mad.onspawn`

`-ExecCmds` runs before the world streams in, so `mad.onspawn cmd; wait 2; cmd`
queues commands that the player drains once standing on the ground. CI gate 9:
craft a pickaxe, mine (durability drops), place a block and a crate, loot it,
eat, die, respawn. Gate 10: a zombie reaches and hits the player; a horde zombie
digs through a wood-frame shelter and hits again; setting the clock to 21:58 on
day 7 starts horde night and spawns a wave.

#### Measured (headless, dev box)

| | |
|---|---|
| Spawn to standing (streaming + mesh + collision) | 1.1–1.4 s |
| Path into a sealed room 30 voxels away, digging the wooden side | 402 nodes, 0.15 ms |
| Automation tests / CI gates | 43 / 10, all green (server build skipped by scope) |
| Wood-frame shelter breached by one civilian | < 40 s (14 block hits) |

#### Content shipped

16 hand-written items (+ generated block items), 14 recipes including workbench
and forge stations, 22 loot tables covering every block drop table and POI
marker, 3 zombie variants (civilian, soldier from game stage 8, brute from 16).
Prefabs now place a `madfall:loot_crate` on every loot marker. Starting kit:
6 planks, 4 rocks, 2 food, 2 water - enough for a first pickaxe.

### Phase 4 delivered, part 3: persistence, pickups, timed crafting, repair

**Gameplay save (`gameplay.json`, `madfall.save/1`).** Beside the region files:
player (exact location, view, vitals, level, XP, every inventory slot including
empty ones), clock, containers, POI sleeper days, and world pickups. Items are
namespaced ids, so an item from an uninstalled mod stays in its slot verbatim.
Written atomically (temp file, previous save rotated to `.bak`, rename); a
truncated live file loads from the backup; an unreadable save is moved to
`.corrupt` rather than overwritten by the next autosave. Saved on `mad.save`,
every `mad.save.AutosaveMinutes` (5, with the world's dirty chunks written on a
worker - see Durability), and when the player leaves play. Zombies are
deliberately not saved - the director and sleepers re-derive them. Crafts in
progress are saved as their refunded ingredients, so a save never contains
items that exist nowhere. CI gate 9 quits a session and checks the next one
restores the survivor.

**Pickups (`AMadItemPickup`, `UMadPickupSubsystem`).** A bag of stacks lying in
the world, settling onto the voxel below and falling again if it is dug out,
collected by walking within 1.5 m. Used by: zombie loot, a destroyed container's
contents, block drops and craft output that do not fit, `Q` to drop the held
stack, and **the death backpack** - dying drops everything where the survivor
fell (7 Days to Die rules), and the walk back is the penalty.

**Timed crafting.** Ingredients are taken when the craft is queued
(`MadFall::Crafting::TakeIngredients`), the output arrives after
`craft_seconds x mad.craft.TimeScale`, and output that no longer fits is
dropped rather than failing after the wait. Cancelling or dying refunds.

**Repair and mods.** `R` repairs the held tool (`repair_with` cost, restores
`repair_fraction` of max durability, capped, atomic). `mad.player.installmod`
installs a mod into a free slot when its `applies_to_tags` match; mods are
permanent. Tests: `MadFall.Items.RepairAndMods`, `MadFall.Save.RoundTrip`,
`MadFall.Save.AtomicFile`.

### Phase 5 delivered, part 1: mod resolution and Tier-1 data mods

The modder-facing reference is [`docs/MODDING.md`](MODDING.md). This section
records the engineering decisions.

```
PostConfigInit  MadFallModAPI::StartupModule
                  FMadModManager::Discover   Mods/*/mod.json + Saved/Config/MadFallMods.json
                    MadFall::ModResolver::Resolve   (pure; dependencies, versions, cycles, order)
                  FMadModManager::MountPaks  Tier 2, before the asset registry scans
first use       MadFall::Definitions::ForEachSource(kind)   first-party, then load order
                  registry stages JSON  ->  FMadPatchSet::ApplyTo  ->  FinishLoad (inheritance, validation)
```

**One place decides load order.** Every loader - blocks, biomes, prefabs, items,
recipes, loot, zombies, patches - iterates `ForEachSource`, which walks the
resolved order. The three copies of "enumerate Mods/ alphabetically" from
Phases 1-3 are gone. Mod ids come from `mod.json`, not folder names.

**Resolution is pure and total.** `Resolve` takes manifests and a disabled set
and returns an order plus every issue. Ordering is Kahn's algorithm with the
alphabetically smallest ready mod taken first, so the result is independent of
discovery order and machine; `MadFall.Mods.LoadOrder` asserts that directly.
Dependency failures cascade to a fixed point. Cycles are found with Tarjan's
SCC over the unorderable remainder and only true cycle members fail - a mod that
merely requires one then fails with "requires X, which failed", and a mod that
only soft-orders after one loads. A resolution fingerprint (CRC of id@version in
order) is exposed for stamping into world saves.

**Patches edit JSON, before inheritance.** Patching the parsed structs would need
a reflective path language over C++ types and could not add fields a struct did
not anticipate; patching the source JSON reuses the one parser and its
validation, reaches children through `extends`, and makes a patched definition
indistinguishable from a hand-written one. Recipes, loot tables and zombies now
keep their source JSON after staging so they can be re-parsed when patched.
Same-path writes from different mods warn; unmatched targets warn.

**Tier 2 status.** Raw paks listed in `mod.json` are mounted through
`FCoreDelegates::MountPak` at priority 1000 + load position (packaged builds
only). The supported content path is a plugin inside the mod - see "Phase 5
delivered, part 3" below.

**Tier 3 status.** Delivered - see "Phase 5 delivered, part 4" below.

Tests: `MadFall.Mods.Versions`, `MadFall.Mods.LoadOrder`, `MadFall.Mods.PatchOps`,
`MadFall.Mods.Patches`, `MadFall.Mods.ExampleDataMod`.

### Phase 5 delivered, part 2: survival tuning and perks as data

The last hard-coded balance moved into Tier 1: `madfall.tuning/1` and
`madfall.perk/1` definitions, both patchable.

**Tuning is a flat number map, not a typed schema.** Core parses
`values: {key: number}` and knows nothing about survival; Gameplay maps keys onto
`FMadSurvivalTuning` fields through one member-pointer table
(`MadFall::Survival::ApplyTuning`). The tradeoff: Core cannot reject a typo at
load, so unknown keys are returned, logged, and `MadFall.Items.ShippedContent`
fails if shipped data contains one. The gain is that adding a tunable is one
line in Gameplay with no Core schema change. The shipped file restates every
default so the numbers a modder patches are visible in one place.

**Perk modifiers are multipliers on named stats.** Eight stats, each read at
exactly one place: swing stamina, block damage, melee damage and craft time in
the player verbs; max health/stamina and food/water drain pushed into the
survival component by `ApplyPerkStats` (on BeginPlay, purchase and save
restore). A rank replaces the previous rank rather than adding to it, so a
modder can read any rank's effect without summing, and a patch to rank 3 cannot
silently change what rank 4 means. Multipliers across perks multiply.

**Why not Gameplay Effects yet.** Perks are the obvious GE use, and the attribute
set is ready for them, but only two of the eight stats are attributes; the other
six are verb-local numbers. Routing those through GE would mean inventing
attributes for "mining damage multiplier". Revisit when buffs and armour arrive
and several sources modify the same stat with different stacking rules.

**Saves keep unknown perks.** Ranks are saved by id; a removed mod's perk keeps
its ranks and spent points, contributes nothing, and comes back with the mod -
the same rule as unknown blocks and items. Maxima are recomputed from ranks on
restore, never trusted from the save.

Console: `mad.perks`, `mad.player.perk <id>`, `mad.player.xp <n>`. Tests:
`MadFall.Progression.Perks`, `MadFall.Progression.SurvivalTuning`, the perk
round trip in `MadFall.Save.RoundTrip`; the survival acceptance gate buys a
perk and the reload session checks it survived.

### Phase 5 delivered, part 3: packaging, surfaces, Tier-2 content mods

**The game packages.** `Scripts/Package.ps1` runs BuildCookRun (build, cook,
IoStore paks, archive) with `-createreleaseversion`, in about two minutes on
the dev box after the first cook. `Definitions/` is staged loose next to
`Content/` (`DirectoriesToAlwaysStageAsNonUFS=../Definitions`), and every mod is
installed from its build output rather than staged, so a content mod's
uncooked source never ships. The packaged exe was run headless: it mounts its
container, finds the mods in `MadFall/Mods`, streams the world and spawns.

Packaging found what uncooked CI could not: `M_MadVoxel` was absent from the
cook, because the only reference to it is a console-variable string.
`DirectoriesToAlwaysCook=/Game/Materials` fixes it, and gate 11 fails if the
packaged log ever says the section material did not load.

**Surfaces (`madfall.surface/1`)** map a block material class to a vertex
colour and optionally a material asset. The mesher already split chunk meshes
into one section per class; `UMadChunkMeshSubsystem::GetMaterialForClass`
now gives each section its surface's material (loaded once per class, falling
back to the default with a warning), and vertex colours come from the surface
instead of a hash. Surfaces are patchable (`kind: surface`) so a texture pack
re-skins a first-party class with one patch. The registry is immutable after
load and is loaded on the game thread in the mesh subsystem's `Initialize`, so
mesher workers read it without locks.

**Content mods are Unreal "Mod" plugins.** A mod folder holds one plugin with
`CanContainContent`. The editor discovers plugins under `<Project>/Mods` by
itself, so modders author and test against source assets. `Scripts/PackageMod.ps1`
DLC-cooks just that plugin (`-dlcname`, `-basedonreleaseversion`) into a pak +
IoStore container holding its packages and shader library, and copies the
`.uplugin` and `Content/Paks` into an install folder. A packaged game discovers
the plugin under `Mods/`, mounts the container, registers `/PluginName/` and
opens the shader library - none of which MadFall code does. Measured in the
packaged exe: the `example_paint` container mounted, its shader library loaded,
and its surface rendered with `/ExamplePaint/M_Paint` (screenshot checked, when
the mod still re-skinned all concrete; it now paints only its own block, since
every test mod is on by default and red concrete everywhere is not the game).

*Why DLC cooking rather than UnrealPak by hand:* the packaged game loads through
IoStore; loose cooked assets in a legacy pak are invisible to the Zen loader,
and building containers by hand means reimplementing package ids, container
headers and shader extraction. *Tradeoff:* a mod is tied to the release it was
cooked against.

Known limits: the base pak still carries each example plugin's `.uplugin`
descriptor (the plugins are enabled in the project so they load in the editor),
though not their content; content mods supply materials and static meshes (model
blocks) so far; Windows only.

### Phase 5 delivered, part 4: Tier-3 script mods

Lua 5.4.9, vendored in `Source/ThirdParty/Lua` (the lua.org release, checksum
and licence recorded in its `LICENSE.txt`), compiled into
`MadFallScriptRuntime`, which depends on `Core` and `MadFallModAPI` only. The
surface a script reaches is `IMadScriptHost` in ModAPI - plain names, voxel
coordinates, numbers and strings - implemented by `UMadScriptSubsystem` in
Gameplay. `Mods/example_scripted` is the reference mod.

**Raw Lua C API, not sol2.** The approved design named sol2. sol2 needs RTTI,
is a very large template header, and hides where a Lua error can unwind; the
bindings are twelve small functions, where explicit stack code is shorter than
the binding layer and every error path is visible. Cost: arguments are checked by
hand (`luaL_check*`), which the tests exercise.

**Lua is compiled as C++, with exceptions, in one file.** UBT builds `.c` files
as C, and C Lua raises errors with `longjmp`. On Win64 a `longjmp` unwinds
through the C++ binding frames between the error and `lua_pcall`, and the C++
unwinder (FH4) terminates the process when it meets it - the first script error
crashed the game. `Private/MadLua.cpp` includes the Lua sources in `onelua.c`
order, so Lua raises errors with `throw`, and the module enables exceptions. The
sources sit outside the module folder so UBT does not also compile them as C.
The module has no PCH and no unity build: Lua has functions called `check`
and `verify`, which are Unreal macros. Exceptions never leave the module,
because the game side of `IMadScriptHost` never calls back into Lua.

**One Lua state per mod.** Isolation is structural, not by convention: mods
cannot see or overwrite each other's globals, one mod's memory cap is its own,
and a disabled mod is a closed state. Cost: roughly 25 KB per mod before its
script runs.

**Limits, and how each is enforced.**
- *Libraries*: only base, coroutine, table, string, math and utf8 are opened;
  `io`, `os`, `package` and `debug` are not in the source set at all. `load`,
  `loadfile`, `dofile` and `string.dump` are removed, and chunks load in text
  mode, because hostile bytecode can break the VM's memory safety.
- *Time*: a count hook reads the clock every 1000 instructions and stops a call
  past 1 ms. A script can `pcall` around that error, so once a call is over
  budget the hook switches to every instruction and raises on each until the
  call has unwound - including in coroutines, whose hooks are per thread.
- *Memory*: a custom allocator refuses allocations past 16 MB; Lua raises "not
  enough memory" inside the script.
- *World writes*: 256 block edits and 4 spawns per call.
- *Errors*: 20 and a mod is disabled for the session.

Known gaps, accepted: the hook counts VM instructions, so a single call into a
C library function (a pathological `string.find` pattern) runs to completion
before the check; and compiling a script is not budgeted, which is why mods load
one per frame. Both are bounded by the memory cap and by being authored rather
than adversarial content; a hostile mod is out of scope for a single-player
game where the player installs the mod.

**Events are queued and drained inside the frame budget.** Gameplay code never
calls Lua directly. The player's quest notifications (place, break, craft,
wear, kill, bed), the clock's dawn and dusk, and polled transitions (spawned,
died, horde started) become `FMadScriptEvent`s. An event with no handler is
dropped before it is built. The subsystem dispatches at least one event a frame
and then only while `FrameBudget::GetRemainingMs` allows (a 0.75 ms share, new
system `Scripts`). Why: a script that edits the world can never re-enter the
player's update half-way through it, and script time is charged in one place.
Cost: a handler runs up to a frame late; events carry their data, so nothing
observable changes. The queue is capped at 512, oldest dropped with a warning.

**Scripts load after the save is applied.** `UMadGameplaySaveSubsystem` hands
`mod_store` to the runtime in `ApplyWorldState`; the script subsystem waits for
`HasAppliedWorldState` before loading, so a script's top level can already read
its store. A store for a mod that is not installed is carried through saves
unchanged, like an unknown item.

Tests: `MadFall.Scripts.Sandbox` (libraries, isolation, bytecode refused, three
kinds of endless loop stopped in under the budget plus margin, memory cap,
disable after 20 errors), `MadFall.Scripts.Api`, `MadFall.Scripts.Store`,
`MadFall.Scripts.ExampleMod`, and `mod_store` in `MadFall.Save.RoundTrip`. CI
gate "scripts" plays the example in a real game: self-test, beacon, a chopped
block, 25 kill events, a real horde dusk and dawn, then a second launch reads the
tallies back from the save.

### Model blocks

A block with `shape.kind: "model"` draws `render.mesh` instead of a cube
(`MadModelInstances.h`, in the mesher module). The voxel is unchanged - still
solid, targetable, structural, damageable, a container if tagged - so nothing
in gameplay learns about meshes.

**Chunk mesher.** Model voxels contribute no cubic faces and count as empty for
the isosurface, and they never hide a neighbour's face: a barrel does not fill
its voxel, so the wall behind it must still draw. The check is a flat 65536-entry
table filled lazily per block id, because it sits in the isosurface corner loop
where a map probe was measurable.

**One `UInstancedStaticMeshComponent` per (chunk, block type).** A chunk's
barrels are one draw call; an edit rebuilds only its chunk's instance list; an
unload destroys exactly that chunk's components. An actor per model would be a
UObject, scene proxy and physics body each. The component is also the
collision (BlockAll), since the chunk mesh has none there. *Tradeoff:* every
edit in a chunk with models clears and re-adds its instances, including their
physics bodies - fine at tens per chunk, and the place to optimise (diffing
instance lists) if a mod packs hundreds.

**Collection is pure** (`MadFall::Models::CollectInstances`): palette first, so a
chunk with no model block (nearly all of them) costs O(palette) and never
touches its voxels. `MakeTransform` builds the rotation from the orientation
table's columns, and `MadFall.Mesher.ModelInstances` checks all 24 orientations
against `Orientation::GetMatrix` so the drawn mesh and the voxel data can never
disagree about which way a block faces.

**Budget.** Rebuilds run in `UMadModelInstanceSubsystem::Tick` under
`MAD_FRAME_SCOPE(Meshing)`, `mad.models.BudgetMs` (0.5) per frame with at least
one chunk. Meshes and materials load in `Initialize`: loading them in the first
rebuild measured 12.9 ms; preloaded, the worst rebuild is 0.48 ms.

**Material.** `render.material` if given; otherwise a dynamic instance of
`M_MadVoxelHeld` with the block's surface colour and pattern as parameters,
because a mod's static mesh has no MadFall vertex colours for `M_MadVoxel` to
read. It draws the pattern from the mesh's local position, so a barrel has
planks and a bush has leaves; the engine's `BasicShapeMaterial` tinted flat was
the first version, and is the fallback without the asset. The material is saved
with the instanced-mesh usage, which a cooked build cannot add on demand. The
berry bush and potato plants were spheres; they are boxes now, like the world
around them.
`/Engine/BasicShapes` is always cooked for the shipped barrel and this material.

Tests: `MadFall.Mesher.ModelBlocks` (faces around a model voxel),
`MadFall.Mesher.ModelInstances` (transforms, collection, removal),
`MadFall.Mesher.ShippedModels` (every installed model block's mesh loads, and
its surface-named slots resolve). CI
gate 9 places a barrel and requires `mad.models.stats` to count its instance.

### World scatter

Biomes grow trees, boulders and berry bushes (`scatter` in `madfall.biome/1`,
see docs/MODDING.md). Wood now comes from trees rather than only from loot:
oak and pine logs (planks), leaves (plant fiber, cloth), berry bushes (food),
desert cacti (fiber, water).

**Every chunk re-derives every feature that reaches it.** A tree's canopy
crosses chunk borders and chunks generate independently on workers, so
`GenerateScatter` rolls every column within the largest feature reach of the
chunk, not just its own. The column roll is a hash of the seed and the column;
only the ~1-3% of columns whose roll is under the largest biome chance compute
a biome and a height, so the margin costs almost nothing. Roots are visited in
one global order (Y then X) and the overlap rules do not depend on which chunk
is writing (a trunk replaces leaves, leaves go only into air, boulders never
replace construction), so two trees that meet resolve identically in both
chunks. `MadFall.WorldGen.Scatter` generates 27 chunks separately and floods
from grounded trunks to prove nothing is cut off at a border; measured mean
chunk generation stayed at 3.4 ms.

**Trees are real structure.** Trunk and leaves are cubic blocks, so a chopped
tree falls. That made two things necessary:

- *The canopy follows the solver's rules.* `MadFall::Scatter::BuildTree` keeps a
  leaf only if its cost from the trunk - resting free, sideways or hanging one -
  is within the leaves block's span. The first version checked plain
  connectivity and generated leaves that only hung from the leaf above them
  (cost 5 with span 4); `MadFall.Structural.GeneratedScatterStands` runs the
  real solver over generated forests, plains, tundra, desert and highlands for
  three seeds and requires zero failures.
- *Load checks skip `block.natural`.* A forest canopy is one structure of
  ~28,000 blocks, and load seeding re-solved it every time a chunk of it streamed
  in: 64 of 78 over-budget frames in a forest walk were the solver. Scenery is
  generated sound (the test above), so it is only checked when edited.

**Found on the way: a solver bug older than trees.** Gathering spans frames. A
member processed while a neighbouring chunk was unloaded records that position
as an anchor; when the chunk streams in and the neighbour is gathered as a
member later, its edge back was set but the first node's was not. Support never
flowed across that one-way edge, so a block beside a freshly loaded chunk could
collapse as "unsupported" - seen as single leaves dropping out of forests, and
able to drop any building on a chunk border. Gather now sets both directions
(`MadFall.Structural.ChunkLoadMidJob`, which fails without the fix: 360360
support distance instead of 240240).

**Frame share, not just per-system budgets.** Forest chunks broke 2 ms with no
single system over its own budget: structural 1.0 + meshing publish 1.0 plus a
model rebuild. `MadFall::FrameBudget::GetRemainingMs(Share, Minimum)` gives a
system its share cut to what is left of 90% of the frame after systems that
already ticked; the structural solver uses it (minimum 0.1 ms) and model
instance rebuilds skip a frame that is already half spent. Chunk meshes also
merge mesh sections that share a material (one per material, not per material
class): forest chunks had 6-9 sections and 1.6-1.8 ms of section creation; the
worst apply after merging was 1.16 ms. Four-biome rendered session: 4 of 13,594
frames over 2 ms, worst 2.29 ms, no collapses.

`mad.player.tpbiome <biome>` travels to the nearest place well inside a biome
(holding the survivor until it streams in).

### Block look

Blocks read as materials, not coloured cubes, without a texture asset. Code:
`Scripts/make_voxel_material.py` (builds `M_MadVoxel` and `M_MadVoxelHeld`),
`MadFall::Surfaces` in `MadSurfaceRegistry.h`, `ComputeFaceOcclusion` in
`MadChunkMesher.cpp`.

- **Patterns are procedural, chosen per surface.** A surface names one of 16
  patterns (`pattern`); the registry writes its index into vertex alpha
  (`255 - index * 16`, so an old mesh with alpha 255 is `plain`) and the
  material draws it in HLSL at 16 texels a voxel, projected from world
  position along the face's dominant axis. *Why:* no atlas, UVs or import
  pipeline before anything looks different; the pattern is one moddable field
  and a surface with its own `material` skips it. *Cost:* per-texel hash noise
  aliases far away, so it fades to the pattern's average from 25 to 65 m, and
  a smooth slope's pattern steps where its dominant axis changes.
- **Corner ambient occlusion is baked by the cubic mesher.** Each corner of a
  visible face counts the solid blocks beside and across it in the layer in
  front (two sides make a full corner) and stores 0-3 per vertex; the
  component passes it as UV1.x, since vertex colour is full, and the material
  darkens base colour to at most half. *Why baked:* chunk meshes are
  procedural, so they have no mesh distance fields and Lumen's software
  tracing cannot see them off screen; without it an inside corner or a block
  on a floor had no contact shadow at all. *Cost:* three lookups per corner of
  a visible face on the worker thread, and merging: faces merge only with
  faces of the same occlusion, faces whose corners differ do not merge (a
  stretched gradient would misplace the shadow), and each quad splits along
  the diagonal joining its more alike corners. An open 32x32 floor still
  merges to one quad. Smooth terrain is not occluded.
- **The held block wears its pattern.** `M_MadVoxelHeld` runs the same HLSL
  from the cube's local position, colour and pattern from parameters, so the
  pattern rides with the view model instead of swimming through world space.
  Wood, stone and metal resources in hand use `planks`, `stone` and `metal`.
- **Leaves are lit through.** `madfall:leaves` and `madfall:pine_leaves` name
  `M_MadVoxelFoliage`: the same patterns with the two-sided foliage shading
  model, subsurface colour 0.6 of the albedo, so a canopy's underside glows
  green where the sun is on its top. *Cost:* leaves are their own mesh section
  (one more `CreateMeshSection` in a forest chunk) and a transmission term.
- **Sky light 3, not 1.5.** Measured at noon in a forest: trunks in shade had
  luma 14 of 255 at 1.5, 30 at 3, 57 at 6, where 6 also washed the sunlit ground
  out. Night and a stall interior were unchanged, being set by auto exposure.
  Lumen's `LumenSkylightLeaking` was tried first and changed nothing measurable.
- **Block lights are warm, not orange, and cast shadows.** Light colours are
  sRGB, so the old `[1.0, 0.55, 0.25]` was linear (1, 0.26, 0.05), a saturated
  orange that blew out at night; torches are now `[1.0, 0.78, 0.54]`, about
  2700 K. Without shadows a torch lit the room behind a wall as if the wall were
  not there. `mad.models.LightShadows` (on) turns them off; each light renders
  virtual shadow maps, a GPU cost bounded by `mad.models.MaxLightsPerChunk`.
- **Test mods do not re-skin first-party classes.** Every test mod is enabled
  by default, and `example_paint` used to point `madfall:concrete` at its red
  demo material, so all concrete in the game was red. It now adds its own
  surface and a Painted Concrete block.

Tests: `MadFall.Mods.Surfaces` (pattern names, alpha), `MadFall.Mesher.CornerOcclusion`
(open floor unoccluded and still merged, a wall's foot and end, an inside
corner fully occluded, winding intact).

### Realism pass: ray tracing, relief, atmosphere

Step 3 of making the game look more three-dimensional without new art.

- **Hardware ray tracing for Lumen** (`Config/DefaultEngine.ini`: SM6 as a
  targeted shader format, `r.RayTracing`, `r.Lumen.HardwareRayTracing`).
  Procedural chunk meshes have no mesh distance fields, so software Lumen could
  not see terrain or buildings off screen; ray traced, it traces the chunk
  geometry itself (`r.RayTracing.Geometry.ProceduralMeshes`). Measured
  difference in a torch-lit outpost: warm bounce from wooden walls onto the
  shaded stone floor. Shadows stay virtual shadow maps. Cost: SM6 shaders (a
  first launch compiles them) and ray tracing geometry built on the render
  thread with each chunk section; GPUs without it fall back to SM5 software
  Lumen.
- **Relief from the patterns** (`M_MadVoxel` version 6). Each pattern returns a
  height with its colour; the material evaluates it at the four neighbouring
  texels and tilts the normal by the slope, so mortar is recessed, plank seams
  and nails catch light, stone is rough. Placed blocks - the cubic mesher sets
  UV1.y through `FMadMeshSection::CubicFaceFlag` - also lean their edge texels
  outward, a one-texel bevel round every face. The first strengths (0.35 bump,
  0.55 bevel) drew a harsh grid across a torch-lit floor at night and were
  halved. Four more pattern evaluations a pixel within 25-65 m, none beyond.
- **Volumetric fog and sun scattering.** The map's height fog is turned
  volumetric at startup (`mad.weather.VolumetricFog`), forward-scattering and
  thin, and the sun scatters in it with light-shaft bloom, so a low sun through
  a canopy shows shafts and torches glow in haze.
- **A camera, not a debug view:** vignette 0.32, bloom 0.45 over 1.2, contrast
  x1.06, no fringe, on the runtime post-process volume.
- **FIXED: automated runs could silently render at Low.** The engine's
  `scalability` console command saves to `GameUserSettings.ini`; one probe left
  shadows off at 50% resolution and every rendered screenshot after it was at
  Low. `MadFall::Settings::EnsureApplied` now forces Epic, unsaved, for
  `-unattended` and `-MadDefaultSettings` runs.
- **Photo-textured surfaces** (step 1). `M_MadVoxelPBR`
  (`Scripts/make_pbr_material.py`) projects colour, normal and roughness maps
  along each face's dominant axis in world space - no UVs, seamless across
  chunks - with a second "side" set for grass-over-dirt, a tint toward the
  surface colour, and the procedural material's occlusion, bevel and weather.
  Twelve 2K sets from ambientCG (CC0: public domain, no attribution required)
  are fetched by `Scripts/fetch_surface_textures.ps1` into `SourceArt/` (not
  committed, ~680 MB unzipped) and imported by
  `Scripts/import_surface_textures.py` into `/Game/Surfaces/MI_<Set>`, which
  `/Game/Surfaces` in `DirectoriesToAlwaysCook` ships, since surfaces name
  them only by path. Textured: stone, granite and generic (Rock030), dirt
  (Ground048), grass (Grass004 over Ground048 sides), sand (Ground080), gravel
  and rubble (Gravel022), wood (Planks021, tinted 0.35 toward its brown),
  concrete (Concrete034), steel (Metal041B, metallic 0.85), bark (Bark012),
  cloth (Fabric066). Kept procedural on purpose: bedrock (a hue-only tint cannot
  make a photo dark), iron ore (its flecks are how ore is found), water,
  leaves, cactus, farmland and crops. Bricks and snow are imported but unused:
  no brick surface exists yet, and snow is the weather layer's.
  Tradeoffs: three samples a pixel (six on grass sides), and single-axis
  projection steps at slope changes on smooth terrain. Ray tracing hit shaders
  read mip 0 (no derivatives there).
  **Held blocks and model blocks wear the textures too.** A world instance
  projects from world position, which on a block in the hand or a barrel slides
  the texture through the object as it moves, so those had kept the procedural
  look beside photo-textured ground. `M_MadVoxelPBRHeld` is the same shader in
  the mesh's own space (local position +50 on the engine cube's voxel grid,
  normal brought back to world), and `MadFall::SurfaceMaterials::MakeHeld`
  builds a dynamic instance of it from any `M_MadVoxelPBR` instance a surface
  names - copying its textures, tile size, tint, sides and metallic - so mod
  surfaces get a held look with no second asset. The view model uses it with
  Weather 0 (dry in the hand); model blocks with no material of their own use
  it with Weather 1. Untextured surfaces keep the procedural held material.
  Adding the new source file regrouped the unity build and exposed old name
  collisions (six identical `FindPlayer` copies, two `ChunkSize` aliases, two
  rigs' `AttackSeconds`); they are now one `MadFall::FindLocalPlayer`, using-
  declarations of `MadFall::ChunkSize`, and `AttackPoseSeconds`.
  **One material for the layered sets, and meshes prepared on the worker.**
  The frame-budget gate failed after the textures went in (35 of 6,357 frames
  over 2 ms, meshing 34 of them). A chunk's mesh becomes one component section
  per material, and each section is a mesh creation, a collision update and a
  draw call; a material instance per set took a forest chunk from one or two
  sections to four or more. Measured with `mad.mesh.stats`' new mean apply
  (spawn area, every chunk rebuilt twice): no photo textures 0.647 ms and 1.39
  sections an apply; one layered material 0.790 ms and 1.98 sections (the
  procedural surfaces - ore, bedrock, water - still split off).
  - `M_MadVoxelPBRArray` samples three texture arrays (`TA_SurfaceBaseColor`,
    `TA_SurfaceNormal`, `TA_SurfaceRoughness`, nine 2K layers built by
    `import_surface_textures.py` from `Scripts/surface_sets.py`); a surface
    names it with `"texture_layer"`, which replaces the pattern in vertex
    alpha (same encoding, so the section-alpha rule above keeps it constant
    per triangle). Per-layer tile, tint, side layer and metallic are compiled
    into the shader from the same Python list. Concrete keeps its own instance:
    its set is 2048x1024 and array slices must match. `M_MadVoxelPBRArrayHeld`
    takes the layer as a parameter for held and model blocks.
  - `UMadChunkMeshComponent::Prepare` builds each section's
    `FProcMeshSection` (the component's own vertex struct) on the meshing
    worker; the game thread hands it to `SetProcMeshSection`, appending with
    an index offset where sections share a material. The apply had widened
    every vertex and then `CreateMeshSection` copied each one again - two
    per-vertex passes on the game thread. Mean apply 0.790 -> 0.565 ms, below
    the untextured figure; the frame-budget scenario then measured 15 of
    7,402 frames over 2 ms and meshing at 0.070 ms a frame, where it was
    before textures.
  **FIXED before it shipped: black ground.** The normal maps import BC5, which
  stores only X and Y; the material node the engine uses rebuilds Z, a raw
  Sample in a Custom node does not, so Z stayed 0 and every normal pointed
  into its surface - lit ground rendered black, shade dark blue. The earlier
  check with the engine's FlatNormal (uncompressed, Z stored) could not show
  it. The bevel was also narrowed (6% to 3%) and softened (0.35 to 0.15): photo
  textures carry their own relief, and trunks showed a dark band per block.

- **Photo-scanned models** (step 2). Two model blocks draw Poly Haven (CC0)
  scans: the storage barrel is `Barrel_01` (a red steel drum - its surface
  class became steel, it crafts from 4 scrap iron and breaks into 2-3, so it
  is never a profit) and the loot crate is `wooden_crate_01` (a cubic block
  before; now a model block facing one of four ways). `Scripts/fetch_models.ps1`
  downloads them into `SourceArt/polyhaven/`, `make_model_material.py` builds
  `M_MadModel` (UV-mapped colour, normal, roughness, metallic, rain darkening)
  and `M_MadModelFoliage` (masked, two-sided foliage), and `import_models.py`
  imports mesh, textures (OpenGL normals flipped to DirectX) and an instance
  into `/Game/Models`, logging bounds for the block's render scale and offset.
  Tried and dropped: a ladder (free-standing, not wall-mounted), a fern and a
  sorrel shrub (at block scale a flat cluster and a few blades; the box shapes
  read better as a berry bush and a potato plant).
- **Hand-built props.** The campfire (a stone ring, an ash bed and a teepee of
  logs), torch (a tapered stick in a cairn, its head bound in cloth), plank
  door (horizontal planks, battens, a brace and iron hinges; the open door is
  the same mesh swung onto its side), bedroll (mat, folded blanket, strapped
  pillow roll) and wall ladder were scaled engine cubes and cylinders. No free
  scan fit at one voxel, so `Scripts/build_prop_meshes.py` builds them from
  Geometry Script primitives (`/Game/Models/Props`, 150-800 triangles each;
  the plugin is enabled for the editor target only). Each part's material slot
  is named after a surface class, and `UMadModelInstanceSubsystem::ApplyMaterials`
  gives every such slot that surface's held-space material
  (`MadFall::Models::GetSlotSurface`), so a prop needs no UVs or textures of
  its own and follows the surfaces - a modded stone texture reaches the
  campfire's stones. A block's `render.material` still wins, which is how the
  scanned models keep theirs; surface materials are cached per surface, so the
  campfire and a stone block's barrel share instances. The torch's light moved
  up to 0.3 voxel: its cloth head sat between the light and the floor and threw
  a shadow disc a metre across. Door planks run horizontally because the wood
  texture's boards do; vertical planks under horizontal boards read as a grid.
  `MadFall.Mesher.ShippedModels` requires every slot that looks like a surface
  class (contains `:`) to name a known one.

**Menus and HUD are styled.** The menus use rounded dark panels with a faint
outline, buttons that light up in the accent colour (`FMadMenuStyle`, no
assets), shadowed text and a short accent rule under headings. The vitals are
framed bars with a lit top edge and their labels inside, on a dim panel; a
vital under a quarter pulses.

### Weather you can see

The weather system already rolled fronts of cloud, rain, storm and snow with
fog, dimmer sun, thunder and cold; now the world shows it (step 4).

- **One parameter collection, every material.** `MPC_MadWeather`
  (`Scripts/make_weather_collection.py`) holds Wetness, SnowCover and Wind;
  `UMadWeatherSubsystem` writes it only when a value moves by 1%, and the voxel,
  foliage, model and grass materials read it, so no material instance is
  touched per chunk.
- **Surfaces lag the sky** (`MadFall::Weather::StepSurface`, tested in
  `MadFall.World.SurfaceWeather`): two minutes of downpour soak the ground and
  ten dry it; a drizzle only dampens; snow covers in about three minutes and
  lingers twenty, keeping the ground under it damp; rain washes snow away. A
  world loaded mid-storm starts wet. `mad.weather.surface <wet> <snow>` sets
  it for probes.
- **Wet** surfaces go darker (porous ones more) and glossier; on open, flat
  natural ground (not placed blocks: UV1.y) value noise gathers puddles that
  are dark, mirror-smooth and flat. **Snow** settles on whatever faces up, with
  a grainy edge as it thins, flattening the relief under it; grass is buried
  to 60% of its height with its tips dusted. **Wind** lays grass over, from 3 cm
  of sway in a breeze to 30 in a gale. The block in the survivor's hand sets the
  held material's `Weather` parameter to 0 and stays dry; barrels and bushes in
  the world take the weather.
- **Lightning you can see.** Each storm strike draws a jagged bolt with two
  forks (`MadFall::Weather::MakeBolt`) 250-900 m away at a random bearing,
  flickering for a third of a second in `M_MadLightning`, and its thunder comes
  after the distance over the speed of sound (`ThunderDelay`) rather than with
  the flash. `mad.weather.strike` strikes now.

### Ground cover

Grass tufts and wildflowers on open ground near the survivor
(`MadGroundCover.h`, `Scripts/make_cover_material.py`). Open ground was a flat
green carpet to the horizon.

- **Decoration, not voxels.** Nothing to save, mine, collide with or stream:
  a surface opts in with `cover` data and the tufts are recomputed from the
  chunk whenever it changes. `MadFall::GroundCover::Collect` walks each column
  from the top of the chunk to the first solid voxel with air above, and on a
  covered surface places at most one grass tuft and one flower, from a hash of
  the world column (the same ground always grows the same grass). On smooth
  terrain the base follows the isosurface between the two density samples; on
  a placed block, its top face. A column whose top is solid belongs to the chunk
  above. Tested by `MadFall.Mesher.GroundCover` (density, flowers, the smooth
  and cubic surface heights, rock and roofed ground bare, deterministic, the
  card stood upright).
- **Two crossed cards per tuft** on the engine plane, instanced per chunk,
  surface and kind, in chunks within `mad.cover.Radius` (2) of the camera and
  culled past `mad.cover.Distance` (36 m). The material cuts seven tapering
  blades (or stems and a flower head) out of each card with an opacity mask,
  sways the tips with a world-position offset, and uses a world-up normal so a
  tuft is lit like the ground it grows from; cards cast no shadows, so the sway
  invalidates no shadow maps.
- **Cost, measured** in a rendered probe (CI runs without a renderer, where the
  subsystem does not exist): 75 chunks, ~12,000 instances; the worst chunk
  build 10.9 ms until the plane mesh and material were preloaded and one
  material instance shared per surface and kind, then 0.52 ms, and no frame
  over 2 ms while walking. The first version planted every card upside down
  (a roll of +90 turns the plane's +Y to the ground).

### Sky, exposure and light blocks

Every screenshot since Phase 2 was washed out, and it took three separate
fixes, each found by looking at renders rather than numbers:

1. **The palette was sRGB used as linear.** Surface colours were authored as
   picker values but written straight into linear vertex colour, so grass had
   46% albedo and sand 76%. Surface (and light) colours are now sRGB in JSON and
   converted once on load (`MadFall::Surfaces::SRGBToLinear`).
2. **No exposure control.** The project disables default auto exposure and the
   map had no post-process volume. `UMadSkySubsystem` spawns an unbound volume
   (priority -1, so any authored volume wins) with histogram auto exposure. The
   floor of its range is what matters, measured on the same forest shot: EV100
   -1 still washed out, +2 left shade black; the floor is +1 by day and -2 at
   night, blended by sun height, so night is dark but moonlit terrain reads.
   Light adapts fast (3), dark slowly (1).
3. **The sun never moved.** The map's sun was Stationary, and the clock's
   rotation code quietly skipped non-movable lights - noon, dusk and midnight
   rendered identically. The sky subsystem makes the sun movable at runtime (and
   `make_test_map.py` creates it movable).

The sky model (`MadFall::Sky::Compute`, tested for continuity, symmetry and
horizon fades over a full day in `MadFall.World.SkyCycle`): the sun rises in
the east, peaks 60 degrees up in the south and sets in the west - a sun at the
zenith, the first version, left every trunk and wall black at noon. A spawned
moon opposite it lights the terrain cool and dim; it is not an atmosphere light,
because a moon-lit atmosphere scattered a daytime blue sky at midnight. Both
lights fade over a band around the horizon and are hidden, not zeroed, when
down (a zero-intensity directional light still renders its shadow maps).
Tunables: `mad.sky.SunLux`, `MoonLux`, `SkyLightIntensity`, `ExposureMinEV`,
`ExposureMinEVNight`, `ExposureMaxEV`, `ExposureBias`, `ExposureBiasNight`,
`NightSkyBrightness`; `mad.sky.status`.

**Night reads as night.** With the sun down and the moon kept out of the
atmosphere, nothing lit the sky: night was moonlit ground, auto-exposed almost
to afternoon, under a black ceiling. Three changes, each measured on the same
plains shot:
- *A night sky dome.* `M_MadNightSky` (`Scripts/make_sky_material.py`) on the
  engine sphere scaled to 10 km, following the camera: deep blue lightening to
  the horizon, and stars from a hash per cell of the cube-mapped view direction,
  so there are no textures and the stars do not swim. Unlit and marked as sky,
  faded in from just after sunset, dimmed by cloud, hidden by day and absent
  without a renderer. **FIXED: the dome had collision.** Its component's
  collision was switched off only after the spawn, and a hidden actor still
  collides: a survivor teleported or restored inside the 10 km sphere was
  pushed out to its top, froze at -20 C, and saved there. Headless CI has no
  dome, so only a rendered probe showed it. The actor's collision is now off
  before anything else, and a saved location outside the world's height range
  (`MadFall::GameplaySave::IsRestorableLocation`, tested in
  `MadFall.Save.RoundTrip`) starts the player at their spawn column with
  everything else kept, so a location from any such bug cannot strand a save.
- *An exposure bias at night, not a lower floor.* Auto exposure settled above
  the night floor on the moonlit field, so floors of -2, -1 and 0 gave identical
  pictures; `ExposureBiasNight` -1 darkens night by a stop, blended by daylight.
  -1.8 was too dark to see a horde by.
- *Slightly cooler and greyer grading at night* (saturation x0.75, a faint blue
  gain). A first pass at x0.55 with a strong blue gain turned torch-lit walls
  pink: grading the picture grades the firelight too.

**Flames.** Every light block with `render.light.flame` above 0 (default 1)
draws a flickering additive cone centred on the light, one instanced component
per chunk (`M_MadFlame`, `Scripts/make_flame_material.py`), including blocks
past the light cap, so every torch visibly burns. The flicker is in the
material with a phase per instance; the point light stays steady, since
animating up to 16 lights a chunk from the game thread costs more than it adds.
Flames are kept out of the model instance counts and skipped without a
renderer. The first version was centred 10 cm below the light and sat half
inside the torch's stick.

**Light blocks** (`render.light`) are point lights managed with model instances:
collected per chunk with the same palette pre-check, placed with the same
transform (so the offset turns with the block), shadowed (see "Block look"), capped at
`mad.models.MaxLightsPerChunk` (16), and updated in place on edits. The torch
was first authored at 900 lm and blew a night scene to white; 150 lm with a
9-voxel reach gives a warm pool around it. `MadFall.Mesher.BlockLights`.

### Title screen, worlds and settings

The game used to launch straight into `DevWorld`, and every world - whatever
its folder - was seed 0: the seed was never stored. Now:

**world.json per world** (`MadFall::Session`, Core): name, display name, seed
(written as a string - a JSON number is a double and cannot hold every int64),
created, last played, day. The voxel world reads the seed from it; a folder
from before world.json (regions, no world.json) is listed and loaded as seed 0,
which is what it was generated with. Typed seeds: a number is used as is, any
other text is FNV-1a hashed, empty is random.

**A session choice outlives the level load it causes.** Selecting a world
records it in a process-wide session and reopens the map; the NEW world's voxel
subsystem reads the choice in its `Initialize`, before any game mode or game
instance code of that world runs (the map URL is not yet set there). Whether a
launch starts at the title is decided once from the command line - a plain game
launch does; `-unattended`, `-MadWorld=`, `-MadNoTitle` and the editor go
straight in; `-MadTitle` forces it for tests - and changed only by selecting a
world or returning to the title.

**The title is a real world.** `_Title` (a reserved name no player world can
take) is a read-only world (`UMadVoxelWorldSubsystem::IsReadOnly`: no chunk,
gameplay or world.json writes) streamed around an orbiting camera near a
forest at a held late-afternoon hour, with no survivor spawned
(`AMadGameMode::GetDefaultPawnClassForController`). Its folder is wiped on
startup: region files are created on open even when nothing is saved, and the
leftovers made the next launch read the backdrop as a pre-world.json world.

**Menus are Slate built in C++** (`UMadMenuSubsystem`, `SMadMenuWidget`): no
assets, reviewable in a diff, like the HUD. Every button calls a public function
on the subsystem and every function has a `mad.menu.*` command, so the CI gate
"title screen and worlds" drives the path a click takes: start at the title,
create a seeded world, pause, save and quit to the title, check the list and
world.json, load it again, and check the title wrote nothing. The subsystem
ticks the console script itself at the title and while paused, when no
survivor is ticking it. Deleting a world takes two clicks and refuses anything
that is not a MadFall world folder.

**Settings** (`FMadSettings`) are applied only through console variables
(`mad.input.LookSensitivity`, `mad.input.InvertY`, `mad.view.FieldOfView`,
`mad.stream.Radius`, `mad.Language`), so the menu, the console and `-ExecCmds`
all change the same thing. Saved to `Saved/Config/MadFallSettings.json`,
clamped on load, and not applied in `-unattended` runs so a developer's view
distance never changes what CI measures. Tests: `MadFall.Session.Worlds`,
`MadFall.Session.Settings`.

### Key rebinding

Settings > Controls rebinds the twelve keyboard actions (`FMadKeyBindings`,
`MadKeyBindings.h`); `mad.input.bind <action> <key>`, `mad.input.bindings` and
`mad.input.reset` do the same from the console for the session.

- **A clash swaps, it is never refused.** Taking a key another action uses gives
  that action the key just given up, so no action is ever unbound and a player
  never has to clear a key before reusing it.
- **Escape, P, 1-9 and the mouse are reserved.** Rebinding Escape away would
  lock a player out of the menu that undoes it; the mouse and number keys are
  what the help page, tooltips and hint lines name.
- **Only overrides are saved** (`"keys"` in `MadFallSettings.json`), so a
  default changed in a later version reaches everyone who never rebound that
  action. Loading resolves a hand-edited file the same way rebinding does:
  unknown actions, bad key names and reserved keys are dropped, and entries
  apply in action order as if rebound one by one.
- **The survivor remaps whole on change.** `MadFall::Input::SetActive`
  broadcasts; `AMadPlayerCharacter::MapKeys` unmaps the context and maps it
  again from the bindings, then asks Enhanced Input to rebuild. A dozen mappings
  cost nothing, and patching them in place would have to handle two keys
  swapping.
- **The controls page takes the key in `OnPreviewKeyDown`.** A focused Slate
  button treats Space and Enter as a click, so a bubbling `OnKeyDown` would
  start waiting for a key again instead of binding Space.
- HUD hint lines read key labels from the active bindings
  (`MadFall::Input::GetKeyLabel`), so they stay true after a rebind.

Tests: `MadFall.Input.KeyBindings` (defaults unique and legal, swap, back to
default, reserved and invalid keys, a hand-edited file, the settings file round
trip, change notification). CI gate "inventory screen" rebinds interact onto
jump's key and checks the survivor's remap line, then that Escape is refused.

### Doors, ladders, beds and water

Survival needs a base you can close, climb and come back to, and water that
does not run out:

- **Interaction is data.** A block's `interact.toggle_to` swaps it for another
  block, keeping orientation, damage and flags, together with matching blocks
  stacked directly above and below (both halves of a two-high door). The open
  door is its own block with `"collision": "none"`; model instances honour it,
  so the survivor walks through. Because an open door is still a solid voxel,
  zombies path around it or break it, as they would the closed one.
  `interact.spawn_point` makes a bed: interacting records it (saved with the
  player), and respawning uses it while a spawn-point block still stands there.
- **Facing placement.** `facing_4` blocks take the survivor's yaw, snapped to a
  quarter turn, as their orientation - so a ladder hugs the wall you look at and
  a door spans the doorway.
- **Climbing.** Standing in a `flags.climbable` voxel (feet or chest) switches
  the survivor to flying movement with vertical speed from the forward axis;
  stepping off the top gives a small lift onto the floor above. Water is also
  climbable, which stands in for swimming up.
- **Water.** Drinks can `return` their container; an item with `fill` turns into
  another when right-clicked at water (a ray that, unlike targeting, stops at
  liquid). Murky water hurts (infection) and boils into clean water at a
  campfire, a light-emitting station block.

Found on the way: `mad.voxel.set` gave water the construction flag and not the
liquid one, so the structural solver judged a console-placed water block an
unsupported member and removed it the frame it appeared. Liquid blocks are now
flagged as generated water is. The "base building" CI gate opens and closes a
door, fills, boils and drinks, climbs a five-high ladder, and respawns on a
bedroll.

### Farming

A renewable food source, so a base can outlast the loot around it:

- **Till, plant, wait, harvest.** A hoe (`till`) turns `block.tillable` dirt
  or grass into cubic farmland. Seeds are block items whose sprout carries
  `placement.on_tag: block.farmland`. Each stage names the next and how many
  in-game hours it takes (`grow`). The ripe stage's loot table uses the new
  `always` entries, so a harvest gives food and seeds back.
- **A list of plants, not a random tick.** `FMadPlantTracker` records each
  growing voxel and the world hour its stage started. It learns about planting
  from `OnVoxelChanged`, the same funnel the structural solver uses; a damage
  write to the same block is not a replant. The alternative, scanning loaded
  chunks for growing ids as Minecraft does, costs a 32,768-voxel pass per chunk
  whether anything grows or not, and stops farms dead in unloaded chunks.
  Planting is rare and deliberate, so the list is small.
- **Catch-up.** A stage change starts the next stage at the hour the old one
  was due, not when the check noticed it. A field left for three days has
  ripened on return, not merely sprouted. Plants in unloaded chunks keep their
  record; checks run once a second, at most 16 stage changes per check, each an
  ordinary SetVoxel.
- **Saved with gameplay.** `gameplay.json` gains a sorted `plants` array (voxel,
  block id, stage start hour). Stage lengths are re-read from definitions on
  load, so a mod that changes a growth time applies to existing farms. A plant
  whose block's mod was removed keeps its record and never comes due, like an
  unknown item in a slot. The cost of a list: a growing block it does not know
  about (from a prefab, or with the save lost) never grows. No shipped prefab
  places crops.
- **Crosshair.** Crops show `growing 40%` or `ripe` in place of a structural
  load.

`mad.farm.status` and `mad.farm.advance <hours>` drive it from the console. The
"farming" CI gate tills, plants, checks that a seed aimed at stone is refused,
saves, advances 30 hours, and harvests corn and seeds.

### Animals

Hunting is the other renewable food source, and wolves make the night outside
worse than zombies alone:

- **A kind of its own.** `madfall.animal/1` rather than zombie variants:
  animals spawn by biome instead of by POI group, never dig, never join a
  horde, and mostly run. What they share with zombies is shared in code: the
  voxel pathfinder (with digging off), line-of-sight and noise senses, loot
  bags on death. Patches and mods treat `animal` like any other kind.
- **Behaviour is a pure function.** `MadFall::Animals::Decide` maps a
  definition and what the animal senses (distance, sight, hearing, seconds since
  hurt) to graze, flee, chase or attack, and is unit tested case by case. The
  actor only acts on its answer: a flight path 12 voxels directly away with
  sideways jitter so a herd scatters, a chase repathed when the survivor moves,
  a gore on a cooldown. Skittish animals can be stalked by walking: only a
  survivor seen within `flee`, or heard sprinting or swinging, spooks them.
- **One swing, two kinds of target.** The survivor's melee trace looks for
  `IMadDamageable`, implemented by both `AMadZombie` and `AMadAnimal`. An
  interface, not a base class: a zombie is GAS-backed and digs, an animal is a
  health float, and the swing needs only `ReceiveHit` and `IsDead`.
- **Director.** `UMadAnimalSubsystem` tries a herd every 25 s in a 30-56 voxel
  ring around the survivor: the dominant biome at that column, species living
  there and active at this hour, a weighted pick, herd members on standable
  ground nearby. It caps live animals at 12 and removes those beyond 110 voxels,
  or out of their active hours and out of sight. Nothing is saved, as with
  wandering zombies.
- **Quadruped rig.** `UMadQuadrupedRigComponent` is the humanoid rig's
  four-legged sibling: basic shapes sized from the definition's body, legs and
  neck; a diagonal trot that widens into a gallop; the head lowered to graze
  or butt; a roll onto its side on death. Built lazily, two per frame, and
  posed only when recently rendered.

- **Bows.** A deer that bolts at 12 voxels cannot be clubbed, so hunting needs
  range. A tool with `ranged` fires `AMadProjectile`, moved by hand each tick:
  a ballistic step (`MadFall::Projectile::Step`, unit tested against the closed
  form), then the segment traced against pawns and against voxels, the nearer
  hit winning. The voxel raycast rather than terrain physics, because cooked
  collision lags a fresh edit and the voxel data never does. Shots are silent,
  so stalking works, and arrows that hit the world are sometimes recoverable.

The "animals" CI gate clubs a boar that then gores the survivor, butchers it
for meat and hide, watches a deer bolt, shoots a second boar with a bow, and
checks that natural spawning places a herd. The bow shot turns off grazing
strolls first (`mad.animals.Stroll 0`): a boar that wandered two voxels between
the aim and the arrow's arrival failed the gate on a correct shot.

### Clothing

The survival model always had a comfort band that insulation shifts. Clothing
is what finally feeds it, and adds armour:

- **Four worn slots** (head, body, legs, feet) held as a second small
  `FMadInventory` on the inventory component. The item/slot rules that inventories
  already have (move, swap, quick-move, save as stacks by id) apply to it for
  free. Only the "does this fit here" check is new, and it runs in
  `MoveItem` whichever way an item travels, so a pickaxe cannot be swapped
  into the head slot from the other direction either.
- **Totals, not events.** Worn `cold`, `heat` and `armor` are summed after
  every change to the worn slots and pushed into the survival component.
  Armour applies only to creature attacks (`ApplyAttackDamage`), not falls or
  collapses, capped at 80%.
- **Kept on death.** A dead survivor's backpack drops; clothing stays on. A
  run of bad luck in the tundra then does not also mean freezing on the walk
  back to the bag.
- **Content.** Hides from hunting make the warm set (hat, coat, trousers,
  boots). Plant fibre makes a straw hat against desert heat. Scrap iron at the
  forge and workbench makes a helmet and vest, which also turn up in military
  weapons lockers.

### Traders

A trader outpost near every spawn, where a player trades loot for coins and
coins for supplies. Data: `madfall.trader/1` definitions, item `value`s, a
`trader` prefab marker, and `madfall:trader_outpost` with `near_spawn`
placement. Code: `MadTrading.h` (rules, `AMadTrader`, `UMadTraderSubsystem`).

- **One outpost per world, placed by the planner, not stored.** POI placement
  is a pure function of the seed per cell, so "exactly one, near the spawn"
  cannot be a random roll. `FMadPoiPlanner::ResolveNearSpawn` walks the rings
  of cells around the spawn - skipping the four cells that meet at the spawn
  column, so no building lands on a new player - nearest ring first, cells of a
  ring in an order hashed from the seed, and reserves the first cell with a site
  for each near_spawn prefab. It runs once, under a lock, using only the site
  test (never `PlanCell`), so it cannot recurse and gives the same answer on any
  thread in any order; `MadFall.WorldGen.NearSpawnPoi` checks exactly that for
  three seeds. Cost: the cell's ordinary POI is replaced, so adding the outpost
  changes that one cell of an existing world's ungenerated terrain, like any
  new prefab.
- **Shelves per trader position, restocked lazily.** `FMadTraderState` is keyed
  by the marker's world voxel and saved in `gameplay.json`. It is rerolled the
  first time it is looked at on or after its restock day, seeded by world,
  position and day, so a trader nobody visits costs nothing and each outpost
  has its own shelves.
- **Traders are actors only near the survivor**, like sleepers: the subsystem
  scans POIs within two chunks once a second and stands a trader at each
  marker over loaded ground within 64 voxels, removing them past 96. They are
  not damageable and not zombie targets - a trader who can die is a softlock.
- **Trades are atomic**, worked on a copy of the backpack and committed only if
  everything fits (`MadFall::Trade::Buy`/`Sell`). A trader buys at most
  `buy_factor` < 1 of an item's value, so buying and selling back never pays.
- **The trade screen is the inventory screen** with the shelves where a crate's
  slots would be and prices on each slot: click buys one, shift-click the
  stack, shift-click on the backpack sells. Interact on a trader goes through a
  pawn trace in `UpdateTarget`, accepted only if nearer than the targeted block.
- The compass always shows the outpost; the tutorial quest "Rosa's Outpost"
  (`trade` objective) sends a player there after their first timber.

Tests: `MadFall.Items.Trading` (definitions, prices and offers, restock,
buying, selling, a full backpack), `MadFall.WorldGen.NearSpawnPoi`. CI gate
"traders" travels into the outpost, trades through the hit boxes and reloads
the shelves.

### Quests and the compass

A new survivor needs to be told what the game is, and every survivor needs to
find their way home:

- **Quests are data** (`madfall.quest/1`) with objectives of nine types. The
  tutorial chain (first tool, timber, shelter, bed, clean water, hunting,
  farming, clothing, the seventh night) ships as one JSON file a mod can
  extend by `requires`.
- **`FMadQuestLog` is plain C++**, so unlock order, counting, clamping and
  save/restore are unit tested without a world. The player feeds it events
  from the verbs that already exist (craft finished, place, break, kill,
  wear, sleep) and, once a second, state (items carried, the day). Event
  objectives accumulate; state objectives are re-read, so `have 12 planks`
  means holding them, not having once held them.
- **No giver, no hand-in.** A quest starts when its requirements are complete
  and pays out the moment its last objective is met. Adding a giver later is
  a new objective source, not a redesign.
- **Saved by id** in the player record: completed ids and the counts of active
  quests. Counts are resized against the current definitions on load, so a mod
  that adds an objective does not break old saves. Unknown quests are kept.
- A test walks the shipped quests in unlock order and checks every one is
  reachable, and that a fresh survivor starts with exactly one.
- **Compass.** A heading strip with markers for the bed, dropped backpacks and
  POIs within 300 m. POIs come from the planner's per-cell cache, so asking
  about the surrounding cells each frame costs map lookups, not planning.

### Far terrain

Chunks stream to 256 m; beyond that the land used to simply end. The
generator's surface height is analytic, so the rest is drawn without a voxel:

- **Heightfield tiles on fixed world grids**: 128 voxels at 8-voxel spacing
  near the chunk edge, 512 at 32, and 2048 at 128 out to about 6 km. A
  clipmap recentred on the viewer would rebuild and swim as you walk; anchored
  tiles are built once and kept while in range, so walking only adds tiles
  ahead and drops tiles behind.
- **Seams without stitching.** Each level sinks a little under the one inside
  it (2, 6, 20 voxels), and every tile has a skirt hanging from its edges. A
  tile wholly inside what a finer level or the real chunks cover is never
  requested. Tests check there are no holes from the chunk edge to 4 km, and
  that neighbouring tiles share edge heights exactly.
- **Colour from biomes, flat water at sea level.** No trees, POIs or edits:
  at that distance fog and silhouette carry it.
- **Cost.** Sampling runs on workers (four at a time). The game thread only
  creates the procedural mesh components, within a 0.5 ms share of the frame
  budget. They have no collision and cast no shadows. Headless, 192 tiles built
  with no frame over 2 ms. `mad.far.Enabled`, `mad.far.Range`, `mad.far.status`.
- **FIXED: one slow tile could stop the far terrain for good.** Whether a tile
  fits the share is judged by a running estimate of tile cost, and the estimate
  only learned from tiles actually created. A single slow creation lifted it
  above 0.5 ms; nothing fit again, the four builds in flight were never applied,
  and nothing new launched. The frame-budget gate saw 4, 28, 123 and 195 of the
  same 195 tiles on four runs of one walk. While nothing fits, the estimate now
  decays (x0.95 a frame, about half a second from a 3 ms spike), so a spike
  delays the far terrain rather than ending it. Found while ruling out the road
  banks, which `MadFall.WorldGen.StreamingCost` measures (seed 0, the gate's
  walk: 4.15 ms a chunk with banks, 4.13-4.20 without).

### World map

- **Exploration is remembered, the picture is not.** A set of chunk columns
  within 128 voxels of wherever the survivor has been is saved as a flat
  `[x, y, ...]` array in gameplay.json. The image is drawn from the
  generator's surface and biome fields on a worker whenever the map opens,
  zooms, or the survivor walks a fifth of the way across it. It is 256 pixels
  at 4, 8 or 16 voxels a pixel, hill shaded from the north-west, water
  darkening with depth, north up like the compass.
- **Game-thread cost is a memcpy.** The sRGB conversion happens on the worker,
  and the texture is created while the world loads and updated through
  `UpdateTextureRegions`. Locking the mip and calling `UpdateResource` measured
  4 ms; this path measures 0.1 ms.
- Markers (the survivor's facing arrow, bed, backpacks, POIs) are drawn only
  on explored ground. A survey map shows neither edits nor POI interiors.
- The quests CI gate opens the map, walks, and checks it drew, grew and saved.

### Zombie variety

- **Abilities are data** on the zombie definition (`abilities.ranged`,
  `abilities.scream`), not subclasses, like every other difference between
  variants.
- **Spit reuses the arrow.** A spitter fires `AMadProjectile` with a lob that
  cancels the drop over the flight. To make that work, the survivor now
  implements `IMadDamageable` too (acid infects), and a projectile ignores
  actors of its shooter's own class, so a spitter behind the horde does not
  shoot its way through it.
- **A screamer is a force multiplier.** Its scream goes through the horde
  director: every live zombie in the radius is alerted to the survivor, and
  wanderers spawn out of sight already hunting. That is what makes the one to
  kill first obvious.
- A "zombie variety" CI gate checks a spit hits and hurts, and a scream alerts
  and summons.

### Traps

- **Data on the block**: `trap { damage, seconds, wear, slow }`, which requires
  collision none. Wooden spikes, iron spikes and barbed wire ship.
- **Walking into them is the point**, so the pathfinder now sees every
  collision-none block as open space (`MadFall::Traps::ForPathing`, backed by
  a 65,536-bit table built once from the registry). That also fixed a quiet
  bug: an open door used to be a wall that zombies broke down.
- Zombies and animals query the voxels at their feet and chest each think.
  A trap's hit wears the trap through the structural damage path, so spikes
  break and fall like any block. The zombie-variety CI gate walks a zombie
  across a spike field.

### Weather

Six-hour fronts of clear, cloudy, rain or storm, turning to snow below freezing:

- **A function of seed and time.** `MadFall::Weather::Evaluate(seed, hours, air)`
  rolls each front from a hash and blends neighbours over an hour, so the
  weather needs no save: a loaded world has the sky it would have had. The
  first twelve hours of a world are always clear. Tests check determinism,
  each kind's share, and that nothing jumps between samples minutes apart.
- **One state, many effects.** Cloud dims the sun and sky light; the exposure
  bias drops and the picture desaturates with the gloom, or auto exposure
  would light an overcast noon like a clear one. The map's height fog thickens
  with rain, and the survivor's air temperature falls: -3 C in rain, -6 C in a
  storm. That matters with clothing.
- **Rain and snow for one transform a frame.** Streaks are instances fixed in a
  box around the camera. Each is placed twice, a box height apart, and the
  whole component scrolls down and wraps, so there are no particle assets and no
  per-drop updates. Three layers switch on as the rain grows heavier, and all
  hide when a voxel roof is overhead.
- **Sound** has looping ambient channels in the audio subsystem: a procedural
  wave kept fed by re-queueing its samples. The rain and wind loops are
  synthesised to tile (cross-faded tail, whole-cycle gusts). Thunder comes with
  a flash of sky light.
- `mad.weather.set <kind|auto>` forces weather, blending over six seconds. The
  frame-budget CI session now runs in a forced storm.

### Difficulty and the help page

- **A world property**, chosen on the New World page and written to
  world.json next to the seed. The voxel world reads it with the seed, so
  every system asks `MadFall::Difficulty::GetWorldScale(World, key)`.
- **Numbers are data**: three tuning definitions of multipliers (zombie damage
  and health, horde size, animal damage, hunger and thirst). An unlisted key
  is 1, so normal is an empty object and a mod patches one value. Tests check
  easy < normal < hard for every key.
- **How to Play** is a title-screen page built from `menu.help_N` strings, a `#`
  prefix marking a heading, so a translation covers it without code.

### Sound

There are no audio assets, so every effect is synthesised in code
(`MadFall::Synth`): noise, sine and sawtooth oscillators, one-pole filters and
envelopes. Struck stone is a short bright noise burst over a low knock, wood a
damped resonance, metal inharmonic partials that ring, dirt a dull thud, foliage
a high-passed rustle; a break is a strike followed by smaller duller pieces; a
groan is a vibrato sawtooth through a low pass with breath; a collapse is brown
noise with cracks; the horde horn two detuned sawtooth drones a fifth apart. Four
seeded variations of each, plus a random pitch spread, so repetition does not
sound mechanical. `MadFall.Audio.Synth` checks every sound is deterministic per
seed, varies across seeds, is peak-normalised without clipping and is not silent.

`UMadAudioSubsystem` plays them. Game code reports what happened - a block of
some material class was hit, a zombie attacked - and the surface's `impact`
decides the voice, so a mod re-voices a whole class in JSON. All variations are
generated on a worker when the world starts: the three-second horn took 4.5 ms
on the game thread the first time a horde arrived. Playback feeds the PCM to a
`USoundWaveProcedural`; procedural voices never end on their own, so each is
tracked and stopped when its samples run out. At most `mad.audio.MaxVoices` (24)
play at once, with a minimum gap per sound so forty zombies are not forty
simultaneous groans. Volume is `mad.audio.Volume`, a menu setting.

Hooked: mining hits and breaks, placing, footsteps by the material underfoot,
the survivor's hurt voice on any health drop, zombie groans (more often when
hunting), attacks and digging, collapses landing, pickups, finished crafts, the
horde-night horn and menu clicks. Plays are counted even with no audio device,
which is how the survival and zombie gates check events reach the audio system
(`mad.audio.stats`). `mad.audio.play <name>` auditions one.

**Recorded sounds replace most synthesised ones.** 27 of the 36 sounds now play
recordings from CC0 packs: Kenney's Impact Sounds and RPG Audio (hits, breaks
and footsteps per impact kind, placing, pickups, crafting, wood creaks), the
Summoning Wars zombie pack (groans, attack grunts, screams), and OpenGameArt's
100 CC0 SFX #2, 30 CC0 SFX loops and Wind Whoosh Loop (stone breaks, thunder,
rain, wind, clicks). `Scripts/fetch_audio.ps1` downloads them (12.5 MB) into the
uncommitted `SourceArt/audio/`, `Scripts/prepare_audio.py` maps files to sounds
and uses ffmpeg to trim silence from one-shots, fold to mono and peak-normalise
to -2 dBFS - the synthesised sounds' level, so every volume tuned against them
still holds - and `Scripts/import_audio.py` imports them into
`/Game/Audio/<sound name>/` (3.2 MB, committed). The zombie pack's 24 files are
unnamed, so they were sorted by length and spectral centroid: long low ones are
groans, short loud ones attacks, the brightest screams.

`UMadAudioSubsystem` finds recordings by folder through the asset registry and
loads them asynchronously at world start; a sound with recordings plays one at
random (never the same twice running) through the same voice cap, gap and
attenuation, and a sound without - the horde horn, the collapse rumble, the
survivor's hurt voice, the bow, the spit, non-wood creaks - and every sound
before the loads land keeps its synthesised voice. A recorded loop is imported
looping and played by one voice. `mad.audio.stats` reports how many sounds are
recorded. Tested: `MadFall.Audio.Recordings` (every folder names a sound, every
wave loads and has length, loops loop, at least 25 sounds recorded).

**Music** (`UMadMusicSubsystem`): three CC0 tracks from OpenGameArt in
`/Game/Music` - "EmptyCity" by yd by day, "Cold Silence" by Eponasoft by night,
"Zombies' March" by yd while a horde attacks (`MadFall::Music::ChooseTrack`).
Day and night tracks play through and then leave 60-150 s of quiet
(`SilenceAfter`), so a whole day is not one theme on repeat; the horde track
loops for the length of the horde. A change fades the old track over four
seconds and, except for a horde arriving, waits six before the new one.
`Scripts/prepare_audio.py` normalises them to -20 LUFS, under the effects, at
32 kHz stereo, with the six-minute night track cut to 150 s with fades: at full
length and rate the three were 95 MB of PCM, against a 1 GB LFS allowance
(25 MB imported). `mad.audio.MusicVolume` (0.6) is its own menu setting. Starts
are counted without an audio device; the zombie gate checks the horde track
started on horde night (`mad.music.status`). Tested: `MadFall.Audio.Music`.

**Zombie bodies were also a frame-budget change**: fourteen rig components
registered inside `SpawnActor` put a horde wave's construction on one frame
(3.3 ms); rigs now assemble on their first tick, at most two per frame.

### Text and translations

`FMadStringTable` (Core) maps language -> key -> text, loaded from
`definitions/strings/` through the same sources and load order as everything
else; `display_name: "@items.wood_plank"` resolves through it. Lookup falls
back current language -> English -> a readable name from the key, so a gap in a
translation degrades to English and a gap in English degrades to "Wood Plank",
never a raw key. Name lookups for the HUD and messages go through
`FMadGameplayDefinitions::GetItemName`/`GetPerkName`/`GetBlockName`; logs keep
ids, because CI and bug reports grep them.

*Why JSON and not FText string tables:* Unreal's string tables and `.locres`
are cooked assets from the localization dashboard, and a translation pack is a
data mod with no editor. The cost is no ICU plurals or gender; Phase 7 can
feed these strings into `FText` where UI needs formatting.

`MadFall.Mods.Strings` covers the rules; `MadFall.Mods.ShippedStrings` and
`MadFall.Items.ShippedContent` fail if any shipped `@key` lacks English, or if
the `example_german` pack translates a key English does not have.

### Inventory screen

`I` opens the backpack; `E` on a crate opens it beside the backpack (and rolls
its loot the first time, which is when discovery experience is given). Click
picks a stack up, a second click puts it down, right-click picks up half,
shift-click quick-moves (backpack to crate when one is open, otherwise hotbar
and backpack trade). A crate that is broken or walked away from closes the
screen.

**The rules are pure functions** (`MadFall::InventoryOps::MoveSlot`,
`QuickMove`) over two `FMadInventory`s, which may be the same one; the player
only picks which two and the HUD only turns clicks into slot indices, so
`MadFall.Items.InventoryMoves` checks every rule - partial moves, merge caps,
swaps, tools never merging, quick-move ranges - by counting items before and
after.

**A picked-up stack stays in its slot** until it is put down. The cursor is just
"which slot is selected", so closing the screen, dying, or the crate being
destroyed mid-drag cannot lose or duplicate anything. The cost is that the
dragged stack is not drawn under the mouse; the panel says what is held.

**Canvas hit boxes, not UMG.** `AHUD::AddHitBox` per slot and
`NotifyHitBoxClick` keep the screen asset-free like the rest of the HUD. Enhanced
Input still sees the click, so `UsePrimary`/`UseSecondary`/look handlers return
early while the screen is up. Phase 7's UMG screen replaces the drawing and
keeps the verbs.

Console (used by the survival gate): `mad.player.store <item>`,
`mad.player.takeall`, `mad.player.invmove <b|c> <slot> <b|c> <slot> [count]`,
`mad.player.container`, `mad.player.closeinventory`.

### Known gaps in Phase 4 so far

1. **Progression is flat.** Levels grant perk points and perks are data, and
   the inventory screen has a skills column (rank pips, what the next rank
   does, a Take button greyed with the reason it cannot be taken:
   `MadFall::Perks::CanBuy`, `DescribeRank`).

   **FIXED: perks form trees.** A rank can require ranks of other perks
   (`FMadPerkRank::Requires`, `"requires"` in JSON); `TryBuy` refuses with
   `MissingPrerequisite` after the level check, so a greyed button shows the
   nearer goal first. Requirements are per rank, so the shipped perks open
   freely and gate their top ranks (Miner 3 needs Athlete, Brawler 3 and
   Survivalist 2 need Tough, Artisan 2 needs Survivalist). Load removes what
   could lock a rank forever - unknown perks, ranks a perk lacks, cycles (the
   same depth-first pass as nested loot tables) - reporting each. The skills
   column is ordered by `GetTreeOrder`: attribute (first tag) headings, then
   depth within the attribute, indented with a branch mark; a requirement in
   another attribute does not indent, because the perk would hang off nothing
   on screen, and the button text names it instead. Prerequisites gate buying,
   not effects: ranks a save already owns keep working. Tests:
   `MadFall.Progression.PerkPrerequisites`.
2. **The inventory screen is functional, not finished.** Hovering a slot shows
   a tooltip built by `MadFall::Items::DescribeStack` (damage after mods,
   durability, effects, protection, installed mods), and an item mod put down
   on a tool is installed rather than swapped (`IsModInstallDrop`; backpack to
   backpack, since `InstallMod` takes the mod from the tool's own inventory).
   Hover is tested while the slots are drawn, not through hit-box cursor
   events, which need the cursor to move. Clicks go through
   `AMadHUD::ClickBox`, which `mad.hud.click` also calls, so the CI gate
   "inventory screen" drives the exact path a mouse does.

   **FIXED: the rest of the inventory screen.**
   - *A drag image.* The held stack still stays in its slot until put down;
     `AMadHUD::DrawHeldStack` draws a ghost of its icon and the held count
     beside the mouse, over everything else, so a click's result is visible.
   - *Exact counts.* `FInventoryCursor::Count` replaces the half flag: 0 is the
     whole stack (which can still swap), otherwise the part taken. While a
     stack is held the mouse wheel takes one more or one fewer
     (`AdjustHeldCount`, 1 to all); right-click still starts at half. The count
     is clamped again on drop, since the stack can shrink while held (eaten
     from the hotbar, used by a craft).
   - *Sorting.* A Sort button on the backpack (not the hotbar, which is the
     player's own arrangement) and on an open crate:
     `MadFall::InventoryOps::SortRange` pools plain stacks of an item into full
     stacks and orders tools, weapons, mods, consumables, blocks, resources, by
     name, the freshest tool first, unknown items last. If pooling needs more
     slots than the range has - a stack above a patched max_stack - it only
     reorders, so a sort never destroys items.
   - *Removing mods.* Ctrl-click a tool in the backpack takes its last mod off
     into the backpack (`MadFall::Items::RemoveMod`), refused whole with no room.
     Mods were permanent so they could not be swapped every swing; limiting
     removal to the inventory screen keeps that, and a player can now move a
     mod to a better tool.
   Tests: `MadFall.Items.InventoryMoves` (sort order, pooling, conservation,
   oversized stacks), `MadFall.Items.RepairAndMods` (removal, full backpack);
   the inventory-screen gate removes the grip with ctrl, holds 3 of 4 rocks,
   drops them and sorts (`mad.player.slots`, `mad.player.invheld`).

   **Items have icons, painted from their data** (`MadItemIcons.h`). Slots
   showed a name cut to eight letters ("Wood Pla", "Canned F"), which read as a
   debug view. There are no icon assets, but every item says what it is, so
   `MadFall::Icons::ChooseShape` picks a picture from kind, tags, wear slot and
   id (a pickaxe, a bow, a can, a bottle, a helmet, seeds, a coin, a gear for a
   mod...) and `Paint` draws it as 32x32 pixel art with grain and a dark
   outline; a block item is an isometric cube in its surface's colour with a
   hint of its pattern. A mod item with its own `icon` texture uses that. The
   pixels are pure, so `MadFall.Items.Icons` checks every installed item paints
   a picture, deterministically, mostly distinct, and that wood is warm and
   concrete grey; `MadFall::IconCache` turns them into rooted transient textures
   (nearest filtering) once per item, and nothing without a renderer. The
   hotbar, slots and crafting rows draw icons, counts are right-aligned and
   shadowed, and the selected hotbar item's name sits above the hotbar.

   **Crafting is a column of the inventory screen**, beside the backpack it
   draws from, with a Skills tab next to it (Tab opens it on Crafting, I on the
   last tab). It replaced a wheel-scrolled list of every recipe, which could not
   be read past a few dozen recipes and hid what was missing. Recipes are
   grouped by what they make (`MadFall::Crafting::CategoryOf`: tools, weapons,
   item mods and the ammunition a weapon fires under Tools; block items
   Building; consumables Food; clothing Wear; the rest Other), so a mod's
   recipes file themselves with no category field to author. The list puts what
   can be made now first, then sorts by name (`ListRecipes`), and the detail
   pane shows each ingredient as have / need in green or red, plus a missing
   station or level. Tests: `MadFall.Items.RecipeList`; the CI gate "inventory
   screen" filters to craftable tools, selects the stone axe and crafts it
   through the same hit boxes a mouse clicks.
3. **Two worlds initialise in `-game`** (the startup world, then the map), so
   world subsystems log their startup twice. Harmless - the first is torn down
   before play - but it doubles registry-independent startup work.
4. **Placeholder presentation.** The HUD is canvas text and surfaces are flat
   vertex colours on one placeholder material (the Phase 2 grey-surface bug is
   fixed). Sound is synthesised (see "Sound" below) rather than recorded. Zombies were tinted cylinders; they are now
   `UMadHumanoidRigComponent` figures - six boxes on hip, shoulder and neck
   joints, posed in code each frame (`MadFall::Humanoid::ComputePose`,
   `MadFall.AI.HumanoidPose`): a shamble with arms reaching forward scaled by
   speed, a raised two-arm swing on each attack on the player or a block, a red
   flash when hit, and a face-down topple on death. Posing is skipped for
   zombies not rendered in the last quarter second.

   **Characters are pixel art, not tinted primitives** (`M_MadCharacter`,
   `Scripts/make_character_material.py`). The rig was cylinders and boxes in
   flat colours: a green head on a black box read as a placeholder and clashed
   with blocks drawn 16 texels a voxel. Now every part is a box with one
   material instance setting its part (skin, shirt, head, trousers, fur, animal
   head), size and colours; the HLSL draws from the part's local position at
   ~3 cm texels, and a face on an 8x8 grid on the head's forward face. A zombie
   (the default) has rotting blotches and wounds, red eyes under a heavy brow,
   an open mouth with teeth, sleeves torn at the elbow, and blood and tears in
   its clothes; a trader (`SetLiving(true)`) has eyes, brows, a mouth and clean
   clothes. Skin is the definition's tint; shirt, trousers and hair come from
   small palettes hashed from the actor and definition, so a horde is a crowd
   rather than a row of copies (they used to derive from the skin, and every
   zombie wore the same green-black). Animals use the same material: fur, a
   pale belly, and eyes on the head's sides. Cost: one dynamic material
   instance per part (six for a zombie), all set again on a hit flash. Without
   the asset the rigs fall back to the tinted engine material.

   **Humanoids are animated mannequins where they can be.** The box figures
   read as a voxel toy next to photo-textured ground. The engine install ships
   Epic's UE5 Manny and Quinn with idle, walk, jog, hit-react and death
   animations in its Third Person template; `Scripts/copy_mannequin.ps1` copies
   them (101 MB, no download) into `Content/Characters/Mannequins`, which is
   gitignored because the EULA does not allow publishing uncooked Epic content
   in a source repository. When they are installed and the game renders,
   `UMadHumanoidRigComponent` draws a skeletal mesh (a third of zombies and
   half the living are Quinn) instead of the boxes; a fresh clone, a server and
   every `-nullrhi` CI run keep the boxes, and `mad.characters.Skeletal 0`
   forces them.
   - `UMadCharacterAnimInstance` has no animation blueprint: its proxy samples
     idle, walk and jog, blends them by ground speed with walk and jog sharing
     one stride phase (`MadFall::CharacterAnim::ComputeBlend`; a 1.2 m/s
     zombie plays the 3 m/s walk at 0.4, which reads as a shamble, clamped at
     0.35 against slow motion), and blends in the hit reaction and the death
     fall. Root motion is extracted and dropped so the capsule moves the body.
   - The zombie look is procedural on top: upper arm then forearm aimed along
     `ArmReachDirection` (forward, a little down, lifted through an attack
     like the box rig's swing) and the chest pitched 12 degrees. Aiming only
     the upper arm left the walk's bent elbow, and the hands met at the chest.
     The template's ABP had no input for this, and editing a copy would put an
     unpublishable binary at the centre of the characters.
   - The mannequin material's Paint Tint takes the skin colour (head, arms,
     legs) and the clothes colour (torso), matte, with the logo hidden; a hit
     flashes it red.
   - **Zombies wear decay as an overlay material** (`M_MadZombieOverlay`,
     `Scripts/make_zombie_overlay_material.py`). A green-tinted mannequin read
     as a clean painted robot. The overlay is a second translucent pass over
     the mannequin's own shading, so its panel normals survive and nothing
     references the uncommitted Epic textures: grime heavier towards the feet,
     grey-green rot blotches and blood around the mouth, down the chest and on
     the hands, all from the pre-skinned position (so a wound stays put as the
     body moves) and shifted per zombie by its seed, with rot and blood amounts
     varied too. The pre-skinned position and normal only exist in the vertex
     shader and reach the pixel shader through vertex interpolators - wired
     directly, the material fell back to the default. Not drawn beyond 40 m
     or on the living. Found on the way: in an uncooked `-game` run a new
     material draws nothing until its shaders compile, which looked like a
     broken overlay for three probes; a cooked build compiles them at cook.
   - Cost: posed only when rendered (`OnlyTickPoseWhenRendered`), with update
     rate optimisations for distant ones, fixed bounds and no bone updates to
     physics; evaluation runs on animation workers. Measured with a CSV
     profile, 30 zombies in view and AI off: game-thread animation 0.46 ms a
     frame (0.03 ms for box figures; 0.54 before the last two settings), whole
     game thread 5.0 against 4.8 ms, 1.5 ms of worker evaluation. This cost is
     the engine's and outside `MAD_FRAME_SCOPE`, so the frame-budget gate does
     not see it; a much larger visible horde would want the animation budget
     allocator.
   Tested: `MadFall.AI.CharacterAnim` (blend weights and rates, the reach
   direction through a swing, and the installed sequences when present).

   **The deer and the wolf are animated models too** (Quaternius, Ultimate
   Animated Animal Pack, CC0, via Poly Pizza; no CC0 animated rabbit or boar
   was found that was not a cartoon character or a block toy, so those two keep
   their proportioned figures). Definitions name the model and a clip per role
   in `appearance.model` (`FMadAnimalModel`), and `UMadQuadrupedRigComponent`
   drives them with the same `UMadCharacterAnimInstance` as the mannequins,
   given the model's cycle speeds, a whole-body attack clip blended through
   each attack, and a graze clip in place of the idle while grazing.
   - `Scripts/prepare_animals.py` rewrites the downloaded glTFs before import.
     The pack carries a 100x scale and a -90 degree turn on the armature and
     mesh nodes, which Unreal's importer does not carry through a skinned mesh:
     the first import had the deer's head and tail bones on one side of its
     body and bone bounds kilometres wide; fixing the scale alone stood it on
     its tail. The script bakes both into joint translations and rotations,
     their keys, vertices, normals and inverse bind matrices, and the import
     keeps one copy of each clip (every clip came in twice).
   - `FitModel` scales the model uniformly to the figure's height and lifts its
     lowest point to the ground, so the capsule, sized from the same
     proportions, still matches.
   - `UMadCharacterAssetSubsystem` loads the mannequins and every animal model
     asynchronously when a rendering world starts; rigs wait up to ten seconds
     for it rather than load a skeletal mesh, skeleton, physics asset and clips
     on the frame their first character appears. Only packages that exist are
     requested, so a checkout without the mannequins loads quietly.
   Tested: `MadFall.Animals.Definitions` (the model block parses, rejects ids and
   inverted cycle speeds, every shipped clip loads, `FitModel` scales and lifts).

   The survivor's own hand was invisible, so a swing had no visible feedback.
   `UMadViewModelComponent` (on the camera) draws the held item from basic shapes
   chosen by kind and tags (`MadFall::ViewModel::ChooseShape`): a handle with a
   pick, axe or shovel head (stone-grey for stone tools), a club, the block tinted
   with its surface colour, a can, a bottle, a resource cube, or a fist. It swings
   forward and down on every tool use, pushes out when placing or eating, and bobs
   with the stride (`ComputeOffset`, `MadFall.Items.ViewModel`); it hides while the
   backpack or crafting screen is up. Drawn at 60% life size: full size filled
   half the screen at a 90-degree field of view.
5. **Zombies do not stack** (climb on each other). **FIXED (Phase 6): they avoid
   each other and resist damage.** Crowd avoidance is the engine's RVO on the
   zombie's movement (consideration radius 160 cm, weight 0.5): a horde
   converging on a survivor was one column of overlapping capsules; in a probe
   eight zombies now surround a pillar from several sides. Walking only; its
   cost shows in the frame budget gate's horde. Damage resistances are
   multipliers by damage type on the zombie definition (`resistances`, as on
   blocks), applied inside `ReceiveHit` so swings, arrows, traps and debris all
   respect them; a weapon with several types hits with the one that does the
   most after resistance (`MadFall::Combat::ChooseDamage`), and a swing that
   does little says so. Tests: `MadFall.AI.ZombieResistances`. They do climb and dig down now: the pathfinder has a `Climb`
   move (up and down ladders, which pathing otherwise sees as open air, via
   `FMadPathSettings::IsClimbable`; up any wall for `abilities.climbs_walls`)
   and a `DigDown` move (through the floor toward a goal below, falling to the
   next ground). A zombie follows a climb step by flying up its column, slowly
   (140 cm/s), and drops back to walking the moment the step is done or it
   stops to attack - nothing holds a zombie on a wall. A path planned from
   mid-air starts where the walker will land, except for one holding on to a
   ladder or (for a climber) a wall: zombies re-plan every 2-3 s, and planning
   from the foot of the wall sent a climber back down each time, so a five-voxel
   climb finished only when it happened to fit between re-plans (the variety
   gate failed one run in three). Tests: `MadFall.AI.Climbing`
   (ladders up and down, a climber on a bare cliff and re-planning halfway up, no climbing on air, digging
   into a cellar, wanderers never digging); the zombie variety gate sends
   `madfall:zombie_climber` up the pillar a survivor stands on. Dig-down is
   covered by the unit test only: through natural terrain it takes a civilian
   tens of seconds per voxel, too slow for a gate. A survivor out of reach above is handled by
   undermining (below), which only brings them down when what they stand on is
   construction: natural terrain never collapses, so a dirt spire is a refuge.

   *Undermining:* when no path gets within attack reach and the target is more
   than one voxel up, `MadFall::Pathfinding::FindUndermineTarget` picks a
   breakable voxel in reach (adjacent, feet or head height) under or beside the
   target's column - the column itself first, then the quickest break - and the
   zombie hits it through the normal block damage path. The structural solver
   does the rest. Measured headless: a civilian next to a 3-high wood pillar
   broke 2 blocks, the pillar's top collapsed, and the survivor dropped 3 voxels
   into reach. Tests: `MadFall.AI.Undermine`; the zombie gate stands the survivor
   on a pillar (`mad.player.pillar`) and checks they come down.

   **FIXED: a sealed base kept the horde out.** Measured with a 13 x 13 hollow
   concrete shell around the survivor (`mad.voxel.box ... hollow`) and nineteen
   horde zombies outside: 2,574 paths and 8 block hits in two minutes, nobody
   inside. `FindPath` allows digging, but at `DigCostPerSecond` a concrete wall
   costs as much as a long walk, and on open ground the 1,500-node budget ran
   out exploring detours before the path through the wall was ever cheapest;
   the best partial path ended at the wall and the zombies stood there
   re-planning. *Breaching:* when no path reaches the target, undermining does
   not apply, and the zombie is already where its partial path ends,
   `MadFall::Pathfinding::FindBreachTarget` picks the block to break among the
   eight columns within 60 degrees of the target - the most direct column
   unless a similarly direct one is quicker, feet voxel before head - and it
   breaks it as a standing step. The same shell with breaching: 41 breaches and
   274 block hits, survivor reached inside 80 s. Structural jobs under that
   load: 23 completed, 0 restarted - the "edited every frame" starvation below
   did not appear, since damage stages change only every few hits. Tests:
   `MadFall.AI.Breach`; a CI gate ("horde breach") runs the shell.
6. **Stress is a readout, not shading.** The HUD shows the targeted block's load
   ("load 72%", green to red, "FAILING") from
   `UMadStructuralSubsystem::QueryStress`: a separate probe job on its own
   0.25 ms budget (`mad.si.ProbeBudgetMs`), refreshed about once a second, that
   gives up above `mad.si.ProbeMaxMembers` (16384) so aiming at a huge structure
   cannot hitch. `mad.player.stress` is the console version; the survival gate
   checks it resolves for a placed block.

   **FIXED: strained structures creak.** A solve that an edit caused and that
   leaves the structure standing reports its most stressed surviving member
   (`FMadStructuralJob::GetMostStressed`); at `mad.si.CreakStress` (0.85) or
   more, `OnStructureStrained` fires and the debris subsystem - already the
   voice of collapses - plays a creak in the block surface's impact kind,
   louder toward the limit. The member is picked during the load sweep, where
   each member's load is final when visited, so it costs no extra pass over a
   65k-member structure. Jobs seeded only by chunks streaming in stay silent,
   or every stressed POI would groan as the survivor walked past, and creaks
   are at least `mad.si.CreakCooldown` (1.5 s) apart so building onto a
   strained frame warns instead of droning. A collapse is its own noise, so a
   job with failures does not also creak; the cascade's next solve, once the
   failed blocks are gone, is what creaks if the remainder is at its limit.
   The sounds are synthesised like the rest (`MadFall::Synth`): stick-slip
   pulse trains through resonators for wood and a long low groan for metal,
   grinding brown noise with cracking ticks for stone, a trickle for dirt.
   Tests: `MadFall.Structural.Strain` (tip at span, failures skipped, a
   loaded post); the structural gate checks the 6-block cantilever creaks at
   100% through the live subsystems.

   **FIXED (Phase 6): stress is shaded around the aim.** While the survivor
   holds a block to place and aims at a structure, `UMadStressOverlaySubsystem`
   washes the members within `mad.si.OverlayRadius` (8) of the aimed block in
   colour - blue sound, green at half, yellow at 0.8, red at the limit, pulsing
   red failing - with each block's outline brighter so neighbours of close
   stress still read apart. It reuses the probe solve the readout already runs:
   when the probe finishes, `FMadStructuralJob::CollectStressNear` keeps the
   nearest `mad.si.OverlayMaxBlocks` (768) members' stress, so there is no extra
   solve, and moving the aim along a wall keeps the old shading until the next
   solve lands. One instanced component of translucent boxes 2% larger than a
   voxel, stress and failing in per-instance custom data for
   `M_MadStressOverlay` (`Scripts/make_stress_material.py`), rebuilt only when
   the samples change, about once a second. A whole-building per-chunk solve was
   not needed for what a builder looks at. `mad.si.Overlay 0` turns it off.
   Tested: `MadFall.Structural.Load` (nearest first, radius, cap, stress equal
   to the member's report).
7. **A structure edited every frame can starve its own job** (restarted each
   time it is touched). A warning logs after 8 consecutive restarts. Incremental
   repair of a finished solve is the fix if horde nights show it. Measured once
   a horde could breach a base (a 531-block shell, 274 block hits in 80 s): 23
   jobs, none restarted, 6.7 ms total - not a problem at that scale.
8. **A structure is re-checked once per chunk it spans.** Load seeding (below)
   seeds every member of each arriving chunk, so a POI across four chunks is
   solved up to four times as they stream in. Each solve is time-sliced and a
   tier-1 POI is ~0.1 ms, but a very large player base loading in pieces pays
   the gather repeatedly.
9. **FIXED: debris fell rigidly.** A cluster landed as a unit on its first
   contact, so a long beam that clipped a post hung off it in mid-air.
   `MadFall::Debris::SplitUnsupported` now splits a landing cluster by (x, y)
   column: columns whose lowest block rests on something land, the rest shear
   off as a new cluster that keeps the drop and speed and falls on (with its own
   visual, and without re-hitting pawns the whole piece already hit). A slab on
   uneven ground settles column by column. Tradeoff: nothing pivots or cantilevers
   - an overhang attached to the part that landed still breaks off - because a
   rigid-body solve is exactly what the kinematic model avoids, and rubble does
   not hold overhangs anyway. `Debris: ... N sheared off` in `mad.debris.status`.
   Tested: `MadFall.Structural.Debris` (a 7-block beam over a post lands one
   block; six fall on to the ground; a flat landing does not split).
11. **FIXED: falling through ground that has not loaded.** Spawning and travel
   wait for the chunks around the survivor, but a raw teleport or a fall out
   ahead of streaming dropped them through chunks with no collision yet -
   teleported 70 m up into unloaded ground, the survivor came to rest 43 m under
   it. `AMadPlayerCharacter::TickTerrainHold` hangs a falling survivor where
   they are while the chunk at their feet or just below is unloaded or has never
   been meshed, and lets go 0.25 s after it settles (collision cooks a few
   frames behind the mesh). A chunk being rebuilt does not hold: it keeps its
   old mesh and collision meanwhile. The first version held on any rebuild
   too, which froze the survivor mid-fall whenever they dug or built in their
   own chunk - the gameplay gate caught it as a scripted walk that made no
   footsteps. Two lookups a frame, only while falling. The
   gameplay gate teleports 80 m up at (900, 900) and checks the survivor was
   held and landed above ground.
10. **Debris passes through pawns.** A survivor or zombie is hurt once by a
   cluster sweeping through their voxels, but the cluster does not stop on
   them; it lands on what is below.

   **FIXED: rubble no longer entombs whoever it falls on.** Rubble used to
   settle into the voxels a pawn stood in, so a survivor who lived through the
   hit was sealed inside a block and had to dig out, or died in a doorway.
   Landing now collects the voxels every living character's capsule occupies
   plus half a voxel of skin to the sides and above
   (`MadFall::Debris::GetPawnVoxels`) and writes no rubble there; that
   block's share drops as salvage like rubble with nowhere to go
   ("N kept out of a pawn's space" in `mad.debris.status`). The skin is not
   cosmetic: rubble is isosurface terrain and bulges about half a voxel into
   its neighbours, and written right against the capsule the pile overlapped
   it - resolving the overlap shoved a boxed-in survivor 22 voxels down into
   solid granite. The cost is that a collapse straight onto someone leaves no
   rubble in the 3x3 columns around them, only salvage.
   `mad.player.buried` ignores the voxel the feet stand in, which smooth
   ground partly fills. The hit itself is
   unchanged. Tests: `MadFall.Structural.DebrisPawnsAndDrops` (centred,
   straddling, brushing); the gameplay gate drops a slab with every block
   turned to rubble onto the survivor and checks `mad.player.buried`.

### Known gaps after Phase 3

1. ~~**No surface scatter.**~~ Resolved in Phase 5: biome `scatter` grows
   trees, boulders and plants from cubic blocks, terrain and model blocks (see
   "World scatter"). Grass tufts and real tree meshes still need art.
2. **Prefabs are authored by script or by `mad.prefab.capture`, not by an editor
   volume.** The brief asks for in-editor prefab volumes. The capture command
   works in any world, including PIE, and produces the canonical JSON; an
   editor-mode volume actor with a Capture button is a usability layer on top
   of it that does not exist yet. Captured prefabs also mark all empty space as
   `air`; turning padding into `void` is a manual edit.
3. ~~**Markers are data only.**~~ Resolved in Phase 4: loot markers sit on
   container blocks rolled by `UMadContainerSubsystem`, spawn markers wake
   sleepers through `UMadHordeSubsystem`. Looted state persists with the
   containers in `gameplay.json` (Phase 4 persistence).
4. **FIXED: roads have banks.** A road into a slope was a trench: its surface
   followed the smoothed, unwarped height field, but the real ground - that
   height moved by the 3D warp, up to 9 voxels - stood above the 4-voxel
   clearance in walls. `FMadPoiPlanner::StampRoadBanks` runs before each road's
   body and shapes every column within `MadFall::Roads::MaxBank` (10) of the
   road's edge to `BankHeight`: the road's height at the edge, easing by
   smoothstep to the generated ground, over a width of 1.5 voxels per voxel of
   difference, so banks stay at or under 45 degrees until a cutting is deeper
   than about 7 voxels, where they steepen rather than shave the hill flat. It
   cuts earth above the bank, fills hollows below it with the ground's own block
   (a grassy slope stays grassy), and on the road itself clears earth above the
   clearance that would otherwise roof a deep cutting. The generated ground is
   found from the noise (surface + warp), not from the chunk's voxels, and each
   column takes its single nearest centreline point, so every chunk - above,
   below or beside - shapes a column the same way. Columns holding construction
   or another road's surface are left alone. Road search pads and the scatter
   keep-out include the bank. Generation version 4: unedited chunks regenerate
   with banks; stored ones keep their old shape. Test
   `MadFall.WorldGen.RoadBanks`: along real roads, road edges beside a
   neighbour standing 3+ voxels higher went from 151 of 946 to 4 of 944.
   Cost: per chunk a road crosses, a nearest-point search per column against
   the ~100 centreline steps near the chunk, and a warp root search for columns
   in the band, on the generation worker. The first version also did this in
   every chunk a road's search reaches (64 voxels above and below it) and looked
   each column's height up again; the frame-budget gate caught it - generation
   workers so busy the far terrain built 4 of 195 tiles. The banks now take the
   column heights GenerateChunk already has, skip a chunk more than warp slack
   plus 24 voxels from every column's surface, and skip a column's warp search
   when the span between road and ground misses the chunk: 195 of 195 again,
   and the bank test's numbers unchanged.

   **FIXED: bright lines along every surface boundary.** A smooth-terrain quad
   goes to the section of its first corner's surface, but each vertex carried
   its own surface's vertex colour - including the alpha that holds the pattern
   index. Interpolated across a triangle between, say, gravel and snow, the
   index passes through every index in between, so a thin band of water, metal
   or plank pattern drew bright zig-zag lines along roads, beaches and grass
   edges. Vertices now take RGB from their own surface (colour still blends)
   and alpha from their section's. Test: `MadFall.Mesher.SmoothMaterialBoundary`.
5. **The prefab set does not feed the generation version.** Adding a prefab mod
   changes where POIs appear in *ungenerated* chunks, exactly as adding a biome
   mod does; it cannot corrupt chunks already on disk.
6. **Terrain is gentle at scale.** Height spans −9 … 62 over hundreds of
   voxels, which is geologically reasonable but visually mild. Making it
   dramatic is art direction (biome `roughness`, `ridging`, `height_variation`
   are all data), not engineering, and is better tuned against real materials.
7. **`ClimateContrast` saturates.** Values beyond the range become plateaus.
   That is the right shape for deep ocean and high plateau, but it does mean
   the tails of the climate distribution are flattened rather than rare.

### Known gaps after Phase 2

**Rendering fidelity — the half of Phase 2 that is not done.** The brief asks
for triplanar PBR with virtual texturing, Nanite props, mesh decals, weather and
a dynamic day/night cycle. None of that exists, and most of it cannot until
there are textures and meshes to work with. What is there instead:

1. **A placeholder material.** `Content/Materials/M_MadVoxel` reads base colour
   from vertex colour, giving each block material class a distinct stable shade.
   Real triplanar PBR blended by slope, altitude, moisture and biome lands when
   art does.

   **PARTLY ADDRESSED (Phase 6): procedural patterns and corner occlusion.** See
   "Block look" below. Still no textures, normal maps or slope/biome blending.

   **FIXED (Phase 5): the material's base colour was never connected.**
   `Scripts/make_voxel_material.py` connected the vertex colour node's `"RGB"`
   output, which is not a pin name (the pin is `""`); the call failed without an
   error, so every surface rendered with black albedo - a dark, desaturated,
   specular-only grey. Found by inspecting the saved asset from Python
   (`get_material_property_input_node(MP_BASE_COLOR)` returned None) after a
   cooked build rendered identically to uncooked. The script now checks the
   connection and repairs an existing asset. Two further findings from the same
   investigation: the cooked build did not contain the material at all (it is
   referenced only by a string, so `+DirectoriesToAlwaysCook=/Game/Materials`
   was added), and vertex colours are read as linear, so surfaces emit linear
   bytes. The remaining wash-out was lighting and palette: FIXED in Phase 5,
   see "Sky, exposure and light blocks".

   *Earlier notes, kept for the record* - the checker-default symptom they
   describe predates the current asset:

   - The component genuinely holds the intended material — `GetMaterial(0)` is
     logged after every apply and returns `M_MadVoxel`.
   - It is not our material: the same thing happens with the engine's own
     `/Engine/EngineDebugMaterials/VertexColorMaterial`.
   - Not a shader-model problem: identical at SM5 and SM6.
   - Not an ordering problem: assigning the material before `CreateMeshSection`
     rather than after changes nothing.
   - No missing-shadermap or material-usage warning is logged for it.

   The renderer log names `WorldGridMaterial` compiling, which is what Unreal
   substitutes for a **null** material at scene-proxy build time — so the proxy
   is reading null even though the component is not. Untried next steps:
   capture from a real editor viewport rather than `-game`, and test a cooked
   build. Geometry is unaffected; the mesher is verified by its automation
   tests, not by these screenshots.
2. **FIXED: no LOD.** Measured first: at view distance 16 the loaded chunks held
   3.45 M triangles (0.89 M at the default 8), with meshing the largest share of
   the frame. Chunks now mesh their terrain at three levels by horizontal chunk
   distance from the camera: full detail within `mad.mesh.LodDistance` (5
   chunks, 160 m), every second voxel column within `mad.mesh.Lod2Distance`
   (11), every fourth beyond. `FMadLodSampleGrid` (Core) holds the coarse
   lattice, copied on the worker by `SnapshotLodFromSources`, and the Surface
   Nets pass is one template over either grid.
   - **Horizontal stride only.** Every one of the 34 layers is kept. A vertical
     stride too moved every flat surface by up to half a stride: the sea showed
     a dark square step along each level boundary from a hilltop. Terrain is a
     heightfield, so its triangles scale with ground area and the horizontal
     stride alone gives most of the saving. Normals divide the gradient by the
     cell's width, or every slope would light as if flatter.
   - **Point samples on world-aligned columns**, so two chunks at one level
     agree on every shared point and meet exactly, like two full chunks.
   - **Seams between levels without stitching.** A full chunk's surface ends
     within half a voxel of its boundary; a coarse chunk's ends anywhere in its
     2-4 voxel boundary cells, and the ground showed pinholes along the seam. A
     coarse chunk's outer rows of vertices are pulled half a voxel past its X/Y
     boundaries, so it always overlaps the finer surface it meets (a column of
     chunks is one level, so Z needs nothing). Cost: up to a coarse step of
     sideways shift at a distant seam, where overlap reads better than a hole.
   - **Hysteresis.** Finer detail is taken at once; coarser only one chunk past
     the boundary (`MadFall::ChunkMesher::ChooseLod`), so pacing across a
     boundary does not remesh its ring every time. The viewer's chunk column is
     checked each tick; on a change one pass over the components marks those at
     the wrong level dirty. Empty chunks take their level when next built.
   - **Placed blocks stay full detail** in every chunk: a distant building is a
     few hundred merged faces, and a coarse one would lose one-voxel walls.
   - **Collision** follows the mesh, so beyond 160 m the ground a zombie or
     animal walks on can sit up to a voxel off the pathfinder's; nothing the
     survivor stands beside is that far.

   Result (same spawn, 60 s): view distance 8, 892 k → 588 k triangles, mean
   apply 0.44 → 0.34 ms; view distance 16, 3.45 M → 1.21 M triangles, mean
   worker build 0.89 → 0.43 ms, mean apply 0.52 → 0.32 ms, meshing frames over
   2 ms 23 → 1. `mad.mesh.stats` reports the level mix and level-change
   rebuilds. Tested: `MadFall.Mesher.Lod` (level choice and its band; a flat
   surface at the same height at every level; heights within 1 and 2 voxels;
   ground coverage with no holes across full|half|half|quarter and
   quarter|half|full|full rows of chunks).
3. **FIXED (Phase 5): meshing broke the 2 ms rule while streaming.** Measured
   honestly for the first time (whole-frame publish + launch, not just the
   apply call), a rendered session teleporting across the world had **193 of
   2,325 working frames over 2 ms**, worst 8.8 ms. The costs, each measured:
   component registration up to 2.9 ms, a busy chunk's `CreateMeshSection`
   calls up to 2.4 ms, snapshots up to 1.1 ms each with four per frame, and the
   first material load 5.5 ms inside the first apply. Changes:

   - publishing and launching are **time-budgeted** (`mad.mesh.PublishBudgetMs`
     1.0, `mad.mesh.LaunchBudgetMs` 0.5) instead of fixed counts, with at least
     one item per frame so meshing always advances; a mesh whose new component
     used up the budget is applied next frame. Later, with far terrain, weather
     and more AI sharing the frame, that guaranteed first apply was the culprit in
     every remaining over-budget frame. Now an apply is estimated from its vertex
     count (a running ms-per-vertex) and held back if it will not fit in what
     the frame has left, but for at most six frames in a row, so meshing is never
     starved;
   - released components go to a **pool** (`mad.mesh.ComponentPoolSize` 256),
     kept registered and empty, so streaming reuses them instead of paying
     registration;
   - the **snapshot copy moved to the worker**: the game thread only gathers 27
     chunk pointers (`GatherSnapshotSources`, ≤0.36 ms per frame of launches),
     and the worker copies under read locks with a damage-free sample accessor
     that removed 39,304 hash probes per chunk. An edit colliding with a copy
     waits on the lock for at most one copy;
   - surface materials are **preloaded** during world initialisation.

   **Then the whole frame.** Per-system stats cannot see two systems at 1.2 ms
   each sharing a frame, so `MadFall::FrameBudget` (Core) charges every MadFall
   tick to a system bucket (`MAD_FRAME_SCOPE`), closes the frame at
   `FCoreDelegates::OnEndFrame`, and reports worst frame, frames over 2 ms and
   the largest system in each (`mad.perf`, `mad.perf.reset`;
   `MadFall.Perf.FrameBudget`). Its first run found meshing was not the main
   problem: **streaming** was the largest cost in 210 of 216 over-budget frames
   while moving (worst 33.7 ms) and in a collapse session (worst 112 ms), for two
   reasons:

   - it saved **every** chunk it unloaded, untouched generated terrain
     included, and flushed the region after each - on the game thread. Now only
     dirty chunks are saved, by a worker (`PendingSaves`); a load of a chunk
     whose save is outstanding is deferred (async) or waits (sync), shutdown and
     region closes wait for all of them, and a clean chunk simply regenerates;
   - it rescanned ~1,000 wanted coordinates and copied every loaded chunk key
     each frame. Load and unload queues are now rebuilt only when a source
     crosses a chunk (or once a second), from a distance-sorted offset list
     computed once per radius.

   Measured after, rendered, one session each: a 19-zombie horde 0 of 1,905
   frames over (worst 1.6 ms); streaming 3 of 1,473 (worst 2.2 ms); an 81-block
   concrete collapse 4 of 159 (worst 2.6 ms). CI gate 12 now runs all three and
   fails above 0.5% over budget or on any frame over 5 ms.

   After the meshing changes alone: the same teleport test **3 of 1,657** frames over (all in one burst);
   moving faster than a sprint **0 of 1,188**, worst 1.7 ms; initial load 0 of
   399, worst 1.95 ms. CI gate 12 now runs that streaming test rendered and
   fails above two over-budget frames. Remaining single-operation risk: one very
   busy chunk's section creation (~1.1 ms measured) plus a registration in the
   same frame.
4. **No distance field updates for edited chunks**, no mesh decals for wear or
   damage, no Nanite (there are no authored props yet), no volumetric weather.
5. **No Unreal Insights trace captured.** Named CPU scopes and stat groups are
   in place (`TRACE_CPUPROFILER_EVENT_SCOPE`, `STATGROUP_MadFallMesher`), so the
   trace will be readable when there is a scene worth profiling.

### Known gaps after Phase 1

1. **No world generator.** A chunk with nothing on disk loads as empty air
   rather than terrain. That is Phase 3. `mad.debug.fillterrain` writes a
   sine-based heightfield into a loaded chunk as a mesher test fixture — it is
   explicitly not a generator, and it stays useful for reproducing meshing bugs
   in isolation after Phase 3 exists.
2. **No write-ahead journal, no region compaction, no backups.** See
   [Durability](#durability).
3. **The 2 ms game-thread budget is unmeasured.** Edits are O(1) plus at most
   one 32 KiB side-array allocation, and chunk loads have an async path, but no
   Unreal Insights trace has been captured. That measurement belongs with
   Phase 2, when there is meshing work to profile alongside it.
4. **`extends` is JSON-only.** A data asset has no way to distinguish an unset
   field from a defaulted one, so the loader rejects `extends` on assets with an
   explanatory error rather than guessing.
5. ~~**Patch files are specified but not implemented.**~~ Resolved in Phase 5:
   `madfall.patch/1` applies to blocks, biomes, items, recipes, loot and zombies.
6. **No Linux cross-compile toolchain installed.** Only relevant once the
   server target is revived in Phase 6.
7. `Scripts/CI.ps1` is Windows/PowerShell only.
8. The development machine is an RTX 3080, below the RTX 4070 target spec. The
   Phase 2 render-thread budget cannot be validated here.

---

## Environment

Verified on the development machine, 2026-09-12:

| Component | Version | Path |
|---|---|---|
| Unreal Engine | 5.8.2 (CL 56702186, `++UE5+Release-5.8`) | `C:\Program Files\Epic Games\UE_5.8` |
| Engine distribution | **Launcher (installed)** — `Engine/Build/InstalledBuild.txt` present | |
| Visual Studio | 2022 Community 17.14.40 | |
| MSVC toolchain | 14.44.35207 | |
| Windows SDK | 10.0.22621.0 | |
| Git / Git LFS | 2.54.0 / 3.7.1 | |
| Target GPU (dev box) | RTX 3080 | below the RTX 4070 target spec — Phase 2 budgets must be validated on 4070-class hardware, not here |

Build settings are pinned, not `Latest`:

```csharp
DefaultBuildSettings = BuildSettingsVersion.V7;          // 5.8 defaults
IncludeOrderVersion  = EngineIncludeOrderVersion.Unreal5_8;
CppStandard          = CppStandardVersion.Cpp20;
```

An engine upgrade should be a deliberate commit that changes these lines, not a
silent behaviour change when someone installs a newer engine.

---

## (a) Module dependency graph

```
                        +-----------------------+
                        |    MadFallModAPI      |  Runtime | PostConfigInit
                        | STABLE PUBLIC SURFACE |  Core, CoreUObject only
                        +-----------+-----------+
                                    |
                        +-----------v-----------+
                        |     MadFallCore       |  Runtime | PreDefault
                        | volume/chunks/palette |
                        | serialization         |
                        | block registry        |
                        | mod discovery + merge |
                        +-----+-----------+-----+
                              |           |
              +---------------v--+     +--v--------------------+
              |  MadFallMesher   |     |                       |
              | DC/Surface Nets  |     |                       |
              | greedy cubic     |     |                       |
              | LOD, Chaos cook  |     |                       |
              +---------+--------+     |                       |
                        |              |                       |
                   +----v--------------v-+                     |
                   |  MadFallGameplay    |  Runtime | Default   |
                   |  GAS, survival, SI  |<---------------------+
                   |  crafting, AI, loot |  PRIMARY GAME MODULE
                   +----------+----------+
                              |
                   +----------v----------+
                   |   MadFallEditor     |  Editor | Default
                   |  prefab/POI tools   |
                   |  block-def linter   |
                   +---------------------+

Phase 5 addition:
     MadFallModAPI <-- MadFallScriptRuntime <-- MadFallGameplay
     (Lua 5.4 sandbox; depends on Core and ModAPI only - the sandbox surface
     IS IMadScriptHost in ModAPI, which Gameplay implements.)
```

### Declared dependencies

| Module | Type | LoadingPhase | Public | Private |
|---|---|---|---|---|
| `MadFallModAPI` | Runtime | `PostConfigInit` | `Core`, `CoreUObject` | `Json`, `JsonUtilities`, `PakFile`, `Projects` |
| `MadFallCore` | Runtime | `PreDefault` | `Core`, `CoreUObject`, `Engine`, `MadFallModAPI` | `NetCore`, `AssetRegistry`, `DeveloperSettings`, `Json`, `JsonUtilities` |
| `MadFallMesher` | Runtime | `Default` | `Core`, `CoreUObject`, `Engine`, `MadFallCore` | `RenderCore`, `RHI`, `Chaos`, `GeometryCore`, `MeshDescription`, `StaticMeshDescription` |
| `MadFallScriptRuntime` | Runtime | `Default` | `Core`, `MadFallModAPI` | (Lua sources from `Source/ThirdParty/Lua`) |
| `MadFallGameplay` | Runtime | `Default` | `Core`, `CoreUObject`, `Engine`, `GameplayAbilities`, `GameplayTags`, `GameplayTasks`, `MadFallCore`, `MadFallMesher`, `MadFallScriptRuntime` | `AIModule`, `NavigationSystem`, `EnhancedInput`, `InputCore`, `Json`, `NetCore`, `Slate`, `SlateCore` |
| `MadFallEditor` | Editor | `Default` | `Core`, `CoreUObject`, `Engine`, `UnrealEd`, `MadFallCore`, `MadFallMesher`, `MadFallGameplay` | `Slate`, `SlateCore`, `EditorSubsystem`, `EditorStyle`, `PropertyEditor`, `ToolMenus`, `Projects` |

### Decisions

**1. ModAPI is at the bottom of the graph, not the top.**

The instinct is to put a mod API on top of the game. That is backwards, and it
is how mod APIs rot. Sitting at the bottom with no dependency on Core or
Gameplay buys three things:

- It *physically cannot* leak internal types. A refactor of `FMadChunkStorage`
  cannot reach a modder, because ModAPI cannot see `FMadChunkStorage`. The
  stable surface is enforced by the build graph, not by discipline.
- It can load at `PostConfigInit` — the only phase early enough to mount Tier-2
  mod `.pak` files **before** the asset registry scans. A module that depended
  on `Engine` could not load that early, and mod assets would be invisible to
  every soft-path reference in a mod definition.
- In Phase 5 the Lua sandbox allow-list is "everything in this module's `Public`
  folder, nothing else", which a script can verify.

Accepted cost: ModAPI owns POD mirror types (`FMadVoxel`, and later
`FMadBlockDefView`, `FMadWorldEditRequest`) rather than reusing Core's.
Duplication is the price of a surface that can hold still across versions.

`MadFallModAPI.Build.cs` must never gain an `Engine` dependency. Code that needs
`Engine` belongs in `MadFallCore` or higher.

**2. Tier-2 pak mounting lives in ModAPI; definition parsing lives in Core.**

Mounting is a filesystem concern with no game knowledge and must run at
`PostConfigInit`. Parsing needs `UMadBlockDefinition`, which needs Core. The
seam is at exactly that line. Mounting happens from
`FMadFallModAPIModule::StartupModule()` rather than from a `UEngineSubsystem`,
because engine subsystems do not exist yet at that phase.

**3. Gameplay depends on Mesher.**

Structural collapse spawns falling-debris actors whose meshes are generated from
a voxel sub-volume. Routing that through an interface would be purity for its
own sake — the two modules ship and version together.

**4. Nothing depends on MadFallEditor, and it is absent from the server target.**

Building `MadFallServer` in CI on every commit turns "someone added an
editor-only include to runtime code" into a compile error the same day, instead
of a discovery three months before ship.

**5. `MadFallGameplay` is the primary game module.**

`IMPLEMENT_PRIMARY_GAME_MODULE` lives there because it is the module a packaged
build cannot run without. There is no module named `MadFall`.

**Rejected: a single monolithic `MadFallRuntime` module.** Faster to compile at
first, but it makes the mod surface undefinable and the server target
unpruneable.

---

## (b) Voxel struct layout

Implemented in
[`Source/MadFallModAPI/Public/MadFallVoxelTypes.h`](../Source/MadFallModAPI/Public/MadFallVoxelTypes.h).
Covered by `MadFall.Core.Voxel.Layout`.

### `FMadVoxel` — 6 bytes, packed, alignment 1

```cpp
#pragma pack(push, 1)
struct FMadVoxel
{
    uint16 BlockTypeID;   // bytes 0-1
    uint8  Density;       // byte  2
    uint8  Damage;        // byte  3
    uint8  Rotation;      // byte  4
    uint8  Flags;         // byte  5
};
#pragma pack(pop)
```

| Offset | Size | Field | Encoding |
|---|---|---|---|
| 0 | 2 | `BlockTypeID` | `0` = `madfall:air` (reserved, never remappable). `1`–`65534` = registry runtime IDs. `65535` = `madfall:unresolved`. Runtime IDs are **session-local**; the save file stores namespaced strings. |
| 2 | 1 | `Density` | Isosurface sample. `0` fully outside, `255` fully inside, **`128` the surface crossing**. Solid test is `Density >= 128`. Unsigned-with-bias rather than `int8` so `memset(0)` on a fresh chunk means "empty air" and chunk allocation is a single zero-fill. |
| 3 | 1 | `Damage` | `0` intact → `255` destroyed-pending. Stage thresholds come from the block definition, so a mod can have 2 stages or 8 with no format change. |
| 4 | 1 | `Rotation` | Bits 0–4: orientation `0–23` (6 up-faces × 4 spins); `24–31` reserved and read back as `0`. Bits 5–7: shape variant `0–7`, so one `BlockTypeID` covers a modular family (straight / corner / tee / ramp). |
| 5 | 1 | `Flags` | `EMadVoxelFlags` bitmask, below. |

### `EMadVoxelFlags`

| Bit | Name | Meaning |
|---|---|---|
| 0 | `Cubic` | Player-placed construction → greedy cubic mesher and snapped build grid. Cleared → Dual Contouring. **This bit is the terrain/construction seam.** |
| 1 | `Anchor` | Immovable support source. Terminates structural flood-fill. |
| 2 | `Liquid` | Participates in the fluid tick. |
| 3 | `HasBlockEntity` | Extra state in the chunk side table. Avoids a hash lookup per voxel while meshing. |
| 4 | `PlayerModified` | Differs from worldgen output. A chunk with zero of these and a matching seed + worldgen version is never written to disk at all. |
| 5 | `SupportDirty` | Queued for the SI solver. **Transient** — masked off by `MadFall::TransientVoxelFlagMask` before serialization. |
| 6–7 | reserved | Written `0`, ignored on read. |

Adding a runtime-only flag without adding it to `TransientVoxelFlagMask` leaks
dirty state into save files. The layout test asserts the mask strips
`SupportDirty` and preserves `PlayerModified`.

### Why 6 bytes and not the 8 the budget allows

`FMadVoxel[4]` is 24 bytes, so a 64-byte cache line holds 10.67 voxels instead
of 8 — about 33% fewer cache lines on the mesher's inner loop, which is the
hottest loop in the engine. The two spare bytes would be speculative.

### Chunk storage is SoA + palette, not `TArray<FMadVoxel>`

A dense `FMadVoxel[32768]` is 192 KiB per chunk. At a 12-chunk horizontal radius
× 16 vertical that is 625 chunks ≈ **117 MB of voxels alone**, most of it
storing 32768 identical sky voxels.

```cpp
// MadFallCore, Phase 1. Converted to/from FMadVoxel at the API boundary.
struct FMadChunkStorage
{
    TArray<FMadBlockPaletteEntry> Palette;   // 8 B: {uint16 RuntimeId; uint16 Pad; uint32 RefCount;}
    FMadBitPackedIndexArray       Indices;   // 1/2/4/8/16 bpv, tier from Palette.Num()

    TUniquePtr<uint8[]>           Density;   // 32 KiB, null for uniform/pure-cubic chunks
    TUniquePtr<uint8[]>           Rotation;  // 32 KiB, null until a rotatable block is placed
    TUniquePtr<uint8[]>           Flags;     // 32 KiB, null until a non-zero flag
    FMadSparseDamageMap           Damage;    // sparse until >4096 damaged voxels

    FRWLock                       Lock;
};
```

Linear index is **`idx = x + 32*(y + 32*z)`**, X fastest
(`MadFall::VoxelIndex`). Mesher, serializer, SI solver and net delta encoder all
depend on it; a mismatch is silent corruption rather than a crash, so it lives
in exactly one place and is tested.

| Chunk kind | Palette | Indices | Density | Rot | Flags | Damage | Total |
|---|---|---|---|---|---|---|---|
| Uniform air (sky) | 8 B | 0 | — | — | — | — | **~64 B** |
| Solid stone (deep) | 8 B | 0 | — | — | — | — | **~64 B** |
| Terrain surface (6 types, 4 bpv) | 48 B | 16 KiB | 32 KiB | — | — | — | **48 KiB** |
| Heavy player base (40 types, 8 bpv) | 320 B | 32 KiB | 32 KiB | 32 KiB | 32 KiB | ~2 KiB | **130 KiB** |
| Dense `FMadVoxel[]` (rejected) | | | | | | | 192 KiB |

Realistic 625-chunk working set ≈ **16 MB**, versus 117 MB for the naive layout.
That headroom is what pays for Nanite props and virtual textures inside 16 GB.

Palette bpv tiers: `<=2` → 1 bpv, `<=4` → 2, `<=16` → 4, `<=256` → 8, else 16.
Re-tiering is an O(32768) repack on a worker thread. `RefCount` reaching zero
frees a slot; compaction runs on the save path, never on the edit path.

**Threading contract.** Worker threads take a read lock and never mutate. An
edit produces a new `FMadChunkStorage` (copy-on-write of only the touched
sub-arrays) which the game thread pointer-swaps under the write lock. The game
thread's only voxel work is the swap and a mesh component assignment — well
inside the 2 ms rule, and it never waits on a mesher job.

### World constants

| Constant | Value | Note |
|---|---|---|
| `ChunkSize` | 32 | |
| `ChunkVoxelCount` | 32768 | |
| `VoxelSizeUU` | 100.0 | 1 m |
| `WorldMinZ` / `WorldMaxZ` | −128 / 383 | inclusive |
| `WorldChunkLayers` | 16 | 512 voxels ÷ 32; `static_assert`ed |
| `WorldMinChunkZ` / `WorldMaxChunkZ` | −4 / 11 | |
| `RegionChunksXY` | 16 | 512 m × 512 m footprint |
| `RegionChunkSlots` | 4096 | `static_assert`ed |

The vertical range is load-bearing: it is what makes a region file a clean
16×16×16 index with no ragged edge. Changing the build ceiling resizes the
region index table and bumps the save format version.

### `FMadVoxelState` — the Blueprint view

`FMadVoxel` is native-only. Reflection over a `pack(1)` struct is a trap, and
designers should never see a bit-packed rotation byte. Blueprint sees
`FMadVoxelState` (`USTRUCT(BlueprintType)`) with `BlockId` as an `FName`,
orientation and shape variant as separate `int32`s, and a `bSolid` convenience
mirror. Conversion happens at the Blueprint boundary.

### Type naming

`FVoxel` is taken by several published UE voxel plugins, and UE has a single
flat global type namespace across the engine and every plugin; reflection does
not support `USTRUCT` inside a C++ namespace, so prefixing is the only escape.
Hence `FMadVoxel`, `FMadChunkStorage`, `FMadBlockPaletteEntry`.

---

## (c) Block definition schema

Schema id: **`madfall.block/1`**. Phase 1 implements the loader; this is the
contract it will implement.

The same shape is used whether a definition arrives as a `UMadBlockDefinition`
data asset (editor-authored, cooked) or as JSON in
`Mods/<id>/definitions/blocks/*.json`. A Tier-1 mod block and a first-party
block are indistinguishable to the registry. That equivalence is the whole point
of the mod-first pillar: if the shipped content can do something a mod cannot,
the mod API is a lie.

```jsonc
{
  "schema": "madfall.block/1",
  "id": "madfall:rebar_concrete",        // REQUIRED, "<namespace>:<name>", [a-z0-9_]+
  "extends": "madfall:base_concrete",    // optional; resolved depth-first before mod merges
  "display_name": "@blocks.rebar_concrete", // "@key" = localization lookup; bare string = literal

  "shape": {
    "kind": "cubic",                     // "cubic" | "isosurface" | "model"
    "rotation_mode": "full_24",          // "none" | "axis" | "facing_4" | "full_24"
    "variants": ["full", "half", "ramp", "corner_in", "corner_out"], // -> Rotation bits 5-7, max 8
    "occludes_neighbors": true,
    "collision": "mesh"                  // "box" | "mesh" | "none"
  },

  "material": {
    "class": "madfall:concrete",         // material class def: sounds, particles, decal set
    "mass_kg": 2400.0,                   // per 1 m^3 voxel; feeds debris physics and SI load
    "hardness": 850.0,                   // hit points at full health, pre-resistance
    "resistances": {                     // damage-type multiplier; missing key = 1.0
      "madfall:blunt": 0.35,
      "madfall:pierce": 0.20,
      "madfall:explosive": 1.40,
      "madfall:fire": 0.05
    },
    "harvest": { "tool_tags": ["tool.pickaxe", "tool.jackhammer"], "tier": 3 }
  },

  "structure": {
    "support_strength": 9000.0,          // kg this block can carry through itself
    "max_horizontal_span": 9,            // unsupported cantilever in voxels before failure
    "is_anchor": false,
    "debris_on_collapse": "madfall:concrete_rubble"
  },

  "damage_states": [                     // ordered ascending; thresholds on the Damage byte
    { "at": 0,   "mesh": "/MadFall/Blocks/Concrete/SM_Rebar_Intact",
                 "support_multiplier": 1.00 },
    { "at": 96,  "mesh": "/MadFall/Blocks/Concrete/SM_Rebar_Cracked",
                 "support_multiplier": 0.60, "decal_set": "madfall:cracks_light",
                 "sound": "madfall:concrete_crack" },
    { "at": 176, "mesh": "/MadFall/Blocks/Concrete/SM_Rebar_Broken",
                 "support_multiplier": 0.25, "decal_set": "madfall:cracks_heavy" },
    { "at": 255, "downgrade_to": "madfall:rebar_concrete_frame" }   // chain, not deletion
  ],

  "render": {
    "mesh": "/MadFall/Blocks/Concrete/SM_Rebar_Intact",  // soft path; isosurface blocks use "material"
    "material": "/MadFall/Materials/MI_Concrete_Triplanar",
    "material_slot_overrides": { "1": "/MadFall/Materials/MI_Rebar" },
    "nanite": true,                      // ignored for isosurface blocks - see the Nanite boundary note
    "cast_shadow": true
  },

  "sounds": {
    "place":   "/MadFall/Audio/S_Concrete_Place",
    "hit":     "/MadFall/Audio/S_Concrete_Hit",
    "destroy": "/MadFall/Audio/S_Concrete_Break",
    "step":    "madfall:material_default"  // inherit from material class
  },

  "drops": {
    "table": "madfall:loot/rebar_concrete",
    "requires_tool_tags": ["tool.pickaxe"],
    "on_collapse": "madfall:loot/rubble"   // different table when destroyed by collapse
  },

  "placement": {
    "craft_from": "madfall:item/rebar_concrete_block",
    "requires_support": true,
    "snap": "grid",                        // "grid" | "free"
    "upgrade_from": ["madfall:concrete_frame"],
    "repair_with": { "item": "madfall:item/concrete_mix", "hp_per_unit": 200.0 }
  },

  "flags": {
    "transparent": false, "liquid": false, "climbable": false,
    "flammable": false, "conductive": true, "block_entity": null
  },

  "tags": ["block.building", "block.concrete", "block.tier3"],  // GameplayTags, queryable by mods
  "mod_data": {}                           // free-form; Tier-3 scripts read/write via ModAPI
}
```

### Contract

| Rule | Detail |
|---|---|
| Required fields | `schema`, `id`. Everything else has a documented default or is inherited via `extends`. |
| Namespacing | `<namespace>:<name>`. The namespace **must** equal the owning `mod.json` `id`; a mod cannot define into another mod's namespace on first definition. |
| Collision | Two mods defining the same `id`: later-in-load-order wins, and `LogMadFallRegistry: Warning` names both mods and both file paths. Never silent. |
| Overriding | Editing another mod's block is a *patch*, not a redefinition: `definitions/patches/*.json` with `{"target": "...", "ops": [{"op": "set", "path": "/material/hardness", "value": 1200}]}`. `set` / `append` / `remove` on JSON-Pointer paths, applied after all base definitions load. Two mods patching different paths of the same block both succeed. The patch format exists from Phase 1 even though the merge engine is Phase 5 — bolted on later, it never gets designed properly. |
| Assets | All asset references are soft paths, resolved lazily. An unresolvable path is a load-time error **for that definition only**; the block registers with the `madfall:missing_asset` visual and the world still loads. |
| Units | Mass in kg per 1 m³ voxel, hardness in hit points, support in kg. Validated by the `MadFallEditor` definition linter. |
| Validation | Every field validated at load with a typed error naming file, JSON pointer, expected type and actual value. No silent coercion. |
| Versioning | The `schema` string gates migration. `madfall.block/1` → `/2` runs a registered upgrader; an unknown future version fails that definition loudly rather than guessing. |

---

## (d) Chunk save file format

Phase 1 implements the writer and reader. This is the contract.

```
<WorldDir>/
  world.mfmeta            // seed, worldgen version, game version, mod manifest snapshot + hash
  regions/
    r.0.0.mfr             // region = 16x16 chunk columns x the full vertical range
    r.0.0.mfr.index       // only during a flush: the new header + index, replayed after a crash
    r.0.-1.mfr
  players/<uuid>.mfplayer
  backups/<timestamp>/
```

A region covers 16×16 chunks in XY (512 m × 512 m) and **all 16 vertical chunk
layers** (world Z −128…383 → chunk Z −4…11): 4096 chunk slots, one file per XY
region. A vertical column is always in one file, one mmap, one seek.

*Rejected: a separate file per vertical slab.* Voxel edits and the structural
solver are column-coherent, so cross-file column reads would double mmap churn
for no benefit.

### Region header — 4096 bytes, fixed, memory-mapped

Little-endian throughout.

| Off | Size | Field |
|---|---|---|
| 0 | 4 | magic `'MFRG'` |
| 4 | 2 | `format_version` u16 = 1 |
| 6 | 2 | `header_bytes` u16 = 4096 |
| 8 | 4 | `region_x` i32 |
| 12 | 4 | `region_y` i32 |
| 16 | 8 | `world_seed` u64 |
| 24 | 8 | `worldgen_version` u64 (hash of generator params; mismatch ⇒ ungenerated chunks regenerate) |
| 32 | 8 | `last_write_unix_ms` i64 |
| 40 | 4 | `sector_bytes` u32 = 4096 |
| 44 | 4 | `index_offset` u32 = 4096 |
| 48 | 4 | `index_entry_count` u32 = 4096 |
| 52 | 4 | `strtab_offset_sectors` u32 |
| 56 | 4 | `strtab_bytes` u32 |
| 60 | 4 | `strtab_crc32` u32 |
| 64 | 4 | `used_sectors` u32 |
| 68 | 4 | `free_sectors` u32 (compact above 30% fragmentation) |
| 72 | 4 | `flags` u32 — bit0 `UNCLEAN_SHUTDOWN` (bit1 `COMPACTION_PENDING` is unused: compaction happens on close) |
| 76 | 64 | `game_version` char[64], NUL-padded |
| 140 | 4 | `header_crc32` u32 — CRC32 of the 68 KiB header + index with this field zero; 0 in older files |
| 144 | 3952 | reserved, zero-filled |

### Chunk index table — 4096 × 16 B = 64 KiB at offset 4096

Entry index: `local_x + 16*(local_y + 16*(chunk_z + 4))`.

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `offset_sectors` u32 — sector offset of the payload. When `UNIFORM` is set, reinterpreted as the block id's **string table index**. |
| 4 | 4 | `payload_bytes` u32 (prefix + compressed body). When `UNIFORM`, reinterpreted as packed `{ density, rotation, flags, 0 }`. |
| 8 | 4 | `payload_crc32` u32 (over the bytes as written). Zero when `UNIFORM`. |
| 12 | 1 | `compression` u8 — 0 none, 1 LZ4 |
| 13 | 1 | `chunk_version` u8 |
| 14 | 2 | `flags` u16 — bit0 `PRESENT`, bit1 `UNIFORM`, bit2 `PLAYER_MODIFIED`, bit3 `HAS_BLOCK_ENTITIES`, bit4 `SI_CACHE_VALID` |

Header + index = 68 KiB = 17 sectors; payloads start at sector 17. **An all-air
or all-stone chunk costs 16 bytes of index and nothing else** — which is most of
a world.

Two corrections to the original design, made while implementing:

- **A `PRESENT` bit was added.** The original plan used `offset_sectors == 0` to
  mean "never generated", which stops working the moment that field is
  reinterpreted as a string table index — index 0 is a perfectly legal id.
- **A uniform chunk's defaults live in `payload_bytes`.** The original plan had
  nowhere to put a uniform chunk's density, so a uniform stone chunk (density
  255) could not have been expressed without a payload, and 4096 of those would
  have cost 16 MB of sectors per region for four bytes of information each.

The free-sector allocator is not stored. It is rebuilt at open by scanning 4096
index entries (~30 µs). Nothing to corrupt, nothing to keep in sync.

### Chunk payload

A 24-byte **uncompressed** prefix (so LZ4 output sizing needs no allocation
guess), then one LZ4 blob of TLV sections:

```
u32 magic 'MFCH' | u8 version | u8 compression | u16 reserved
u32 uncompressed_bytes | i64 last_modified_ms | u32 reserved
---- LZ4 blob: repeated sections ----
u8 section_id | u32 section_bytes | <body>
```

| ID | Section | Encoding | Phase |
|---|---|---|---|
| 1 | `PALETTE` | u16 count, then count × u32 index into the region string table. **The disk stores `"mymod:rebar_concrete"`, never a runtime uint16.** | 1 |
| 2 | `BLOCK_INDICES` | u32 pair count, then `{u16 run_length, u16 palette_slot}` pairs covering all 32768 voxels, X-fastest. Absent ⇒ every voxel uses palette slot 0. | 1 |
| 3 | `DENSITY` | RLE `{u16 run, u8 value}` over 32768 bytes. Absent ⇒ every voxel uses the `DEFAULTS` density. | 1 |
| 4 | `DAMAGE` | Sparse: u32 count, then `{u16 voxel_index, u8 damage}`. | 1 |
| 5 | `ROTATION` | Same RLE scheme as `DENSITY`. | 1 |
| 6 | `FLAGS` | Same RLE scheme, transient bits masked to 0 *before* encoding so the runs coalesce. | 1 |
| 7 | `BLOCK_ENTITIES` | `{u16 voxel_index, u32 type_strtab_idx, u32 blob_bytes, u8 blob[]}`. An unknown entity type keeps its blob **verbatim**. | 4 |
| 8 | `SI_CACHE` | Cached support values. Purely derived — dropped on version mismatch, never a load failure. | 4 |
| 9 | `MOD_PAYLOAD` | `{u32 mod_strtab_idx, u32 bytes, u8 data[]}` repeated. Payloads for absent mods are **preserved verbatim**. | 5 |
| 10 | `DEFAULTS` | `{u8 density, u8 rotation, u8 flags}` — the value every voxel without a side array carries. | 1 |

Two corrections to the original design, again made while implementing:

- **`BLOCK_INDICES` does not store `bits_per_voxel`.** It is an in-memory
  packing detail derived from the palette size. Writing it would let a file
  disagree with the palette it ships with, which is a corruption no reader could
  adjudicate.
- **A `DEFAULTS` section was added.** Lazily allocated side arrays mean a chunk
  can legitimately have no density array at all, and the value those voxels
  carry has to survive the round trip.

Unknown section IDs are skipped with a load warning and dropped on the next
rewrite; a `chunk_version` newer than the reader is refused outright rather than
partially parsed. Each section handler seeks to its declared end regardless of
how much it read, so one short handler cannot misalign every section after it.

**The output is deterministic.** The damage map is written in sorted voxel
order rather than `TMap` iteration order, so identical chunk state produces
byte-identical files. Without that, "did this actually change?" is unanswerable.

### Region string table

At `strtab_offset_sectors`: `u32 count`, then `count × {u16 byte_len, utf8 bytes}` —
every namespaced ID any chunk in the region references. Append-only during a
session; compacted during region compaction. CRC'd in the header: a bad string
table fails the region loudly rather than mapping blocks to the wrong IDs.

### Graceful mod removal

On load, each palette string resolves against the registry. On a miss:

1. The voxel keeps `BlockTypeID = 65535` (`madfall:unresolved`) in memory.
2. The original string stays in the chunk palette and in an `UnresolvedMap`.
3. The block renders as an inert grey placeholder showing the original ID on
   inspect, has infinite support, and cannot be interacted with.
4. **On the next save the original string is written back out unchanged.**

Reinstalling the mod restores every block exactly. The failure this avoids —
remapping unknown IDs to air on load, which silently deletes a player's base the
first time they toggle a mod off — is the single most common voxel-game save
corruption bug, and it is a *format* decision, not a runtime one.

### Durability

**Implemented (Phase 1):**

- **Copy-on-write payloads.** A chunk's new payload always goes to freshly
  allocated sectors and the index entry is updated only afterwards. The live
  copy is never overwritten in place, so a crash mid-write loses the new version
  of one chunk and never damages the old one.
- **Index written last.** It is the pointer to everything else, so it must never
  name a sector whose contents are not yet on disk.
- **The string table relocates rather than growing in place**, for the same
  reason: the previous, consistent table stays readable until the header points
  elsewhere.
- **CRC32 on every payload**, verified on read, with the failure naming the
  slot, both CRCs, the byte count and the sector — so a damaged world can be
  quarantined chunk by chunk instead of losing a whole region.
- **Version gates.** A region format or chunk payload version newer than the
  reader is refused, not best-effort parsed.
- **Seed guard.** Opening a region whose stored seed disagrees with the world's
  fails, naming both seeds. Silently mixing chunks from two generators produces
  seams no player can explain.

**Implemented (Phase 7 hardening):**

- **Deferred sector frees - a real bug, fixed.** A rewrite used to free the old
  payload's sectors at once. `SaveAll` writes every dirty chunk before one
  flush, so the next chunk of the same save could take those sectors, and a
  crash before the index flush left the on-disk index naming another chunk's
  bytes: the old version destroyed, the new one never indexed. Freed sectors now
  wait (`PendingFrees`) until an index that no longer names them is on disk.
- **The index is double-written, per region, not in a world journal.** Flush
  writes the complete new header and index (68 KiB, with its own CRC) to
  `r.x.y.mfr.index`, then in place, then deletes the sidecar. Open replays a
  valid sidecar - the in-place write may have been torn - and discards an
  invalid one, which means the in-place write never began. Per region rather
  than one `world.journal` because regions flush independently, from the game
  thread and from streaming's save workers, and a shared journal would need its
  own locking and replay ordering for no gain: the unit of consistency is
  already the region.
- **CRCs on the header and index** (in the formerly reserved u32 at offset 140,
  computed with that field zero) **and on the string table**, whose header field
  was always written as zero before. Files from before either CRC carry zero and
  are not checked. A header/index CRC failure with no journal fails the region
  loudly, naming the backup to restore, rather than loading a torn index.
- **`UNCLEAN_SHUTDOWN`** is set by the first write of a session and cleared by
  `Close`; opening a region with it set logs a warning (and `mad.region` shows
  it).
- **Disk flushes where they cannot stall play.** Off the game thread and on
  `Close`, each step reaches the disk (`FlushFileBuffers`) before the next,
  which makes the order hold across power loss. Every mid-play world write is
  off the game thread: unloads, and the autosave (below). The synchronous
  `SaveAll` (`mad.save`, leaving a world) skips the per-step disk flush - a
  5-20 ms stall per region - and relies on the OS keeping write order, which
  holds for a crash of the game; leaving a world then closes every region,
  which does flush.
- **FIXED: the autosave saves the world, safely.** `mad.save.AutosaveMinutes`
  said "voxels and gameplay" but wrote only gameplay: blocks placed in chunks
  that never unloaded - the base the survivor works in - were lost to a crash
  while the materials spent on them were saved. The autosave now calls
  `UMadVoxelWorldSubsystem::SaveAllAsync`: the game thread copies each dirty
  chunk's packed storage (0.01 ms for one chunk; a memcpy per chunk) and marks
  it clean, and a worker compacts the copies, writes them and disk-flushes each
  region in order. The copy is taken in the frame the gameplay state is saved,
  so the two agree. An edit after the copy dirties the chunk for the next save;
  a failed write hands the chunk back to the game thread to be marked dirty
  again; a load of a chunk with a write outstanding waits for it, and a later
  unload save of the same chunk is chained after it (not waited for, which
  would put the autosave's write on that frame), so older bytes never land
  over newer. CI gate: an autosave, an edit, then `mad.world.crash`
  (TerminateProcess - no shutdown save, no region close); the next run reads
  the autosaved edit back and not the later one.
- **Compaction.** `Close` rewrites a region with at least 64 free sectors that
  are 30% or more of the file: payloads and string table packed into
  `r.x.y.mfr.compact`, disk-flushed, then renamed over the original, so a crash
  leaves one complete file or the other (a stale `.compact` is deleted on open).
  First-fit reuse keeps ordinary rewrites from fragmenting a region; what leaves
  holes is chunks that stop needing a payload.

Tests: `MadFall.Core.Serialization.RegionDurability` crashes a save before the
journal, after it and half way through the in-place index, plants a torn
journal, damages an index with no journal, and compacts a region, checking
every chunk each time.

**Not yet implemented:**

- **Backups.** A plain copy of world.json, gameplay.json and regions/ into
  `<world>/backups/<UTC timestamp>/`, made as the world opens for play and
  before any region file is opened - the one moment the folder is guaranteed
  quiescent, so no journal is needed for a consistent snapshot. Skipped when no
  file changed since the newest backup; the newest three are kept
  (`MadFall::WorldBackup`). A background copy would race the first saves and a
  copy on quit would miss a crash, the case backups are for; the cost is copy
  time on the loading path, logged as "World backup ... made in N ms". The
  world list's Restore button (`mad.world.restore`) puts back the newest
  backup - "undo last session" - after backing the current state up, so a
  restore can itself be undone; it refuses the world being played.
  Tests: `MadFall.Session.Backups`. Not LZ4-HC recompressed: region payloads
  are already LZ4, and the copy stays a folder a player can open.

### Compression

**LZ4 level 1 live, LZ4-HC for backups.** Zstd-3 is ~25% smaller but ~4× slower
to decompress, and chunk decompression sits directly in the streaming path where
a stall is visible pop-in during flight. Region files run ~1.5–4 MB for fully
explored terrain, which is not a budget worth spending frames on. `compression`
is a per-chunk byte, so switching later needs no format bump.

---

## Build and CI

```powershell
.\Scripts\Build.ps1 -Target MadFallEditor       # one target
.\Scripts\CI.ps1                                # the gate
.\Scripts\CI.ps1 -RequireServer                 # also enforce the server build (Phase 6)
```

Engine root resolution: `-EngineRoot` → `$env:MADFALL_UE_ROOT` →
`C:\Program Files\Epic Games\UE_5.8`.

CI gates, in order — any one failing fails the run:

1. `MadFallEditor` builds (required to run automation tests at all)
2. `MadFall` (client) builds
3. `MadFallServer` builds — **skipped by default**, opt in with `-RequireServer`;
   see [Dedicated server](#dedicated-server)
4. Zero compiler warnings
5. The test map exists or can be regenerated
6. Every `MadFall.*` automation test passes, with `notRun > 0` and
   `succeeded == 0` both treated as failures

**On warnings.** Every module sets `bWarningsAsErrors = true` in its
`Build.cs`, so a C++ warning is already a hard compile error. Gate 4 exists to
catch what that flag cannot reach: C# warnings from the `Target.cs` / `Build.cs`
rules assemblies and UHT warnings, which otherwise scroll past a green build
forever. It matches compiler-shaped diagnostics (`file(line,col): warning X:`)
rather than the bare word, so UBT progress chatter does not trip it.

`.github/workflows/ci.yml` targets a self-hosted Windows runner labelled
`[self-hosted, windows, unreal]`. Unreal Engine is ~150 GB installed and cannot
run on GitHub-hosted runners. Setting the repository variable `REQUIRE_SERVER`
to `true` promotes the server build to a hard gate — which also means the runner
then needs a source engine build.

### Console commands

The Phase 1 interface to the voxel world. Coordinates are world **voxel**
coordinates unless a command says "chunk".

| Command | Purpose |
|---|---|
| `mad.blocks` | Every registered definition, its runtime id, mass, hardness and support |
| `mad.world.info` | World directory, loaded chunks, voxel memory, unsaved chunks |
| `mad.world.save` | Save every dirty chunk and flush every open region |
| `mad.chunk.load <cx> <cy> <cz>` | Load from disk, or create empty |
| `mad.chunk.unload <cx> <cy> <cz> [nosave]` | Save and drop |
| `mad.chunk.info <cx> <cy> <cz>` | Palette, bit width, damage count, memory, dirty state |
| `mad.chunk.fill <cx> <cy> <cz> <blockId> [density]` | Fill a loaded chunk uniformly |
| `mad.voxel.get <x> <y> <z>` | Read one voxel |
| `mad.voxel.set <x> <y> <z> <blockId> [density] [orientation] [variant]` | Write one voxel |
| `mad.region.info <rx> <ry>` | Chunk count, uniform count, sector use, string table size |

A worked session (this is the Phase 1 acceptance check, run headlessly in CI):

```
mad.chunk.load 0 0 0
mad.chunk.fill 0 0 0 madfall:stone
mad.voxel.set 5 6 7 madfall:rebar_concrete 255 19 5
mad.world.save
mad.chunk.unload 0 0 0
mad.chunk.load 0 0 0
mad.voxel.get 5 6 7
```

### Where block definitions live

| Source | Path | Owner |
|---|---|---|
| First-party | `Definitions/blocks/*.json` | namespace `madfall` |
| Mods | `Mods/<modid>/definitions/blocks/*.json` | namespace must equal `<modid>` |
| Data assets | anywhere in `Content/` | namespace from the id |

First-party blocks deliberately go through the **same JSON loader a mod uses**.
If the shipped content could do something a mod cannot, the mod API would be a
lie, so it does not get a private path. `madfall:air` is the one exception: it
is installed programmatically at runtime id 0 before anything loads, because
`FMadChunkStorage`'s zero-filled default construction assumes it.

### The test map

`Content/Maps/L_MadFall_Test.umap` is generated by
`Scripts/make_test_map.py`, run headlessly through
`UnrealEditor-Cmd.exe -ExecutePythonScript=`. It is idempotent.

A `.umap` is an opaque binary in Git LFS. The starting map is scaffolding, not
art, and regenerating it from 40 lines of readable Python beats reviewing a
binary diff every time someone nudges the sun angle. The map holds a
DirectionalLight, SkyLight, SkyAtmosphere, ExponentialHeightFog, VolumetricCloud
and a PlayerStart — enough to confirm the project opened, nothing that could
later be mistaken for authored content.

---

## Dedicated server

**Deferred to Phase 6. Not built, not gated.** See
[the scope decision](#scope-decision-2026-09-12-single-player-first).

`Source/MadFallServer.Target.cs` is kept and is believed correct, but it is
unverified — it has never successfully compiled, because this machine cannot
build it. UnrealBuildTool refuses with:

```
Server targets are not currently supported from this engine distribution.
```

`C:\Program Files\Epic Games\UE_5.8` is a Launcher (installed) engine — it
contains `Engine/Build/InstalledBuild.txt` and ships precompiled `UnrealGame`
binaries but no Server target libraries. The check is in
`UEBuildTarget.cs:1403` and has no override.

Reviving the server in Phase 6 requires:

1. An Epic Games account linked to a GitHub account (grants access to the
   private `EpicGames/UnrealEngine` repository).
2. Cloning branch `5.8`, running `Setup.bat` then `GenerateProjectFiles.bat`,
   and building `UE5` in Development Editor. Budget ~150 GB of disk and 1–3
   hours of first build.
3. Pointing `MADFALL_UE_ROOT` at that engine and re-associating
   `MadFall.uproject` with it.

For the **Linux** dedicated server, additionally install Epic's clang
cross-toolchain for 5.8 and set `LINUX_MULTIARCH_ROOT`. `CI.ps1` already skips
the Linux server build with a visible message when that variable is unset,
rather than passing silently.

Until Phase 6, `CI.ps1` skips the server gate by default and the
`REQUIRE_SERVER` repository variable stays unset.

---

## Conventions

- Epic's C++ coding standard. Const-correct. No raw `new`/`delete` for
  `UObject`s. `TObjectPtr` for member `UObject` references.
- No blocking work on the game thread. Async everything heavy. The hard rule is
  no game-thread stall over 2 ms.
- Comments say **why**, not what. Any non-obvious algorithm choice is documented
  where it is made.
- Stat groups are declared up front in
  [`MadFallStats.h`](../Source/MadFallCore/Public/MadFallStats.h) —
  `STATGROUP_MadFallVoxel`, `_Mesher`, `_Structural`, `_ModLoad`, `_WorldGen`,
  `_Net` — so Unreal Insights traces are readable from the first commit.
  Retrofitting scopes after a system is already slow is how profiling gets
  skipped.
- Log categories: `LogMadFall` (umbrella), `LogMadFallMods`, `LogMadFallVoxel`,
  `LogMadFallRegistry`, `LogMadFallMesher`, `LogMadFallGameplay`,
  `LogMadFallStructural`, `LogMadFallEditor`.
- Every subsystem gets automation tests. Voxel serialization round-trip,
  structural integrity solver and mod load-order resolution need explicit
  coverage and are called out in the brief as non-negotiable.

### The Nanite boundary

Nanite is used on all static props and authored modular building meshes.
**Runtime-generated voxel meshes are not Nanite-friendly** — Nanite requires an
offline build step producing its cluster hierarchy, which cannot run per-edit on
a worker thread inside the frame budget. Generated geometry (Dual Contouring
terrain, greedy-meshed cubic construction) therefore goes through the standard
dynamic mesh pipeline with hand-authored LODs and distance-field updates.

This is why `render.nanite` in a block definition is honoured for
`"kind": "model"` blocks and ignored for `"kind": "isosurface"`. Phase 2 will
document the measured cost of both paths.

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallVoxelTypes.h"

class FMadBlockRegistry;

/**
 * The structural integrity model.
 *
 * WHAT PARTICIPATES
 *   Only solid voxels carrying EMadVoxelFlags::Cubic - player construction and
 *   stamped POI buildings - are structural members. Everything else is one of:
 *     anchor    natural (non-cubic) solid terrain, blocks whose definition has
 *               is_anchor, unresolved blocks from a missing mod, anything in an
 *               unloaded chunk, and the bottom of the world;
 *     empty     air, liquid, and non-solid density.
 *
 *   Natural terrain never collapses. That is a deliberate tradeoff, not an
 *   omission: making caves and overhangs structural would turn every mining
 *   trip into a physics job over millions of voxels, and the brief's pillar is
 *   that BASES take damage. Terrain under a base is where support comes from.
 *
 *   Unloaded chunks count as anchors because the only alternative - treating
 *   them as empty - collapses half a building the moment its other half streams
 *   out. A structure is only ever judged on what is actually in memory.
 *
 * STABILITY: SHORTEST SUPPORT PATH
 *   Every member gets a support distance: the cheapest path from any anchor,
 *   where stepping onto a neighbour costs
 *       resting on the block below          0
 *       beside a supporting block           Scale / effective_span(receiver)
 *       hanging under a supporting block    the horizontal cost x HangingCostMultiplier
 *   and effective_span = max_horizontal_span x the receiver's damage-stage
 *   support multiplier. A member whose distance exceeds Scale is unsupported.
 *
 *   Scale is 720720, the least common multiple of 1..16, so for every span a
 *   shipped block uses the per-step cost divides exactly: a span-6 concrete
 *   cantilever holds exactly 6 blocks and drops the 7th, with no rounding drift.
 *   Costs are summed along the path, so a wood block hanging off the end of a
 *   steel cantilever has less reach than it would off a column - mixed
 *   materials behave the way a player expects without any special case.
 *
 * LOAD: THE SHORTEST-PATH DAG
 *   Support distance alone would let a 40-storey wood tower stand on one post.
 *   So once distances are known, load flows back down the shortest-path DAG:
 *   every member splits its own mass plus everything it carries equally among
 *   the neighbours that are "tight" supporters (dist(u) + cost(u->v) ==
 *   dist(v)). A member fails when what it carries exceeds support_strength x
 *   stage multiplier. Anchors absorb whatever reaches them.
 *
 *   Dijkstra settles nodes in (distance, z) order. Zero-cost edges only ever
 *   point upward, so that order is a topological order of the DAG and one
 *   reverse sweep accumulates load exactly - O(n), no iteration to converge.
 *
 * WHY NOT FEM / A REAL STRUCTURAL SOLVE
 *   A stiffness-matrix solve gives real stress, but it is O(n^1.5) or worse,
 *   needs a linear solver that converges on degenerate voxel graphs, and its
 *   failures are hard for a player to read. This model is O(n log n), exactly
 *   deterministic in integers, resumable mid-solve, and its outcomes are
 *   explainable in one sentence ("that beam is 7 blocks from a column and
 *   concrete only spans 6"). That legibility is what 7 Days to Die players
 *   actually build against.
 *
 * FAILURE IS PROGRESSIVE
 *   A solve reports the members that fail NOW. Removing them changes the
 *   structure, which triggers the next solve. A column that buckles under load
 *   drops the floor above on the following pass rather than in the same one,
 *   which is both cheaper and reads as a collapse rather than a pop.
 */
namespace MadFall::Structural
{
	/** See the class comment. LCM(1..16). */
	inline constexpr int32 SupportScale = 720720;

	/** Distance value for "no path". */
	inline constexpr int32 Unreachable = MAX_int32;
}

/** Tuning. Defaults are what the shipped content is balanced against. */
struct MADFALLGAMEPLAY_API FMadStructuralSettings
{
	/** Hanging under a block costs this many times a horizontal step. */
	float HangingCostMultiplier = 1.0f;

	/**
	 * Largest connected structure one solve will gather. A structure beyond
	 * this is left standing and reported, rather than spending seconds of
	 * frame budget on a single edit. 65k voxels is a city block of concrete.
	 */
	int32 MaxComponentSize = 65536;

	/** Enables the load (overload) check. Off, only span matters. */
	bool bCheckLoad = true;
};

/** Structural properties of one block type, flattened for the hot loop. */
struct MADFALLGAMEPLAY_API FMadStructuralMaterial
{
	/** False for ids with no definition. Those are treated as anchors (see Get). */
	bool bKnown = false;

	bool bIsAnchor = false;

	/**
	 * Tagged block.natural (trees, cacti): generated sound - MadFall.Structural.GeneratedScatterStands
	 * proves it - so not re-checked when a chunk loads. A forest canopy is one
	 * structure tens of thousands of blocks wide, and re-solving it every time a
	 * chunk of it streamed in was most of the structural time on a walk through
	 * the woods. Edits still check it like anything else.
	 */
	bool bSkipLoadCheck = false;

	float MassKg = 0.0f;
	float SupportStrength = 0.0f;
	int32 MaxHorizontalSpan = 0;

	/** (damage threshold, multiplier), ascending by threshold. Empty means always 1.0. */
	TArray<TPair<uint8, float>> Stages;

	/** Support multiplier at a given FMadVoxel::Damage. */
	float GetMultiplier(uint8 Damage) const;

	/** Index into Stages for a damage value, 0 when there are no stages. */
	int32 GetStageIndex(uint8 Damage) const;
};

/**
 * Per-runtime-id structural table.
 *
 * Built once from the registry rather than looking definitions up per voxel:
 * the registry lookup is a map find behind a virtual call, and the solver
 * touches every member several times.
 */
class MADFALLGAMEPLAY_API FMadStructuralMaterials
{
public:
	/** Copies the structural fields of every registered block. */
	void Build(const FMadBlockRegistry& Registry);

	/** For tests and tools: sets one entry directly. */
	void Set(uint16 RuntimeId, const FMadStructuralMaterial& Material);

	/**
	 * Entry for a runtime id. An unknown id - including an unresolved block
	 * from a mod that is no longer installed - returns an anchor. Removing a mod
	 * must never be what brings a player's base down.
	 */
	const FMadStructuralMaterial& Get(uint16 RuntimeId) const;

	bool IsEmpty() const { return Materials.Num() == 0; }

private:
	TArray<FMadStructuralMaterial> Materials;
	FMadStructuralMaterial UnknownAnchor;
};

/**
 * Where the solver reads voxels from.
 *
 * An interface rather than UMadVoxelWorldSubsystem directly so the solver
 * stays a pure function of voxels, testable against an in-memory world, and
 * reusable by tools that validate a prefab before it is ever placed.
 */
class MADFALLGAMEPLAY_API IMadStructuralWorld
{
public:
	virtual ~IMadStructuralWorld() = default;

	/** False when the voxel's chunk is not in memory. The solver treats that as an anchor. */
	virtual bool IsLoaded(const FIntVector& Position) const = 0;

	virtual FMadVoxel GetVoxel(const FIntVector& Position) const = 0;
};

/** How a voxel takes part in the structure. */
enum class EMadStructuralRole : uint8
{
	Empty,
	Anchor,
	Member
};

/** Why a member failed. */
enum class EMadStructuralFailure : uint8
{
	None,

	/** No path to an anchor within span. */
	Unsupported,

	/** Supported, but carrying more than its strength. */
	Overloaded
};

/** One failed member. */
struct MADFALLGAMEPLAY_API FMadStructuralFailureRecord
{
	FIntVector Position = FIntVector::ZeroValue;
	FMadVoxel Voxel;
	EMadStructuralFailure Reason = EMadStructuralFailure::None;
};

/** Diagnostic view of one member after a solve. */
struct MADFALLGAMEPLAY_API FMadStructuralNodeReport
{
	/** 0 = on an anchor, SupportScale = at the limit of its span, Unreachable = unsupported. */
	int32 SupportDistance = MadFall::Structural::Unreachable;

	/** Kilograms carried, excluding the block's own mass. */
	float CarriedKg = 0.0f;

	/** Kilograms it may carry at its current damage stage. */
	float CapacityKg = 0.0f;

	/** max(span use, load use), 0..1+ - what stress shading displays. */
	float Stress = 0.0f;

	EMadStructuralFailure Failure = EMadStructuralFailure::None;
};

/** One member's stress for shading: see FMadStructuralNodeReport::Stress. */
struct FMadStressSample
{
	FIntVector Position = FIntVector::ZeroValue;
	float Stress = 0.0f;
	bool bFailing = false;
};

/**
 * One resumable structural solve.
 *
 * Created with a set of seed positions (typically a changed voxel and its six
 * neighbours). Step() does bounded work and returns true when finished, so the
 * owner can spread a large solve across frames. Everything is read from the
 * world during the gather phase and then held in the job, so later phases
 * never touch the world - which is what makes it safe to pause between frames,
 * as long as the owner discards the job if the world changes under it
 * (see Touches()).
 */
class MADFALLGAMEPLAY_API FMadStructuralJob
{
public:
	enum class EPhase : uint8
	{
		Gather,
		Solve,
		Load,
		Done
	};

	FMadStructuralJob(TArray<FIntVector> InSeeds, const FMadStructuralSettings& InSettings);

	/**
	 * Advances by roughly WorkUnits node visits. Returns true once Done.
	 *
	 * The world and materials must be the same objects on every call.
	 */
	bool Step(const IMadStructuralWorld& World, const FMadStructuralMaterials& Materials, int32 WorkUnits);

	/** Runs to completion. For tests and small edits. */
	void RunToCompletion(const IMadStructuralWorld& World, const FMadStructuralMaterials& Materials);

	EPhase GetPhase() const { return Phase; }
	bool IsDone() const { return Phase == EPhase::Done; }

	/**
	 * True if a change at Position could invalidate this job: it is a member
	 * the job gathered, or a neighbour of one (whose role the job recorded).
	 */
	bool Touches(const FIntVector& Position) const;

	const TArray<FIntVector>& GetSeeds() const { return Seeds; }

	/** Members that fail. Valid once Done. Deterministic order (gather order). */
	const TArray<FMadStructuralFailureRecord>& GetFailures() const { return Failures; }

	/** True when the component exceeded MaxComponentSize and was left alone. */
	bool WasTruncated() const { return bTruncated; }

	int32 NumMembers() const { return Nodes.Num(); }

	/** Diagnostics for a gathered member, once Done. */
	bool GetNodeReport(const FIntVector& Position, FMadStructuralNodeReport& OutReport) const;

	/**
	 * Stress of the members within Radius voxels (Chebyshev) of Centre, nearest
	 * first, at most MaxCount - what stress shading draws around the block a
	 * builder is aiming at. Empty until Done.
	 */
	void CollectStressNear(const FIntVector& Centre, int32 Radius, int32 MaxCount, TArray<FMadStressSample>& OutSamples) const;

	/**
	 * The member under the most stress that did NOT fail, once Done - what
	 * creaks. Tracked during the load sweep, where each member's carried load is
	 * final the moment it is visited, so it costs no extra pass. False when every
	 * member failed or the job was truncated. Ties keep the first visited.
	 */
	bool GetMostStressed(FMadStressSample& OutSample) const
	{
		OutSample = MostStressed;
		return bHasMostStressed && Phase == EPhase::Done && !bTruncated;
	}

	/** One line per neighbour of a gathered member - what the job saw there (member and its distance, anchor, empty). For collapse diagnostics. */
	FString DescribeNeighbours(const FIntVector& Position) const;

	/** Classifies a voxel. Exposed for the subsystem's change filter and for tests. */
	static EMadStructuralRole Classify(const FMadVoxel& Voxel, const FMadStructuralMaterials& Materials);

private:
	struct FNode;
	/** max(span use, load use): FMadStructuralNodeReport::Stress. */
	static float NodeStress(const FNode& Node);

	/** Neighbour slot values below 0. */
	static constexpr int32 NeighbourAnchor = -1;
	static constexpr int32 NeighbourEmpty = -2;

	struct FNode
	{
		FIntVector Position;
		FMadVoxel Voxel;

		/** Node index, NeighbourAnchor or NeighbourEmpty, in DirectionOffsets order. */
		int32 Neighbours[6];

		/** Horizontal step cost onto this node. MAX_int32 when it cannot cantilever at all. */
		int32 StepCost = 0;
		float MassKg = 0.0f;
		float CapacityKg = 0.0f;

		/** False when a damage stage has taken it to zero strength: it cannot hold anything up. */
		bool bCanSupport = true;

		int32 Distance = MadFall::Structural::Unreachable;
		float Accumulated = 0.0f;
		EMadStructuralFailure Failure = EMadStructuralFailure::None;
	};

	/** Cost for the node at Index to receive support from a neighbour in direction Dir. */
	int32 EdgeCost(int32 Index, int32 Dir) const;

	bool StepGather(const IMadStructuralWorld& World, const FMadStructuralMaterials& Materials, int32& Budget);
	bool StepSolve(int32& Budget);
	bool StepLoad(int32& Budget);
	void Finish();

	EMadStructuralRole ClassifyAt(const IMadStructuralWorld& World, const FMadStructuralMaterials& Materials,
		const FIntVector& Position, FMadVoxel& OutVoxel) const;

	TArray<FIntVector> Seeds;
	FMadStructuralSettings Settings;
	EPhase Phase = EPhase::Gather;

	TArray<FNode> Nodes;
	TMap<FIntVector, int32> NodeIndex;

	// Gather state.
	int32 SeedCursor = 0;
	int32 GatherCursor = 0;

	// Solve state: a binary heap of (distance, z, node).
	struct FHeapEntry
	{
		int32 Distance;
		int32 Z;
		int32 Node;
	};
	TArray<FHeapEntry> Heap;
	TArray<bool> Settled;
	TArray<int32> SettleOrder;
	bool bSolveInitialised = false;

	// Load state.
	int32 LoadCursor = 0;

	TArray<FMadStructuralFailureRecord> Failures;
	bool bTruncated = false;

	FMadStressSample MostStressed;
	bool bHasMostStressed = false;
};

class FMadChunkStorage;
struct FMadChunkCoord;

namespace MadFall::Structural
{
	/**
	 * Every structural member in one chunk, as world positions, for seeding a
	 * check when the chunk arrives.
	 *
	 * The palette is checked first: a chunk whose palette holds no block type
	 * that can be a member (all terrain, all air - nearly every chunk) returns
	 * after touching a handful of entries instead of 32768 voxels. Caller holds
	 * the chunk's read lock.
	 */
	MADFALLGAMEPLAY_API void FindMembersInChunk(const FMadChunkStorage& Storage, const FMadChunkCoord& Coord,
		const FMadStructuralMaterials& Materials, TArray<FIntVector>& OutMembers);
}

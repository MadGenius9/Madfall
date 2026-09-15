// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadInventory.h"
#include "MadStructuralSolver.h"
#include "Math/RandomStream.h"
#include "Templates/Function.h"

/** One voxel of a falling cluster, at its ORIGINAL position. */
struct MADFALLGAMEPLAY_API FMadDebrisBlock
{
	FIntVector Position = FIntVector::ZeroValue;
	FMadVoxel Voxel;
	float MassKg = 0.0f;
};

/**
 * A 6-connected group of collapsed voxels that falls as one rigid piece.
 *
 * WHY KINEMATIC AND NOT CHAOS
 *   Collapsed voxels fall straight down under gravity, on the voxel grid, and
 *   land exactly. The alternative - a Chaos rigid body or geometry collection
 *   per cluster - tumbles beautifully but is non-deterministic, costs a physics
 *   body per chunk of building, and lands at a transform that has to be snapped
 *   back to the grid to damage anything, at which point the result depends on
 *   frame rate. The gameplay outcome (what gets hit, how hard, where rubble
 *   ends up) is computed here, deterministically; purely cosmetic tumbling and
 *   dust can be layered on the visual actor later without touching it.
 */
struct MADFALLGAMEPLAY_API FMadDebrisCluster
{
	TArray<FMadDebrisBlock> Blocks;

	/** Indices into Blocks with no cluster block directly beneath them: the only faces that can hit anything. */
	TArray<int32> BottomBlocks;

	float MassKg = 0.0f;

	/** Whole voxels fallen so far. */
	int32 Dropped = 0;

	/** Continuous fall distance in voxels (1 voxel = 1 m). Always >= Dropped. */
	float FallDistance = 0.0f;

	/** Metres per second, downward. */
	float Velocity = 0.0f;

	bool bLanded = false;

	/** Position of a block after the fall so far. */
	FIntVector GetLandedPosition(int32 BlockIndex) const
	{
		return Blocks[BlockIndex].Position - FIntVector(0, 0, Dropped);
	}
};

/** A landing contact: the voxel under a bottom block, and the energy it absorbs. */
struct MADFALLGAMEPLAY_API FMadDebrisImpact
{
	FIntVector Position = FIntVector::ZeroValue;
	float EnergyJ = 0.0f;
};

namespace MadFall::Debris
{
	/** Metres per second squared. Voxels are 1 m. */
	inline constexpr float Gravity = 9.81f;

	/**
	 * Groups failures into 6-connected clusters. Deterministic: clusters are
	 * ordered by their first failure in the input, and blocks within a cluster
	 * by discovery order from it.
	 */
	MADFALLGAMEPLAY_API TArray<FMadDebrisCluster> BuildClusters(
		const TArray<FMadStructuralFailureRecord>& Failures, const FMadStructuralMaterials& Materials);

	/**
	 * Advances a cluster's fall by DeltaSeconds. IsFree(position) says whether a
	 * voxel can be fallen into. Returns true once the cluster has landed.
	 *
	 * A cluster lands the moment any bottom block would enter a non-free voxel,
	 * or after MaxDrop voxels (the bottom of the world, in practice).
	 */
	MADFALLGAMEPLAY_API bool Advance(FMadDebrisCluster& Cluster, float DeltaSeconds,
		TFunctionRef<bool(const FIntVector&)> IsFree, int32 MaxDrop = 512);

	/** Advances in fixed steps until landed. For flushing and tests. */
	MADFALLGAMEPLAY_API void AdvanceToLanding(FMadDebrisCluster& Cluster,
		TFunctionRef<bool(const FIntVector&)> IsFree, int32 MaxDrop = 512);

	/**
	 * Contacts of a landed cluster. Kinetic energy 1/2 m v^2 is split evenly over
	 * every bottom block that is resting on something; a cluster that never fell
	 * (an overloaded block crushed where it stood) produces no impacts.
	 */
	MADFALLGAMEPLAY_API void ComputeImpacts(const FMadDebrisCluster& Cluster,
		TFunctionRef<bool(const FIntVector&)> IsFree, TArray<FMadDebrisImpact>& OutImpacts);

	/**
	 * Where rubble settles. Roughly one block in KeepOneIn (chosen by a hash of
	 * the original position, so it is deterministic) survives as rubble, and the
	 * survivors in each (x, y) column are compacted down onto the landing
	 * surface rather than hanging in the air where the cluster happened to stop.
	 */
	MADFALLGAMEPLAY_API void ComputeRubble(const FMadDebrisCluster& Cluster, int32 KeepOneIn,
		TArray<TPair<FIntVector, int32>>& OutBlockIndexAtPosition);

	/**
	 * Kinetic energy (J) delivered to a pawn standing with its feet in Feet and
	 * HeightVoxels tall, if a bottom block of the cluster swept through the
	 * pawn's voxels while the cluster fell from FromDropped to its current
	 * Dropped. 0 when nothing hit.
	 *
	 * The mass is the whole (x, y) column of the cluster above the contact: a
	 * wall falling on someone hits with the wall, not one block of it. Checking
	 * the swept interval rather than the current voxel means a fast cluster
	 * cannot step over a pawn between ticks.
	 */
	MADFALLGAMEPLAY_API float ComputeSweptHitEnergy(const FMadDebrisCluster& Cluster, int32 FromDropped,
		const FIntVector& Feet, int32 HeightVoxels);

	/**
	 * The voxels a standing pawn's capsule occupies (Location is the capsule
	 * centre, in uu): every voxel its bounding box overlaps by more than a few
	 * centimetres, feet to head, grown by Skin (cm) on every side but the bottom.
	 * Landing rubble is never written into these, so a collapse hurts whoever it
	 * falls on but does not seal them inside a block.
	 */
	MADFALLGAMEPLAY_API void GetPawnVoxels(const FVector& Location, float HalfHeight, float Radius, TArray<FIntVector>& OutVoxels, float Skin = 0.0f);

	/**
	 * Items a landed cluster leaves behind: one roll of each block's collapse
	 * loot table, skipping the blocks that became rubble (the rubble is the
	 * block's remains; it drops its own loot when broken). Stacks are merged.
	 * CollapseTableOf returns the table for a block type, or null.
	 */
	MADFALLGAMEPLAY_API void RollCollapseDrops(const FMadDebrisCluster& Cluster, const TArray<TPair<FIntVector, int32>>& Rubble,
		TFunctionRef<const FMadLootTableDefinition*(uint16 BlockTypeId)> CollapseTableOf,
		const FMadGameplayDefinitions& Definitions, FRandomStream& Random, TArray<FMadItemStack>& OutStacks);
}

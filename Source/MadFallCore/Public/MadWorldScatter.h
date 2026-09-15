// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Shapes of the features biomes scatter (trees, boulders, plants).
 *
 * Pure functions of a seed, shared by world generation and by the tests that
 * check a generated tree stands up structurally - the same reason POIs are
 * checked from their prefab files rather than from a hand-built copy.
 */
namespace MadFall::Scatter
{
	/**
	 * Voxel offsets of a tree whose root column's top solid voxel is (0, 0, 0):
	 * the trunk runs from (0, 0, 1) to (0, 0, Height), the canopy rounds off
	 * around and above its top. Outer canopy voxels are thinned by the seed so
	 * two trees of one species are not identical.
	 *
	 * Leaves are kept only where the structural solver would hold them up from
	 * the trunk: resting on a leaf is free, a step sideways or hanging under one
	 * costs one, and no leaf may cost more than LeafSpan (the leaves block's
	 * max_horizontal_span).
	 */
	MADFALLCORE_API void BuildTree(uint32 Seed, int32 Height, float Radius, bool bLeaves,
		TArray<FIntVector>& OutTrunk, TArray<FIntVector>& OutLeaves, int32 LeafSpan = 4);

	/** The per-column roll, in [0, 1). A column grows feature i when the roll falls in its slice of the cumulative chances. */
	MADFALLCORE_API float RollColumn(uint32 Seed, int32 WorldX, int32 WorldY);
}

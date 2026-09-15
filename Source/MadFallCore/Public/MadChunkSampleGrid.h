// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallCoordinates.h"
#include "MadFallVoxelTypes.h"

/**
 * An immutable snapshot of one chunk plus a one-voxel margin from its six
 * neighbours.
 *
 * WHY A COPY INSTEAD OF READING THE LIVE CHUNKS:
 * A meshing job runs for tens of milliseconds. Holding read locks on seven
 * chunks for that long would make any edit inside them wait on the mesher, and
 * an edit that waits is a game-thread stall. Copying 34^3 samples costs about
 * 157 KB and one memcpy-shaped pass, after which the worker touches nothing
 * shared and the player can keep mining the block being meshed.
 *
 * Margin semantics: index space runs -1..32 on every axis. A neighbour chunk
 * that is not loaded contributes air, which is the right assumption for the
 * edge of the loaded world - it produces a surface there rather than a hole.
 */
struct MADFALLCORE_API FMadChunkSampleGrid
{
	/** 32 + 2 samples per axis: one margin voxel on each side. */
	static constexpr int32 Size = MadFall::ChunkSize + 2;
	static constexpr int32 Count = Size * Size * Size;

	/** Which chunk the centre of this grid is. */
	FMadChunkCoord Coord;

	/** Density, block id and flags, in separate arrays because the meshers scan them separately. */
	TArray<uint8> Density;
	TArray<uint16> BlockId;
	TArray<uint8> Flags;

	FMadChunkSampleGrid()
	{
		Density.SetNumZeroed(Count);
		BlockId.SetNumZeroed(Count);
		Flags.SetNumZeroed(Count);
	}

	/** Grid index for a voxel in [-1, 32] on each axis. X varies fastest, matching chunk order. */
	FORCEINLINE static int32 Index(int32 X, int32 Y, int32 Z)
	{
		checkSlow(X >= -1 && X <= MadFall::ChunkSize);
		checkSlow(Y >= -1 && Y <= MadFall::ChunkSize);
		checkSlow(Z >= -1 && Z <= MadFall::ChunkSize);
		return (X + 1) + Size * ((Y + 1) + Size * (Z + 1));
	}

	FORCEINLINE uint8 GetDensity(int32 X, int32 Y, int32 Z) const { return Density[Index(X, Y, Z)]; }
	FORCEINLINE uint16 GetBlockId(int32 X, int32 Y, int32 Z) const { return BlockId[Index(X, Y, Z)]; }
	FORCEINLINE uint8 GetFlags(int32 X, int32 Y, int32 Z) const { return Flags[Index(X, Y, Z)]; }

	FORCEINLINE bool IsSolid(int32 X, int32 Y, int32 Z) const { return GetDensity(X, Y, Z) >= 128; }

	/** Player-placed construction, which routes to the cubic mesher rather than the isosurface path. */
	FORCEINLINE bool IsCubic(int32 X, int32 Y, int32 Z) const
	{
		return (GetFlags(X, Y, Z) & static_cast<uint8>(EMadVoxelFlags::Cubic)) != 0;
	}

	/** True if nothing in the grid is solid - the mesher can skip the whole chunk. */
	bool IsEmpty() const
	{
		for (uint8 Value : Density)
		{
			if (Value >= 128)
			{
				return false;
			}
		}
		return true;
	}

	/** True if everything in the grid is solid - an interior chunk with no surface. */
	bool IsFullySolid() const
	{
		for (uint8 Value : Density)
		{
			if (Value < 128)
			{
				return false;
			}
		}
		return true;
	}
};

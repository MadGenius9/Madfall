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

	/**
	 * Damage of this chunk's own voxels, 1-255, for the ones that have any.
	 *
	 * Sparse and centre-only on purpose. Damage is stored sparsely because
	 * almost no voxel has any, and probing it for all 39304 samples of a
	 * snapshot is exactly what GetMeshSample was written to avoid; and a face is
	 * drawn by the chunk that owns the voxel, so a margin voxel's damage is
	 * never needed. Keyed by MadFall::VoxelIndex within the chunk.
	 */
	TMap<int32, uint8> Damage;

	/** True when anything in this chunk is damaged; the meshers skip the lookups otherwise. */
	FORCEINLINE bool HasDamage() const { return !Damage.IsEmpty(); }

	/** Damage of a voxel of this chunk, 0 when undamaged or outside it. */
	FORCEINLINE uint8 GetDamage(int32 X, int32 Y, int32 Z) const
	{
		if (Damage.IsEmpty() || X < 0 || Y < 0 || Z < 0
			|| X >= MadFall::ChunkSize || Y >= MadFall::ChunkSize || Z >= MadFall::ChunkSize)
		{
			return 0;
		}
		return Damage.FindRef(MadFall::VoxelIndex(X, Y, Z));
	}

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

/**
 * Every Stride-th column of a chunk's voxels, plus one lattice step of margin
 * on each side, for meshing a distant chunk's terrain at a lower level of
 * detail.
 *
 * WHY ONLY HORIZONTALLY: terrain is mostly a heightfield, so its triangles grow
 * with ground area and a horizontal stride of 2 already quarters them. A
 * vertical stride too would move every flat surface by up to half a stride -
 * water lying a voxel higher or lower in the distance showed a dark step along
 * every detail-level boundary on the sea. Keeping all 34 layers keeps heights
 * exact, cliffs sharp, and the seams between levels to the horizontal alone.
 *
 * WHY POINT SAMPLES AND NOT BLOCK AVERAGES: two neighbouring chunks at the same
 * level must agree on every lattice point they share, or their surfaces part at
 * the seam. Every chunk origin is a multiple of 32, so of any stride, and a
 * point sample at a world-aligned voxel is the same number whichever chunk reads
 * it. An average would agree too, but it smears the density ramp the vertex
 * placement interpolates and costs Stride^3 reads per point.
 *
 * Index space runs -1..Points in X and Y, where lattice point i is voxel
 * i * Stride (so the margin reaches Stride voxels into each neighbour), and
 * -1..ChunkSize in Z, one per voxel as in the full grid.
 */
struct MADFALLCORE_API FMadLodSampleGrid
{
	FMadChunkCoord Coord;

	/** 2, 4 or 8: voxels per horizontal lattice step. */
	int32 Stride = 1;

	/** Horizontal lattice steps across a chunk (ChunkSize / Stride). */
	int32 Points = MadFall::ChunkSize;

	/** Vertical layers, margin included. */
	static constexpr int32 SizeZ = MadFall::ChunkSize + 2;

	TArray<uint8> Density;
	TArray<uint16> BlockId;
	TArray<uint8> Flags;

	void Init(int32 InStride)
	{
		check(InStride >= 1 && InStride <= MadFall::ChunkSize && MadFall::ChunkSize % InStride == 0);
		Stride = InStride;
		Points = MadFall::ChunkSize / Stride;
		const int32 Count = GetSize() * GetSize() * SizeZ;
		Density.SetNumZeroed(Count);
		BlockId.SetNumZeroed(Count);
		Flags.SetNumZeroed(Count);
	}

	FORCEINLINE int32 GetSize() const { return Points + 2; }

	/** Grid index for a lattice point: X and Y in [-1, Points], Z in [-1, ChunkSize]. */
	FORCEINLINE int32 Index(int32 X, int32 Y, int32 Z) const
	{
		checkSlow(X >= -1 && X <= Points && Y >= -1 && Y <= Points && Z >= -1 && Z <= MadFall::ChunkSize);
		return (X + 1) + GetSize() * ((Y + 1) + GetSize() * (Z + 1));
	}

	FORCEINLINE uint8 GetDensity(int32 X, int32 Y, int32 Z) const { return Density[Index(X, Y, Z)]; }
	FORCEINLINE uint16 GetBlockId(int32 X, int32 Y, int32 Z) const { return BlockId[Index(X, Y, Z)]; }
	FORCEINLINE bool IsSolid(int32 X, int32 Y, int32 Z) const { return GetDensity(X, Y, Z) >= 128; }
	FORCEINLINE bool IsCubic(int32 X, int32 Y, int32 Z) const
	{
		return (Flags[Index(X, Y, Z)] & static_cast<uint8>(EMadVoxelFlags::Cubic)) != 0;
	}

	/**
	 * Fills every lattice point from a sampler over chunk-local voxel coordinates
	 * (X and Y run from -Stride to ChunkSize + Stride, Z from -1 to ChunkSize). Tests use it directly; the
	 * world's snapshot passes a sampler that reads the 27 neighbouring chunks.
	 */
	template <typename SamplerType>
	void Fill(SamplerType&& Sample)
	{
		for (int32 Z = -1; Z <= MadFall::ChunkSize; ++Z)
		{
			for (int32 Y = -1; Y <= Points; ++Y)
			{
				for (int32 X = -1; X <= Points; ++X)
				{
					const int32 GridIndex = Index(X, Y, Z);
					Sample(X * Stride, Y * Stride, Z, BlockId[GridIndex], Density[GridIndex], Flags[GridIndex]);
				}
			}
		}
	}
	/**
	 * Distant terrain does not crack: the cell network would alias into a grey
	 * haze long before a coarse chunk is close enough to read. The mesher is one
	 * template over both grids, so the answer is here rather than in an #if.
	 */
	FORCEINLINE bool HasDamage() const { return false; }
	FORCEINLINE uint8 GetDamage(int32, int32, int32) const { return 0; }

};

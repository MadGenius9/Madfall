// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallVoxelTypes.h"
#include "MadFallCoordinates.generated.h"

/**
 * Coordinate spaces, and the conversions between them.
 *
 * There are four and they are easy to mix up, so every conversion lives here
 * and nowhere else:
 *
 *   World voxel   (int32 x,y,z)  absolute voxel address. Z is clamped to
 *                                [WorldMinZ, WorldMaxZ]. Infinite in X/Y.
 *   Chunk         (int32 x,y,z)  world voxel / 32, FLOOR division (negative
 *                                coordinates must not round toward zero).
 *   Local         (int32 0..31)  position inside a chunk.
 *   Region        (int32 x,y)    chunk / 16 in XY, floor division. Regions span
 *                                the whole vertical range, so there is no
 *                                region Z.
 *
 * Unreal world space is voxel * MadFall::VoxelSizeUU, with the voxel's origin
 * at its minimum corner.
 */

// ===========================================================================
// FMadChunkCoord
// ===========================================================================

USTRUCT(BlueprintType)
struct MADFALLCORE_API FMadChunkCoord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Coordinates")
	int32 X = 0;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Coordinates")
	int32 Y = 0;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Coordinates")
	int32 Z = 0;

	FMadChunkCoord() = default;
	FMadChunkCoord(int32 InX, int32 InY, int32 InZ) : X(InX), Y(InY), Z(InZ) {}

	bool operator==(const FMadChunkCoord& Other) const
	{
		return X == Other.X && Y == Other.Y && Z == Other.Z;
	}

	bool operator!=(const FMadChunkCoord& Other) const { return !(*this == Other); }

	/** True if Z is inside the world's vertical range. X and Y are unbounded. */
	bool IsValidZ() const
	{
		return Z >= MadFall::WorldMinChunkZ && Z <= MadFall::WorldMaxChunkZ;
	}

	FString ToString() const
	{
		return FString::Printf(TEXT("(%d, %d, %d)"), X, Y, Z);
	}
};

FORCEINLINE uint32 GetTypeHash(const FMadChunkCoord& Coord)
{
	// Chunk coords are dense and small in magnitude near the origin, so the
	// naive "xor the three ints" hash collides badly for mirrored coordinates
	// like (1,2,3) and (3,2,1). Mix with distinct odd multipliers instead.
	uint32 Hash = static_cast<uint32>(Coord.X) * 0x9E3779B1u;
	Hash ^= static_cast<uint32>(Coord.Y) * 0x85EBCA77u;
	Hash ^= static_cast<uint32>(Coord.Z) * 0xC2B2AE3Du;
	Hash ^= (Hash >> 15);
	return Hash;
}

// ===========================================================================
// FMadRegionCoord
// ===========================================================================

USTRUCT(BlueprintType)
struct MADFALLCORE_API FMadRegionCoord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Coordinates")
	int32 X = 0;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Coordinates")
	int32 Y = 0;

	FMadRegionCoord() = default;
	FMadRegionCoord(int32 InX, int32 InY) : X(InX), Y(InY) {}

	bool operator==(const FMadRegionCoord& Other) const { return X == Other.X && Y == Other.Y; }
	bool operator!=(const FMadRegionCoord& Other) const { return !(*this == Other); }

	FString ToString() const { return FString::Printf(TEXT("(%d, %d)"), X, Y); }
};

FORCEINLINE uint32 GetTypeHash(const FMadRegionCoord& Coord)
{
	return HashCombine(static_cast<uint32>(Coord.X), static_cast<uint32>(Coord.Y) * 0x9E3779B1u);
}

// ===========================================================================
// Conversions
// ===========================================================================

namespace MadFall
{
	/**
	 * Floor division. C++ integer division truncates toward zero, so -1 / 32
	 * is 0, which would put voxel Z = -1 in chunk 0 alongside Z = 0. Every
	 * world-to-chunk conversion in the codebase goes through this.
	 */
	FORCEINLINE constexpr int32 FloorDiv(int32 Value, int32 Divisor)
	{
		const int32 Quotient = Value / Divisor;
		return ((Value % Divisor != 0) && ((Value < 0) != (Divisor < 0))) ? Quotient - 1 : Quotient;
	}

	/** Always in [0, Divisor). Companion to FloorDiv. */
	FORCEINLINE constexpr int32 FloorMod(int32 Value, int32 Divisor)
	{
		const int32 Remainder = Value % Divisor;
		return (Remainder != 0 && (Remainder < 0) != (Divisor < 0)) ? Remainder + Divisor : Remainder;
	}

	/** World voxel coordinate -> the chunk containing it. */
	/** The voxel containing a world position in centimetres (uu). */
	FORCEINLINE FIntVector WorldCmToVoxel(const FVector& WorldCm)
	{
		return FIntVector(
			FMath::FloorToInt32(WorldCm.X / VoxelSizeUU),
			FMath::FloorToInt32(WorldCm.Y / VoxelSizeUU),
			FMath::FloorToInt32(WorldCm.Z / VoxelSizeUU));
	}

	FORCEINLINE FMadChunkCoord WorldToChunk(int32 WorldX, int32 WorldY, int32 WorldZ)
	{
		return FMadChunkCoord(
			FloorDiv(WorldX, ChunkSize),
			FloorDiv(WorldY, ChunkSize),
			FloorDiv(WorldZ, ChunkSize));
	}

	/** World voxel coordinate -> position within its chunk, each in [0, 32). */
	FORCEINLINE void WorldToLocal(int32 WorldX, int32 WorldY, int32 WorldZ,
		int32& OutLocalX, int32& OutLocalY, int32& OutLocalZ)
	{
		OutLocalX = FloorMod(WorldX, ChunkSize);
		OutLocalY = FloorMod(WorldY, ChunkSize);
		OutLocalZ = FloorMod(WorldZ, ChunkSize);
	}

	/** Chunk + local position -> world voxel coordinate. */
	FORCEINLINE void ChunkToWorld(const FMadChunkCoord& Chunk, int32 LocalX, int32 LocalY, int32 LocalZ,
		int32& OutWorldX, int32& OutWorldY, int32& OutWorldZ)
	{
		OutWorldX = Chunk.X * ChunkSize + LocalX;
		OutWorldY = Chunk.Y * ChunkSize + LocalY;
		OutWorldZ = Chunk.Z * ChunkSize + LocalZ;
	}

	/** Chunk -> the region file holding it. */
	FORCEINLINE FMadRegionCoord ChunkToRegion(const FMadChunkCoord& Chunk)
	{
		return FMadRegionCoord(FloorDiv(Chunk.X, RegionChunksXY), FloorDiv(Chunk.Y, RegionChunksXY));
	}

	/**
	 * Chunk -> its slot in a region's 4096-entry index table, or INDEX_NONE if
	 * the chunk's Z is outside the world.
	 *
	 * Layout is local_x + 16 * (local_y + 16 * (chunk_z - WorldMinChunkZ)), so
	 * a vertical column of 16 chunks is 16 entries apart in the table and a
	 * column scan touches one cache line per 4 entries.
	 */
	FORCEINLINE int32 ChunkToRegionSlot(const FMadChunkCoord& Chunk)
	{
		if (!Chunk.IsValidZ())
		{
			return INDEX_NONE;
		}

		const int32 LocalX = FloorMod(Chunk.X, RegionChunksXY);
		const int32 LocalY = FloorMod(Chunk.Y, RegionChunksXY);
		const int32 LayerZ = Chunk.Z - WorldMinChunkZ;

		return LocalX + RegionChunksXY * (LocalY + RegionChunksXY * LayerZ);
	}

	/** Inverse of ChunkToRegionSlot. */
	FORCEINLINE FMadChunkCoord RegionSlotToChunk(const FMadRegionCoord& Region, int32 Slot)
	{
		checkSlow(Slot >= 0 && Slot < RegionChunkSlots);

		const int32 LocalX = Slot % RegionChunksXY;
		const int32 LocalY = (Slot / RegionChunksXY) % RegionChunksXY;
		const int32 LayerZ = Slot / (RegionChunksXY * RegionChunksXY);

		return FMadChunkCoord(
			Region.X * RegionChunksXY + LocalX,
			Region.Y * RegionChunksXY + LocalY,
			LayerZ + WorldMinChunkZ);
	}

	/** World voxel coordinate -> Unreal world position of the voxel's minimum corner. */
	FORCEINLINE FVector WorldVoxelToUnreal(int32 WorldX, int32 WorldY, int32 WorldZ)
	{
		return FVector(WorldX * VoxelSizeUU, WorldY * VoxelSizeUU, WorldZ * VoxelSizeUU);
	}

	/** Unreal world position -> the voxel containing it. */
	FORCEINLINE void UnrealToWorldVoxel(const FVector& Position, int32& OutX, int32& OutY, int32& OutZ)
	{
		OutX = static_cast<int32>(FMath::FloorToDouble(Position.X / VoxelSizeUU));
		OutY = static_cast<int32>(FMath::FloorToDouble(Position.Y / VoxelSizeUU));
		OutZ = static_cast<int32>(FMath::FloorToDouble(Position.Z / VoxelSizeUU));
	}

	/** True if a world voxel Z is inside the bedrock..build-ceiling range. */
	FORCEINLINE constexpr bool IsValidWorldZ(int32 WorldZ)
	{
		return WorldZ >= WorldMinZ && WorldZ <= WorldMaxZ;
	}
}

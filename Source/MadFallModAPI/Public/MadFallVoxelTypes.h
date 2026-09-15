// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallVoxelTypes.generated.h"

// ===========================================================================
// World constants
// ===========================================================================

namespace MadFall
{
	/** Voxels per chunk edge. */
	inline constexpr int32 ChunkSize = 32;

	/** 32 * 32 * 32. */
	inline constexpr int32 ChunkVoxelCount = ChunkSize * ChunkSize * ChunkSize;

	/** World-space size of one voxel, in Unreal units (1 m). */
	inline constexpr float VoxelSizeUU = 100.0f;

	/** Lowest world voxel Z (bedrock). Inclusive. */
	inline constexpr int32 WorldMinZ = -128;

	/** Highest world voxel Z (build ceiling). Inclusive. */
	inline constexpr int32 WorldMaxZ = 383;

	/**
	 * 512 voxels of vertical range == exactly 16 chunk layers.
	 *
	 * This number is load-bearing: it is what makes a region file a clean
	 * 16 x 16 x 16 = 4096-slot index with no ragged edge. Changing the build
	 * ceiling resizes the region index table and bumps the save format version.
	 */
	inline constexpr int32 WorldChunkLayers = (WorldMaxZ - WorldMinZ + 1) / ChunkSize;
	static_assert(WorldChunkLayers == 16, "Vertical range must be a whole number of chunk layers");

	/** Lowest chunk Z index. */
	inline constexpr int32 WorldMinChunkZ = WorldMinZ / ChunkSize;                  // -4

	/** Highest chunk Z index, inclusive. */
	inline constexpr int32 WorldMaxChunkZ = WorldMinChunkZ + WorldChunkLayers - 1;  // 11

	/** Chunks per region edge, in X and Y. */
	inline constexpr int32 RegionChunksXY = 16;

	/** 16 * 16 * 16 chunk slots per region file. */
	inline constexpr int32 RegionChunkSlots = RegionChunksXY * RegionChunksXY * WorldChunkLayers;
	static_assert(RegionChunkSlots == 4096, "Region index table is sized for exactly 4096 slots");

	/**
	 * Linear voxel index within a chunk. X varies fastest.
	 *
	 * Every subsystem that touches voxel memory - mesher, serializer, structural
	 * solver, network delta encoder - must agree on this ordering. A mismatch is
	 * silent data corruption rather than a crash, so it lives in exactly one place.
	 */
	FORCEINLINE constexpr int32 VoxelIndex(int32 X, int32 Y, int32 Z)
	{
		return X + ChunkSize * (Y + ChunkSize * Z);
	}

	/** Inverse of VoxelIndex. */
	FORCEINLINE constexpr void VoxelCoords(int32 Index, int32& OutX, int32& OutY, int32& OutZ)
	{
		OutX = Index % ChunkSize;
		OutY = (Index / ChunkSize) % ChunkSize;
		OutZ = Index / (ChunkSize * ChunkSize);
	}
}

// ===========================================================================
// Reserved block type IDs
// ===========================================================================

namespace MadFall
{
	/** Runtime ID for "madfall:air". Always 0, never remappable. */
	inline constexpr uint16 BlockTypeAir = 0;

	/**
	 * Runtime ID for "madfall:unresolved" - a block whose defining mod is not
	 * installed. The original namespaced string is retained in the chunk palette
	 * and written back out unchanged on save, so uninstalling and reinstalling a
	 * mod is lossless. Mapping unknown IDs to air on load is how voxel games
	 * silently delete a player base; we do not do it.
	 */
	inline constexpr uint16 BlockTypeUnresolved = 65535;
}

// ===========================================================================
// Voxel flags
// ===========================================================================

/** Per-voxel bit flags. Packed into FMadVoxel::Flags. */
enum class EMadVoxelFlags : uint8
{
	None            = 0,

	/**
	 * Player-placed construction. Routes to the greedy cubic mesher and the
	 * snapped build grid; cleared voxels route to Dual Contouring.
	 * This single bit is the terrain/construction seam.
	 */
	Cubic           = 1 << 0,

	/** Immovable support source. Terminates structural flood-fill. */
	Anchor          = 1 << 1,

	/** Participates in the fluid tick. */
	Liquid          = 1 << 2,

	/** Extra state lives in the chunk block-entity side table. */
	HasBlockEntity  = 1 << 3,

	/**
	 * Differs from worldgen output. A chunk with zero PlayerModified bits and a
	 * matching seed + worldgen version is never written to disk at all.
	 */
	PlayerModified  = 1 << 4,

	/** Queued for the structural integrity solver. Transient: masked off on save. */
	SupportDirty    = 1 << 5

	// bits 6-7 reserved; must be written zero, ignored on read.
};
ENUM_CLASS_FLAGS(EMadVoxelFlags);

namespace MadFall
{
	/** Runtime-only flag bits that must never reach the save file. */
	inline constexpr uint8 TransientVoxelFlagMask = static_cast<uint8>(EMadVoxelFlags::SupportDirty);
}

// ===========================================================================
// FMadVoxel - the packed interchange type
// ===========================================================================

#pragma pack(push, 1)

/**
 * One voxel. 6 bytes, packed, alignment 1.
 *
 * This is the interchange type: edit requests, script queries, mesher input.
 * It is NOT how chunks store voxels - see FMadChunkStorage in MadFallCore,
 * which is structure-of-arrays with a per-chunk palette. A dense array of this
 * struct would be 192 KiB per chunk and would spend most of it storing 32768
 * identical sky voxels.
 *
 * Why 6 bytes and not the 8 the budget allows: FMadVoxel[4] is 24 bytes, so a
 * 64-byte cache line holds 10.67 voxels instead of 8. The mesher walks voxels
 * linearly, so that is ~33% fewer cache lines on the hottest loop in the
 * engine. The two spare bytes would be speculative, so we do not take them.
 *
 * NOT a USTRUCT on purpose - reflection over a pack(1) struct is a trap.
 * Blueprint sees FMadVoxelState below instead.
 */
struct FMadVoxel
{
	/**
	 * Registry runtime ID. 0 = air, 65535 = unresolved.
	 * Session-local: the save file stores namespaced strings, never this value.
	 */
	uint16 BlockTypeID;

	/** Isosurface sample. 0 = fully outside, 255 = fully inside, 128 = the surface crossing. */
	uint8 Density;

	/** 0 = intact, 255 = destroyed-pending. Stage thresholds come from the block definition. */
	uint8 Damage;

	/** Bits 0-4: orientation index 0-23 (6 up-faces x 4 spins). Bits 5-7: shape variant 0-7. */
	uint8 Rotation;

	/** EMadVoxelFlags bitmask. */
	uint8 Flags;

	// --- helpers -----------------------------------------------------------

	/** Solid for meshing and collision purposes. */
	FORCEINLINE bool IsSolid() const { return Density >= 128; }

	FORCEINLINE bool IsAir() const { return BlockTypeID == MadFall::BlockTypeAir; }

	FORCEINLINE bool HasFlag(EMadVoxelFlags Flag) const
	{
		return (Flags & static_cast<uint8>(Flag)) != 0;
	}

	FORCEINLINE void SetFlag(EMadVoxelFlags Flag, bool bValue)
	{
		if (bValue)
		{
			Flags = static_cast<uint8>(Flags | static_cast<uint8>(Flag));
		}
		else
		{
			Flags = static_cast<uint8>(Flags & static_cast<uint8>(~static_cast<uint8>(Flag)));
		}
	}

	/** 0-23. Values 24-31 are reserved and read back as 0. */
	FORCEINLINE uint8 GetOrientation() const
	{
		const uint8 Raw = static_cast<uint8>(Rotation & 0x1F);
		return (Raw < 24) ? Raw : static_cast<uint8>(0);
	}

	FORCEINLINE void SetOrientation(uint8 Orientation)
	{
		checkSlow(Orientation < 24);
		Rotation = static_cast<uint8>((Rotation & 0xE0) | (Orientation & 0x1F));
	}

	/** 0-7. Selects among a block shape family (straight / corner / tee / ...). */
	FORCEINLINE uint8 GetShapeVariant() const
	{
		return static_cast<uint8>(Rotation >> 5);
	}

	FORCEINLINE void SetShapeVariant(uint8 Variant)
	{
		checkSlow(Variant < 8);
		Rotation = static_cast<uint8>((Rotation & 0x1F) | static_cast<uint8>(Variant << 5));
	}

	FORCEINLINE bool operator==(const FMadVoxel& Other) const
	{
		return BlockTypeID == Other.BlockTypeID
			&& Density == Other.Density
			&& Damage == Other.Damage
			&& Rotation == Other.Rotation
			&& Flags == Other.Flags;
	}

	FORCEINLINE bool operator!=(const FMadVoxel& Other) const
	{
		return !(*this == Other);
	}

	/** The default voxel: air, fully outside, undamaged, unrotated, no flags. */
	static FMadVoxel Air()
	{
		return FMadVoxel{ MadFall::BlockTypeAir, 0, 0, 0, 0 };
	}
};

#pragma pack(pop)

static_assert(sizeof(FMadVoxel) == 6, "FMadVoxel must stay 6 bytes - see docs/ARCHITECTURE.md section (b)");
static_assert(alignof(FMadVoxel) == 1, "FMadVoxel must pack into dense arrays without padding");
static_assert(offsetof(FMadVoxel, BlockTypeID) == 0, "FMadVoxel layout is part of the save format");
static_assert(offsetof(FMadVoxel, Density) == 2, "FMadVoxel layout is part of the save format");
static_assert(offsetof(FMadVoxel, Damage) == 3, "FMadVoxel layout is part of the save format");
static_assert(offsetof(FMadVoxel, Rotation) == 4, "FMadVoxel layout is part of the save format");
static_assert(offsetof(FMadVoxel, Flags) == 5, "FMadVoxel layout is part of the save format");

// ===========================================================================
// FMadVoxelState - the Blueprint-facing view
// ===========================================================================

/**
 * Unpacked, reflected view of a voxel for Blueprint and for script mods.
 *
 * Deliberately not the same type as FMadVoxel: designers should never see a
 * bit-packed rotation byte, and UHT should never see a packed struct. The
 * conversion happens at the Blueprint boundary.
 */
USTRUCT(BlueprintType)
struct MADFALLMODAPI_API FMadVoxelState
{
	GENERATED_BODY()

	/** Namespaced block id, e.g. "madfall:oak_log". NAME_None for air. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Voxel")
	FName BlockId;

	/** 0-255. 128 is the isosurface crossing. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Voxel", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Density = 0;

	/** 0 = intact, 255 = destroyed-pending. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Voxel", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Damage = 0;

	/** 0-23. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Voxel", meta = (ClampMin = "0", ClampMax = "23"))
	int32 Orientation = 0;

	/** 0-7. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Voxel", meta = (ClampMin = "0", ClampMax = "7"))
	int32 ShapeVariant = 0;

	/** True when this voxel is player-placed construction rather than natural terrain. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MadFall|Voxel")
	bool bCubic = false;

	/** Convenience mirror of FMadVoxel::IsSolid(). */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Voxel")
	bool bSolid = false;
};

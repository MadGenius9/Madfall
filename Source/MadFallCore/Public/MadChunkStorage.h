// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBitPackedArray.h"
#include "MadFallVoxelTypes.h"

/** One entry in a chunk's block palette. 8 bytes. */
struct FMadBlockPaletteEntry
{
	/** Session-local runtime block id. */
	uint16 RuntimeId = 0;

	uint16 Pad = 0;

	/** Voxels in this chunk currently using this entry. Zero means the slot is free. */
	uint32 RefCount = 0;
};

static_assert(sizeof(FMadBlockPaletteEntry) == 8, "Palette entry is sized for cache-friendly scanning");

/**
 * Voxel storage for one 32x32x32 chunk.
 *
 * Structure-of-arrays with a per-chunk block palette, NOT a dense
 * TArray<FMadVoxel>. A dense array is 192 KiB per chunk and spends most of it
 * storing 32768 identical sky voxels; at a 12-chunk radius that is ~117 MB of
 * voxels alone. This layout puts the same working set at roughly 16 MB.
 * See docs/ARCHITECTURE.md section (b) for the measured table.
 *
 * The four side arrays are allocated lazily. A null pointer means "every voxel
 * has the default value", which is why a freshly generated uniform chunk - air,
 * bedrock, deep stone, all of which are most of a world - costs about 64 bytes.
 *
 * THREAD SAFETY: this class has none of its own. The owning FMadChunk holds the
 * lock. Worker threads read under a read lock and never mutate; an edit builds
 * a modified copy and the game thread pointer-swaps it in. Doing it the other
 * way - locking inside every Get - would put an atomic on the mesher's inner
 * loop, which is the one place in the engine that cannot afford it.
 */
class MADFALLCORE_API FMadChunkStorage
{
public:
	/** A uniform chunk of air. */
	FMadChunkStorage();

	FMadChunkStorage(const FMadChunkStorage& Other);
	FMadChunkStorage& operator=(const FMadChunkStorage& Other);
	FMadChunkStorage(FMadChunkStorage&&) = default;
	FMadChunkStorage& operator=(FMadChunkStorage&&) = default;

	// --- reads -------------------------------------------------------------

	FMadVoxel GetVoxel(int32 Index) const;

	FMadVoxel GetVoxel(int32 X, int32 Y, int32 Z) const
	{
		return GetVoxel(MadFall::VoxelIndex(X, Y, Z));
	}

	/**
	 * What the mesher samples: block, density, flags - no damage, no rotation.
	 * GetVoxel also probes the sparse damage map, which for a 34^3 snapshot was
	 * 39304 hash lookups the mesher then threw away.
	 */
	FORCEINLINE void GetMeshSample(int32 Index, uint16& OutBlockId, uint8& OutDensity, uint8& OutFlags) const
	{
		OutBlockId = GetBlockId(Index);
		OutDensity = Density ? Density[Index] : DefaultDensity;
		OutFlags = Flags ? Flags[Index] : DefaultFlags;
	}

	/** Block runtime id only. Avoids touching the side arrays when that is all the caller wants. */
	FORCEINLINE uint16 GetBlockId(int32 Index) const
	{
		checkSlow(Index >= 0 && Index < MadFall::ChunkVoxelCount);
		const int32 PaletteIndex = Indices.IsEmpty() ? 0 : static_cast<int32>(Indices.Get(Index));
		return Palette[PaletteIndex].RuntimeId;
	}

	// --- writes ------------------------------------------------------------

	void SetVoxel(int32 Index, const FMadVoxel& Voxel);

	void SetVoxel(int32 X, int32 Y, int32 Z, const FMadVoxel& Voxel)
	{
		SetVoxel(MadFall::VoxelIndex(X, Y, Z), Voxel);
	}

	/** Resets to a uniform chunk of one voxel value. O(1) - does not touch 32768 entries. */
	void Fill(const FMadVoxel& Voxel);

	// --- shape -------------------------------------------------------------

	/** True when every voxel is identical and no per-voxel arrays are allocated. */
	bool IsUniform() const;

	/** Valid only when IsUniform(). */
	FMadVoxel GetUniformVoxel() const;

	/**
	 * Drops palette entries that no voxel references and re-tiers the index
	 * array down if that frees a bit width.
	 *
	 * Runs on the save path, never on the edit path: compacting mid-edit would
	 * renumber palette indices while a mesher job is mid-read.
	 */
	void Compact();

	// --- serializer access -------------------------------------------------
	// Deliberately not general-purpose mutators. MadChunkSerializer needs the
	// raw arrays to RLE them; nothing else should reach in here.

	const TArray<FMadBlockPaletteEntry>& GetPalette() const { return Palette; }
	TArray<FMadBlockPaletteEntry>& GetPaletteMutable() { return Palette; }

	const FMadBitPackedArray& GetIndices() const { return Indices; }
	FMadBitPackedArray& GetIndicesMutable() { return Indices; }

	const uint8* GetDensityArray() const { return Density.Get(); }
	const uint8* GetRotationArray() const { return Rotation.Get(); }
	const uint8* GetFlagsArray() const { return Flags.Get(); }

	uint8 GetDefaultDensity() const { return DefaultDensity; }
	uint8 GetDefaultRotation() const { return DefaultRotation; }
	uint8 GetDefaultFlags() const { return DefaultFlags; }

	void SetDefaults(uint8 InDensity, uint8 InRotation, uint8 InFlags)
	{
		DefaultDensity = InDensity;
		DefaultRotation = InRotation;
		DefaultFlags = InFlags;
	}

	/** Allocates a side array filled with its default, so the serializer can write into it. */
	uint8* GetOrCreateDensityArray() { return EnsureArray(Density, DefaultDensity); }
	uint8* GetOrCreateRotationArray() { return EnsureArray(Rotation, DefaultRotation); }
	uint8* GetOrCreateFlagsArray() { return EnsureArray(Flags, DefaultFlags); }

	void DiscardDensityArray() { Density.Reset(); }
	void DiscardRotationArray() { Rotation.Reset(); }
	void DiscardFlagsArray() { Flags.Reset(); }

	/** Sparse damage: voxel index -> damage byte. Absent means zero. */
	const TMap<int32, uint8>& GetDamageMap() const { return DamageMap; }
	TMap<int32, uint8>& GetDamageMapMutable() { return DamageMap; }

	/** Ensures the index array exists (rather than the uniform representation). */
	void MaterializeIndices();

	// --- diagnostics -------------------------------------------------------

	/** Bytes this chunk actually occupies, for the memory budget stat. */
	int64 GetAllocatedSize() const;

	/** Distinct block types currently referenced. */
	int32 GetDistinctBlockCount() const;

	/** Structural equality over every voxel. Used by the serialization round-trip tests. */
	bool EqualsVoxelwise(const FMadChunkStorage& Other) const;

private:
	/** Returns the palette slot for RuntimeId, adding one (and re-tiering) if needed. */
	int32 FindOrAddPaletteEntry(uint16 RuntimeId);

	uint8* EnsureArray(TUniquePtr<uint8[]>& Array, uint8 DefaultValue);

	static TUniquePtr<uint8[]> CloneArray(const TUniquePtr<uint8[]>& Source);

	/** Always at least one entry. Entry 0 is the chunk's majority block after a Compact. */
	TArray<FMadBlockPaletteEntry> Palette;

	/** Empty when the chunk is uniform; otherwise 32768 entries at the palette's bit tier. */
	FMadBitPackedArray Indices;

	/** Null means "every voxel has the matching Default* value below". */
	TUniquePtr<uint8[]> Density;
	TUniquePtr<uint8[]> Rotation;
	TUniquePtr<uint8[]> Flags;

	/**
	 * Sparse because damage is rare: an untouched chunk has none, and even a
	 * heavily fought-over base has a few hundred damaged voxels out of 32768.
	 * A dense uint8[32768] would be 32 KiB of zeros per chunk.
	 */
	TMap<int32, uint8> DamageMap;

	uint8 DefaultDensity = 0;
	uint8 DefaultRotation = 0;
	uint8 DefaultFlags = 0;
};

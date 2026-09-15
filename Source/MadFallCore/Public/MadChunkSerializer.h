// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IMadBlockRegistry.h"
#include "MadChunkStorage.h"

class FMadBlockRegistry;

/**
 * Chunk payload format, version 1.
 *
 * A chunk serializes to a sequence of tag-length-value sections, which is then
 * LZ4-compressed as a unit by the region writer. Sections a reader does not
 * recognise are skipped with a warning; a payload version newer than the reader
 * is refused outright rather than half-parsed.
 *
 * The block palette is written as indices into the REGION's string table, and
 * that table holds namespaced FNames like "mymod:rebar_concrete". Runtime uint16
 * ids never reach the disk. This is the single most important property of the
 * format: it is what lets a world survive a mod being removed, reordered, or
 * reinstalled.
 */
enum class EMadChunkSection : uint8
{
	Palette       = 1,
	BlockIndices  = 2,
	Density       = 3,
	Damage        = 4,
	Rotation      = 5,
	Flags         = 6,

	/** Phase 4. Containers, workstations, land claims. */
	BlockEntities = 7,

	/** Phase 4. Derived, dropped on any version mismatch. */
	StructuralCache = 8,

	/** Phase 5. Per-chunk key-value storage for script mods. */
	ModPayload    = 9,

	/** The chunk's default density/rotation/flags for voxels with no side array. */
	Defaults      = 10
};

/** Payload format version. Bump on any breaking change to the section layout. */
namespace MadFall
{
	inline constexpr uint8 ChunkPayloadVersion = 1;
}

/**
 * A chunk in its on-disk shape, before compression.
 *
 * A uniform chunk carries no payload at all: the region index entry has room
 * for its single block id and its three default bytes, so the most common chunk
 * in any world - sky, bedrock, deep stone - costs 16 bytes of index and zero
 * sectors. See docs/ARCHITECTURE.md section (d).
 */
struct MADFALLCORE_API FMadSerializedChunk
{
	bool bUniform = false;

	/** Valid when bUniform. */
	FName UniformBlockId;
	uint8 UniformDensity = 0;
	uint8 UniformRotation = 0;
	uint8 UniformFlags = 0;

	/** TLV section blob. Empty when bUniform. */
	TArray<uint8> Payload;
};

namespace MadFall::ChunkSerializer
{
	/** Maps a namespaced block id to its index in the region string table. */
	using FInternString = TFunctionRef<uint32(FName)>;

	/** Maps a region string table index back to a namespaced block id. */
	using FResolveString = TFunctionRef<FName(uint32)>;

	/**
	 * Serializes a chunk.
	 *
	 * Transient voxel flags (currently SupportDirty) are masked off, so a chunk
	 * that happened to be mid-solve when it was saved does not come back with
	 * stale work queued.
	 */
	MADFALLCORE_API void Serialize(
		const FMadChunkStorage& Chunk,
		const IMadBlockRegistry& Registry,
		FInternString InternString,
		FMadSerializedChunk& Out);

	/**
	 * Deserializes a chunk.
	 *
	 * Block ids no installed definition provides are given stable placeholder
	 * runtime ids by the registry and keep their original strings, so they
	 * round-trip unchanged. Returns false only for structurally broken data -
	 * a missing mod is not an error.
	 */
	MADFALLCORE_API bool Deserialize(
		const FMadSerializedChunk& In,
		FMadBlockRegistry& Registry,
		FResolveString ResolveString,
		FMadChunkStorage& OutChunk,
		FString& OutError);
}

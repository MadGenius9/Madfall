// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadChunkSerializer.h"
#include "MadFallCoordinates.h"

class IFileHandle;
class IMappedFileHandle;
class IMappedFileRegion;

namespace MadFall
{
	/** 'MFRG' */
	inline constexpr uint32 RegionMagic = 0x4752464D;

	/** 'MFCH' */
	inline constexpr uint32 ChunkMagic = 0x4843464D;

	inline constexpr uint16 RegionFormatVersion = 1;

	inline constexpr int32 RegionSectorBytes = 4096;
	inline constexpr int32 RegionHeaderBytes = 4096;
	inline constexpr int32 RegionIndexOffset = 4096;
	inline constexpr int32 RegionIndexEntryBytes = 16;
	inline constexpr int32 RegionIndexBytes = RegionChunkSlots * RegionIndexEntryBytes;   // 64 KiB

	/** Header (1 sector) + index (16 sectors). Payloads start here. */
	inline constexpr int32 RegionDataStartSector = (RegionHeaderBytes + RegionIndexBytes) / RegionSectorBytes;
	static_assert(RegionDataStartSector == 17, "Region layout assumes a 4 KiB header and a 64 KiB index");

	/** Bytes of the fixed prefix in front of every compressed chunk payload. */
	inline constexpr int32 ChunkPrefixBytes = 24;
}

/** Per-chunk flags in a region index entry. */
enum class EMadRegionEntryFlags : uint16
{
	None            = 0,

	/** The slot holds a chunk. Without this the entry is "never generated". */
	Present         = 1 << 0,

	/**
	 * Every voxel is identical, so there is NO payload: the entry's own fields
	 * carry the block id and the three default bytes. Sky, bedrock and deep
	 * stone are most of a world, and this is what makes them free.
	 */
	Uniform         = 1 << 1,

	PlayerModified  = 1 << 2,
	HasBlockEntities= 1 << 3,
	StructuralCache = 1 << 4
};
ENUM_CLASS_FLAGS(EMadRegionEntryFlags);

/** One 16-byte region index entry, in memory. */
struct FMadRegionIndexEntry
{
	/** Sector offset of the payload, or - when Uniform - the block id's string table index. */
	uint32 OffsetSectors = 0;

	/** Payload size including its prefix, or - when Uniform - packed { density, rotation, flags, 0 }. */
	uint32 PayloadBytes = 0;

	/** CRC32 of the payload bytes as written. Zero when Uniform. */
	uint32 PayloadCrc32 = 0;

	uint8 Compression = 0;
	uint8 ChunkVersion = 0;
	EMadRegionEntryFlags Flags = EMadRegionEntryFlags::None;

	bool IsPresent() const { return EnumHasAnyFlags(Flags, EMadRegionEntryFlags::Present); }
	bool IsUniform() const { return EnumHasAnyFlags(Flags, EMadRegionEntryFlags::Uniform); }
};

/**
 * One region file: 16 x 16 chunk columns across the full vertical range, so
 * 4096 chunk slots in a single file.
 *
 * Header and index live in memory for the file's whole open lifetime (68 KiB);
 * payloads are read through a memory mapping and written through a normal file
 * handle.
 *
 * WHY MAPPED READS BUT BUFFERED WRITES:
 * Chunk loads are scattered 2-6 KiB reads driven by player movement, which is
 * exactly what a mapping is good at - the OS page cache does the readahead and
 * no bytes are copied. Writes extend the file, and a Windows file mapping
 * cannot grow, so a write unmaps first and the next read re-maps lazily. Since
 * writes are batched at save time rather than per edit, that costs one unmap
 * per save, not one per block placed.
 *
 * DURABILITY
 *   - Copy-on-write payloads: a chunk's new payload goes to fresh sectors and
 *     the index entry changes only in memory.
 *   - Deferred frees: the sectors a rewrite gives up stay allocated until an
 *     index that no longer names them is on disk. Freed at once, the next chunk
 *     of the same save could take them, and a crash before the index flush would
 *     leave the on-disk index pointing at another chunk's bytes.
 *   - Double-written index: Flush writes the complete new header and index to a
 *     sidecar (`r.x.y.mfr.index`, with its own CRC) before writing them in
 *     place, and deletes the sidecar after. Open replays a valid sidecar, so a
 *     write torn half way through the 68 KiB index is repaired; an invalid one
 *     means the in-place write never began, and it is discarded.
 *   - CRCs on every payload, the header and index, and the string table.
 *   - UNCLEAN_SHUTDOWN is set by the first write and cleared by Close.
 *   - Off the game thread, and on Close, each step is flushed to the disk
 *     (FlushFileBuffers) before the next, which makes the order hold across
 *     power loss. Mid-play saves are all off the game thread (unloads and the
 *     autosave, UMadVoxelWorldSubsystem::SaveAllAsync). The one game-thread
 *     flush left is a synchronous SaveAll (`mad.save`, leaving a world), which
 *     skips it - a 5-20 ms stall per region - and is followed by Close when
 *     leaving, which does flush.
 *   - Compaction: Close rewrites a region that is more than 30% free space into
 *     a new file, then renames it over the old one.
 */
class MADFALLCORE_API FMadRegionFile
{
public:
	FMadRegionFile();
	~FMadRegionFile();

	FMadRegionFile(const FMadRegionFile&) = delete;
	FMadRegionFile& operator=(const FMadRegionFile&) = delete;

	/** "r.<x>.<y>.mfr" */
	static FString MakeFileName(const FMadRegionCoord& Coord);

	/**
	 * Opens (or creates) a region file.
	 *
	 * A seed or worldgen-version mismatch is reported through OutError and the
	 * file is not opened: silently mixing chunks generated by two different
	 * generators produces seams no player can explain.
	 */
	bool Open(const FString& FilePath, const FMadRegionCoord& Coord,
		uint64 WorldSeed, uint64 WorldGenVersion, bool bCreateIfMissing, FString& OutError);

	/** Flushes and closes. Clears the unclean-shutdown flag, and compacts a region that is mostly free space. */
	bool Close(FString& OutError);

	/** Rewrites the file with no free sectors, atomically. Open regions only. */
	bool Compact(FString& OutError);

	/** Close would compact: at least 64 free sectors, and 30% or more of the file. */
	bool ShouldCompact() const;

	/** True if the file was open for writing when the game last stopped. */
	bool WasUncleanOnOpen() const { return bWasUnclean; }

	/** True if Open replayed an interrupted index write from the sidecar. */
	bool RecoveredIndexOnOpen() const { return bRecoveredIndex; }

	/** Where Flush stops, to test recovery. Never set outside automation tests. */
	enum class ETestCrash : uint8
	{
		None,
		/** Payloads written, index untouched: the journal is never written. */
		BeforeJournal,
		/** Journal written, the in-place index write not begun. */
		AfterJournal,
		/** Half the in-place index written. */
		MidIndexWrite
	};
	static ETestCrash TestCrash;

	/** Drops the file without flushing, as a crash would. Tests only. */
	void AbandonForTest();

	bool IsOpen() const { return bOpen; }

	const FMadRegionCoord& GetCoord() const { return Coord; }

	/** True if the slot holds a chunk. */
	bool HasChunk(int32 Slot) const;

	/** Number of occupied slots. */
	int32 NumChunks() const;

	/** Reads and CRC-verifies a chunk. Returns false and fills OutError if absent or damaged. */
	bool ReadChunk(int32 Slot, FMadSerializedChunk& Out, FString& OutError);

	/** Writes a chunk. The index is not durable until Flush(). */
	bool WriteChunk(int32 Slot, const FMadSerializedChunk& In, bool bPlayerModified, FString& OutError);

	/** Writes the string table, then the header and index (double-written), to disk. */
	bool Flush(FString& OutError);

	/** Interns a block id into the region string table, returning its index. */
	uint32 InternString(FName Value);

	/** String table lookup. NAME_None if out of range. */
	FName ResolveString(uint32 StringIndex) const;

	/** Diagnostics for the `mad.region` console command. */
	FString Describe() const;

	int32 GetUsedSectors() const { return NextFreeSector; }
	int32 GetFreeSectorCount() const;

	/** Sectors given up by rewrites but still named by the index on disk. */
	int32 GetPendingFreeSectorCount() const;

	static FString GetJournalPath(const FString& RegionPath) { return RegionPath + TEXT(".index"); }

private:
	bool ReadHeaderAndIndex(FString& OutError);
	bool WriteHeaderAndIndex(FString& OutError);
	TArray<uint8> BuildHeaderAndIndex(const TArray<FMadRegionIndexEntry>& Entries, int32 StrTabSector, int32 StrTabSectorCount,
		uint32 StrTabCrc, int32 UsedSectors, int32 FreeSectorCount, bool bUnclean) const;
	bool RecoverJournal(FString& OutError);
	bool FlushFileToDisk(FString& OutError);
	bool ReadStringTable(FString& OutError);
	bool WriteStringTable(FString& OutError);

	/** Maps the whole file for reading, if it is not mapped already. */
	bool EnsureMapped();
	void Unmap();

	bool ReadBytes(int64 Offset, int64 Size, TArray<uint8>& Out, FString& OutError);
	bool WriteBytes(int64 Offset, const uint8* Data, int64 Size, FString& OutError, bool bToDisk = false);

	/** First-fit from the free list, else bump the high-water mark. */
	int32 AllocateSectors(int32 SectorCount);
	void FreeSectors(int32 StartSector, int32 SectorCount);

	/** Frees sectors once the index on disk stops naming them (see DURABILITY). */
	void ReleaseSectorsAfterFlush(int32 StartSector, int32 SectorCount) { if (SectorCount > 0) { PendingFrees.Add({ StartSector, SectorCount }); } }

	struct FFreeRun
	{
		int32 StartSector = 0;
		int32 SectorCount = 0;
	};

	FString FilePath;
	FMadRegionCoord Coord;

	uint64 WorldSeed = 0;
	uint64 WorldGenVersion = 0;

	TArray<FMadRegionIndexEntry> Index;

	TArray<FName> Strings;
	TMap<FName, uint32> StringToIndex;
	bool bStringTableDirty = false;

	int32 StringTableSector = 0;
	int32 StringTableSectorCount = 0;

	TArray<FFreeRun> FreeRuns;
	TArray<FFreeRun> PendingFrees;
	uint32 StringTableCrc = 0;
	int32 NextFreeSector = MadFall::RegionDataStartSector;

	TUniquePtr<IMappedFileHandle> MappedHandle;
	TUniquePtr<IMappedFileRegion> MappedRegion;

	bool bOpen = false;
	bool bIndexDirty = false;
	bool bClosing = false;

	/** The header on disk has UNCLEAN_SHUTDOWN set. */
	bool bUncleanOnDisk = false;
	bool bWasUnclean = false;
	bool bRecoveredIndex = false;
};

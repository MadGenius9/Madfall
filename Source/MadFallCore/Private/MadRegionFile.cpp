// Copyright MadFall. All Rights Reserved.

#include "MadRegionFile.h"

#include "Async/MappedFileHandle.h"
#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "MadFallCore.h"
#include "MadFallStats.h"
#include "Misc/Compression.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/StringBuilder.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

DECLARE_CYCLE_STAT(TEXT("Region ReadChunk"), STAT_MadRegionRead, STATGROUP_MadFallVoxel);
DECLARE_CYCLE_STAT(TEXT("Region WriteChunk"), STAT_MadRegionWrite, STATGROUP_MadFallVoxel);
DECLARE_CYCLE_STAT(TEXT("Region Flush"), STAT_MadRegionFlush, STATGROUP_MadFallVoxel);

FMadRegionFile::ETestCrash FMadRegionFile::TestCrash = FMadRegionFile::ETestCrash::None;

namespace
{
	constexpr uint32 RegionFlagUncleanShutdown = 1u << 0;

	/** 'MFIJ': the index journal sidecar. */
	constexpr uint32 IndexJournalMagic = 0x4A49464D;
	constexpr int32 IndexJournalPrefixBytes = 12;

	/** Where the header/index CRC lives, in the header's formerly reserved space. */
	constexpr int32 HeaderCrcOffset = 140;

	int32 SectorsFor(int64 Bytes)
	{
		return static_cast<int32>((Bytes + MadFall::RegionSectorBytes - 1) / MadFall::RegionSectorBytes);
	}
}

FMadRegionFile::FMadRegionFile() = default;

FMadRegionFile::~FMadRegionFile()
{
	if (bOpen)
	{
		FString Error;
		Close(Error);
	}
}

FString FMadRegionFile::MakeFileName(const FMadRegionCoord& InCoord)
{
	return FString::Printf(TEXT("r.%d.%d.mfr"), InCoord.X, InCoord.Y);
}

// ===========================================================================
// Open / close
// ===========================================================================

bool FMadRegionFile::Open(const FString& InFilePath, const FMadRegionCoord& InCoord,
	uint64 InWorldSeed, uint64 InWorldGenVersion, bool bCreateIfMissing, FString& OutError)
{
	check(!bOpen);

	FilePath = InFilePath;
	Coord = InCoord;
	WorldSeed = InWorldSeed;
	WorldGenVersion = InWorldGenVersion;

	Index.Reset();
	Index.SetNum(MadFall::RegionChunkSlots);
	Strings.Reset();
	StringToIndex.Reset();
	FreeRuns.Reset();
	PendingFrees.Reset();
	NextFreeSector = MadFall::RegionDataStartSector;
	StringTableSector = 0;
	StringTableSectorCount = 0;
	StringTableCrc = 0;
	bClosing = false;
	bUncleanOnDisk = false;
	bWasUnclean = false;
	bRecoveredIndex = false;

	// A compaction interrupted before its rename: the original is intact.
	IFileManager::Get().Delete(*(FilePath + TEXT(".compact")), false, true, true);

	if (!RecoverJournal(OutError))
	{
		return false;
	}

	const bool bExists = IFileManager::Get().FileExists(*FilePath);

	if (!bExists)
	{
		if (!bCreateIfMissing)
		{
			OutError = FString::Printf(TEXT("region file %s does not exist"), *FilePath);
			return false;
		}

		const FString Directory = FPaths::GetPath(FilePath);
		if (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
		{
			IFileManager::Get().MakeDirectory(*Directory, true);
		}

		bOpen = true;
		bIndexDirty = true;
		bStringTableDirty = true;

		if (!Flush(OutError))
		{
			bOpen = false;
			return false;
		}

		UE_LOG(LogMadFallVoxel, Verbose, TEXT("Created region %s at %s"), *Coord.ToString(), *FilePath);
		return true;
	}

	bOpen = true;

	if (!ReadHeaderAndIndex(OutError))
	{
		bOpen = false;
		return false;
	}

	if (!ReadStringTable(OutError))
	{
		bOpen = false;
		return false;
	}

	// Rebuild the free-space map from the index rather than storing it. 4096
	// entries is a ~30 us scan, and an allocator that cannot disagree with the
	// index is one fewer thing that can corrupt a world.
	struct FUsedRun { int32 Start; int32 Count; };
	TArray<FUsedRun> Used;

	for (const FMadRegionIndexEntry& Entry : Index)
	{
		if (Entry.IsPresent() && !Entry.IsUniform())
		{
			Used.Add({ static_cast<int32>(Entry.OffsetSectors), SectorsFor(Entry.PayloadBytes) });
		}
	}

	if (StringTableSectorCount > 0)
	{
		Used.Add({ StringTableSector, StringTableSectorCount });
	}

	Used.Sort([](const FUsedRun& A, const FUsedRun& B) { return A.Start < B.Start; });

	int32 Cursor = MadFall::RegionDataStartSector;
	for (const FUsedRun& Run : Used)
	{
		if (Run.Start > Cursor)
		{
			FreeRuns.Add({ Cursor, Run.Start - Cursor });
		}
		Cursor = FMath::Max(Cursor, Run.Start + Run.Count);
	}
	NextFreeSector = Cursor;

	UE_LOG(LogMadFallVoxel, Verbose,
		TEXT("Opened region %s: %d chunks, %d sectors used, %d free."),
		*Coord.ToString(), NumChunks(), NextFreeSector, GetFreeSectorCount());

	return true;
}

bool FMadRegionFile::Close(FString& OutError)
{
	if (!bOpen)
	{
		return true;
	}

	// Closing writes to the disk before returning, and clears UNCLEAN_SHUTDOWN.
	bClosing = true;
	bool bOk = Flush(OutError);

	if (bOk && ShouldCompact())
	{
		const int32 Before = NextFreeSector;
		FString CompactError;
		if (Compact(CompactError))
		{
			UE_LOG(LogMadFallVoxel, Log, TEXT("Compacted region %s: %d -> %d sectors."), *Coord.ToString(), Before, NextFreeSector);
		}
		else
		{
			// The original file is untouched; compaction is only an optimisation.
			UE_LOG(LogMadFallVoxel, Warning, TEXT("Could not compact region %s: %s"), *Coord.ToString(), *CompactError);
		}
	}

	Unmap();
	bOpen = false;
	bClosing = false;
	return bOk;
}

void FMadRegionFile::AbandonForTest()
{
	Unmap();
	bOpen = false;
}

bool FMadRegionFile::RecoverJournal(FString& OutError)
{
	const FString JournalPath = GetJournalPath(FilePath);
	if (!IFileManager::Get().FileExists(*JournalPath))
	{
		return true;
	}

	TArray<uint8> Journal;
	FFileHelper::LoadFileToArray(Journal, *JournalPath);
	const int32 ExpectedBytes = MadFall::RegionHeaderBytes + MadFall::RegionIndexBytes;
	bool bValid = Journal.Num() == IndexJournalPrefixBytes + ExpectedBytes;
	if (bValid)
	{
		uint32 Magic = 0;
		uint32 Bytes = 0;
		uint32 Crc = 0;
		FMemoryReader Reader(Journal);
		Reader << Magic << Bytes << Crc;
		bValid = Magic == IndexJournalMagic && Bytes == static_cast<uint32>(ExpectedBytes)
			&& Crc == FCrc::MemCrc32(Journal.GetData() + IndexJournalPrefixBytes, ExpectedBytes);
	}

	if (!bValid)
	{
		// Torn while the journal itself was being written: the in-place index had
		// not been touched yet, so the file is consistent without it.
		UE_LOG(LogMadFallVoxel, Log, TEXT("Discarding an incomplete index journal for %s."), *FilePath);
		IFileManager::Get().Delete(*JournalPath, false, true, true);
		return true;
	}

	// The in-place index write may have been torn: finish it from the journal.
	const FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}
	if (!WriteBytes(0, Journal.GetData() + IndexJournalPrefixBytes, ExpectedBytes, OutError, /*bToDisk*/ true))
	{
		return false;
	}
	IFileManager::Get().Delete(*JournalPath, false, true, true);
	bRecoveredIndex = true;
	UE_LOG(LogMadFallVoxel, Warning, TEXT("Region %s: recovered an interrupted index write from its journal."), *FilePath);
	return true;
}

// ===========================================================================
// Header / index
// ===========================================================================

bool FMadRegionFile::ReadHeaderAndIndex(FString& OutError)
{
	TArray<uint8> Bytes;
	if (!ReadBytes(0, MadFall::RegionHeaderBytes + MadFall::RegionIndexBytes, Bytes, OutError))
	{
		return false;
	}

	FMemoryReader Reader(Bytes);

	uint32 Magic = 0;
	uint16 FormatVersion = 0;
	uint16 HeaderBytes = 0;
	int32 RegionX = 0;
	int32 RegionY = 0;
	uint64 Seed = 0;
	uint64 GenVersion = 0;
	int64 LastWriteMs = 0;
	uint32 SectorBytes = 0;
	uint32 IndexOffset = 0;
	uint32 IndexEntryCount = 0;
	uint32 StrTabSector = 0;
	uint32 StrTabBytes = 0;
	uint32 StrTabCrc = 0;
	uint32 UsedSectors = 0;
	uint32 FreeSectors = 0;
	uint32 HeaderFlags = 0;

	Reader << Magic << FormatVersion << HeaderBytes;
	Reader << RegionX << RegionY;
	Reader << Seed << GenVersion << LastWriteMs;
	Reader << SectorBytes << IndexOffset << IndexEntryCount;
	Reader << StrTabSector << StrTabBytes << StrTabCrc;
	Reader << UsedSectors << FreeSectors << HeaderFlags;

	if (Magic != MadFall::RegionMagic)
	{
		OutError = FString::Printf(TEXT("%s is not a MadFall region file (magic 0x%08X)"), *FilePath, Magic);
		return false;
	}

	if (FormatVersion > MadFall::RegionFormatVersion)
	{
		// Refuse rather than best-effort: a newer file may have moved fields we
		// would then read as garbage and write back, destroying the world.
		OutError = FString::Printf(
			TEXT("%s is region format version %u; this build understands up to %u. Update the game."),
			*FilePath, FormatVersion, MadFall::RegionFormatVersion);
		return false;
	}

	if (RegionX != Coord.X || RegionY != Coord.Y)
	{
		OutError = FString::Printf(TEXT("%s claims to be region (%d, %d) but was opened as %s"),
			*FilePath, RegionX, RegionY, *Coord.ToString());
		return false;
	}

	if (WorldSeed != 0 && Seed != WorldSeed)
	{
		OutError = FString::Printf(
			TEXT("%s was generated with seed %llu but this world uses %llu"), *FilePath, Seed, WorldSeed);
		return false;
	}

	if (IndexEntryCount != static_cast<uint32>(MadFall::RegionChunkSlots))
	{
		OutError = FString::Printf(TEXT("%s has %u index entries, expected %d"),
			*FilePath, IndexEntryCount, MadFall::RegionChunkSlots);
		return false;
	}

	// Files from before the header CRC have zero here and are not checked.
	uint32 StoredHeaderCrc = 0;
	FMemory::Memcpy(&StoredHeaderCrc, Bytes.GetData() + HeaderCrcOffset, sizeof(uint32));
	if (StoredHeaderCrc != 0)
	{
		FMemory::Memzero(Bytes.GetData() + HeaderCrcOffset, sizeof(uint32));
		const uint32 ActualHeaderCrc = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
		if (ActualHeaderCrc != StoredHeaderCrc)
		{
			OutError = FString::Printf(
				TEXT("%s: header and index failed their CRC check (stored 0x%08X, computed 0x%08X) and no journal could repair them; restore a world backup"),
				*FilePath, StoredHeaderCrc, ActualHeaderCrc);
			return false;
		}
	}

	if ((HeaderFlags & RegionFlagUncleanShutdown) != 0)
	{
		bWasUnclean = true;
		bUncleanOnDisk = true;
		UE_LOG(LogMadFallVoxel, Warning,
			TEXT("Region %s was not closed cleanly%s. Every chunk read from it is CRC-checked; damaged chunks will be reported individually."),
			*FilePath, bRecoveredIndex ? TEXT(" (its index was recovered from the journal)") : TEXT(""));
	}

	WorldGenVersion = GenVersion;
	StringTableSector = static_cast<int32>(StrTabSector);
	StringTableSectorCount = SectorsFor(StrTabBytes);
	StringTableCrc = StrTabCrc;

	// --- index ---
	Reader.Seek(MadFall::RegionIndexOffset);

	for (int32 Slot = 0; Slot < MadFall::RegionChunkSlots; ++Slot)
	{
		FMadRegionIndexEntry& Entry = Index[Slot];

		uint16 RawFlags = 0;
		Reader << Entry.OffsetSectors;
		Reader << Entry.PayloadBytes;
		Reader << Entry.PayloadCrc32;
		Reader << Entry.Compression;
		Reader << Entry.ChunkVersion;
		Reader << RawFlags;

		Entry.Flags = static_cast<EMadRegionEntryFlags>(RawFlags);
	}

	return true;
}

TArray<uint8> FMadRegionFile::BuildHeaderAndIndex(const TArray<FMadRegionIndexEntry>& Entries, int32 StrTabSectorIn, int32 StrTabSectorCountIn,
	uint32 StrTabCrcIn, int32 UsedSectorsIn, int32 FreeSectorCountIn, bool bUnclean) const
{
	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(MadFall::RegionHeaderBytes + MadFall::RegionIndexBytes);

	{
		FMemoryWriter Writer(Bytes);

		uint32 Magic = MadFall::RegionMagic;
		uint16 FormatVersion = MadFall::RegionFormatVersion;
		uint16 HeaderBytes = static_cast<uint16>(MadFall::RegionHeaderBytes);
		int32 RegionX = Coord.X;
		int32 RegionY = Coord.Y;
		uint64 Seed = WorldSeed;
		uint64 GenVersion = WorldGenVersion;
		int64 LastWriteMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
		uint32 SectorBytes = MadFall::RegionSectorBytes;
		uint32 IndexOffset = MadFall::RegionIndexOffset;
		uint32 IndexEntryCount = MadFall::RegionChunkSlots;
		uint32 StrTabSector = static_cast<uint32>(StrTabSectorIn);
		uint32 StrTabBytes = static_cast<uint32>(StrTabSectorCountIn * MadFall::RegionSectorBytes);
		uint32 StrTabCrc = StrTabCrcIn;
		uint32 UsedSectors = static_cast<uint32>(UsedSectorsIn);
		uint32 FreeSectorsOut = static_cast<uint32>(FreeSectorCountIn);
		uint32 HeaderFlags = bUnclean ? RegionFlagUncleanShutdown : 0;

		Writer << Magic << FormatVersion << HeaderBytes;
		Writer << RegionX << RegionY;
		Writer << Seed << GenVersion << LastWriteMs;
		Writer << SectorBytes << IndexOffset << IndexEntryCount;
		Writer << StrTabSector << StrTabBytes << StrTabCrc;
		Writer << UsedSectors << FreeSectorsOut << HeaderFlags;

		Writer.Seek(MadFall::RegionIndexOffset);

		for (int32 Slot = 0; Slot < MadFall::RegionChunkSlots; ++Slot)
		{
			const FMadRegionIndexEntry& Entry = Entries[Slot];

			uint32 Offset = Entry.OffsetSectors;
			uint32 PayloadBytes = Entry.PayloadBytes;
			uint32 Crc = Entry.PayloadCrc32;
			uint8 Compression = Entry.Compression;
			uint8 ChunkVersion = Entry.ChunkVersion;
			uint16 RawFlags = static_cast<uint16>(Entry.Flags);

			Writer << Offset << PayloadBytes << Crc << Compression << ChunkVersion << RawFlags;
		}
	}

	const uint32 HeaderCrc = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
	FMemory::Memcpy(Bytes.GetData() + HeaderCrcOffset, &HeaderCrc, sizeof(uint32));
	return Bytes;
}

bool FMadRegionFile::WriteHeaderAndIndex(FString& OutError)
{
	// Off the game thread and when closing, every step reaches the disk before
	// the next starts (see DURABILITY in the header).
	const bool bToDisk = bClosing || !IsInGameThread();
	const bool bUnclean = !bClosing;

	const TArray<uint8> Bytes = BuildHeaderAndIndex(Index, StringTableSector, StringTableSectorCount, StringTableCrc,
		NextFreeSector, GetFreeSectorCount(), bUnclean);

	if (TestCrash == ETestCrash::BeforeJournal)
	{
		OutError = TEXT("test crash before the journal");
		return false;
	}

	// The payloads and string table the new index names must be durable before
	// anything that names them is.
	if (bToDisk && !FlushFileToDisk(OutError))
	{
		return false;
	}

	// 1. The journal: the complete new header and index, with their own CRC.
	const FString JournalPath = GetJournalPath(FilePath);
	{
		TArray<uint8> Journal;
		Journal.Reserve(IndexJournalPrefixBytes + Bytes.Num());
		FMemoryWriter Writer(Journal);
		uint32 Magic = IndexJournalMagic;
		uint32 Size = static_cast<uint32>(Bytes.Num());
		uint32 Crc = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
		Writer << Magic << Size << Crc;
		Writer.Serialize(const_cast<uint8*>(Bytes.GetData()), Bytes.Num());

		TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*JournalPath, /*bAppend*/ false, /*bAllowRead*/ false));
		if (!Handle.IsValid() || !Handle->Write(Journal.GetData(), Journal.Num()) || !Handle->Flush(bToDisk))
		{
			OutError = FString::Printf(TEXT("could not write the index journal %s"), *JournalPath);
			return false;
		}
	}

	if (TestCrash == ETestCrash::AfterJournal)
	{
		OutError = TEXT("test crash after the journal");
		return false;
	}
	if (TestCrash == ETestCrash::MidIndexWrite)
	{
		WriteBytes(0, Bytes.GetData(), Bytes.Num() / 2, OutError);
		OutError = TEXT("test crash half way through the index");
		return false;
	}

	// 2. In place.
	if (!WriteBytes(0, Bytes.GetData(), Bytes.Num(), OutError, bToDisk))
	{
		return false;
	}

	// 3. Done: the journal goes, and sectors the old index named are free at last.
	IFileManager::Get().Delete(*JournalPath, false, true, true);
	for (const FFreeRun& Run : PendingFrees)
	{
		FreeSectors(Run.StartSector, Run.SectorCount);
	}
	PendingFrees.Reset();

	bUncleanOnDisk = bUnclean;
	bIndexDirty = false;
	return true;
}

// ===========================================================================
// String table
// ===========================================================================

uint32 FMadRegionFile::InternString(FName Value)
{
	if (const uint32* Found = StringToIndex.Find(Value))
	{
		return *Found;
	}

	const uint32 NewIndex = static_cast<uint32>(Strings.Num());
	Strings.Add(Value);
	StringToIndex.Add(Value, NewIndex);
	bStringTableDirty = true;

	return NewIndex;
}

FName FMadRegionFile::ResolveString(uint32 StringIndex) const
{
	return Strings.IsValidIndex(static_cast<int32>(StringIndex)) ? Strings[static_cast<int32>(StringIndex)] : NAME_None;
}

bool FMadRegionFile::ReadStringTable(FString& OutError)
{
	if (StringTableSectorCount == 0)
	{
		return true;
	}

	TArray<uint8> Bytes;
	if (!ReadBytes(static_cast<int64>(StringTableSector) * MadFall::RegionSectorBytes,
		static_cast<int64>(StringTableSectorCount) * MadFall::RegionSectorBytes, Bytes, OutError))
	{
		return false;
	}

	// Zero in files from before the string table was CRC'd.
	if (StringTableCrc != 0 && FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()) != StringTableCrc)
	{
		OutError = FString::Printf(TEXT("%s: the string table failed its CRC check; restore a world backup"), *FilePath);
		return false;
	}

	FMemoryReader Reader(Bytes);

	uint32 Count = 0;
	Reader << Count;

	if (Count > static_cast<uint32>(MadFall::RegionChunkSlots) * 64u)
	{
		OutError = FString::Printf(TEXT("%s declares an implausible string table of %u entries"), *FilePath, Count);
		return false;
	}

	Strings.Reset(Count);
	StringToIndex.Reset();

	for (uint32 Entry = 0; Entry < Count; ++Entry)
	{
		uint16 Length = 0;
		Reader << Length;

		if (Reader.Tell() + Length > Reader.TotalSize())
		{
			OutError = FString::Printf(TEXT("%s string table entry %u runs past the end of its sectors"), *FilePath, Entry);
			return false;
		}

		TArray<uint8> Utf8;
		Utf8.SetNumUninitialized(Length + 1);
		Reader.Serialize(Utf8.GetData(), Length);
		Utf8[Length] = 0;

		const FString Text(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Utf8.GetData())));
		const FName Name(*Text);

		StringToIndex.Add(Name, static_cast<uint32>(Strings.Num()));
		Strings.Add(Name);
	}

	return true;
}

bool FMadRegionFile::WriteStringTable(FString& OutError)
{
	TArray<uint8> Bytes;
	{
		FMemoryWriter Writer(Bytes);

		uint32 Count = static_cast<uint32>(Strings.Num());
		Writer << Count;

		for (const FName& Name : Strings)
		{
			const FString Text = Name.ToString();
			const FTCHARToUTF8 Utf8(*Text);

			uint16 Length = static_cast<uint16>(Utf8.Length());
			Writer << Length;
			Writer.Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
		}
	}

	const int32 NeededSectors = FMath::Max(SectorsFor(Bytes.Num()), 1);

	// Always relocate rather than growing in place. The old table stays intact
	// on disk until the header points elsewhere, so a crash mid-write leaves the
	// previous, consistent table.
	const int32 OldSector = StringTableSector;
	const int32 OldSectorCount = StringTableSectorCount;

	const int32 NewSector = AllocateSectors(NeededSectors);

	TArray<uint8> Padded = MoveTemp(Bytes);
	Padded.SetNumZeroed(NeededSectors * MadFall::RegionSectorBytes);

	if (!WriteBytes(static_cast<int64>(NewSector) * MadFall::RegionSectorBytes,
		Padded.GetData(), Padded.Num(), OutError))
	{
		FreeSectors(NewSector, NeededSectors);
		return false;
	}

	StringTableSector = NewSector;
	StringTableSectorCount = NeededSectors;
	StringTableCrc = FCrc::MemCrc32(Padded.GetData(), Padded.Num());

	// The header on disk still points at the old table until the next index write.
	ReleaseSectorsAfterFlush(OldSector, OldSectorCount);

	bStringTableDirty = false;
	bIndexDirty = true;
	return true;
}

// ===========================================================================
// Chunks
// ===========================================================================

bool FMadRegionFile::HasChunk(int32 Slot) const
{
	return Index.IsValidIndex(Slot) && Index[Slot].IsPresent();
}

int32 FMadRegionFile::NumChunks() const
{
	int32 Count = 0;
	for (const FMadRegionIndexEntry& Entry : Index)
	{
		if (Entry.IsPresent())
		{
			++Count;
		}
	}
	return Count;
}

bool FMadRegionFile::ReadChunk(int32 Slot, FMadSerializedChunk& Out, FString& OutError)
{
	SCOPE_CYCLE_COUNTER(STAT_MadRegionRead);

	if (!Index.IsValidIndex(Slot))
	{
		OutError = FString::Printf(TEXT("slot %d is out of range"), Slot);
		return false;
	}

	const FMadRegionIndexEntry& Entry = Index[Slot];

	if (!Entry.IsPresent())
	{
		OutError = FString::Printf(TEXT("slot %d has never been generated"), Slot);
		return false;
	}

	Out = FMadSerializedChunk();

	if (Entry.IsUniform())
	{
		Out.bUniform = true;
		Out.UniformBlockId = ResolveString(Entry.OffsetSectors);
		Out.UniformDensity = static_cast<uint8>(Entry.PayloadBytes & 0xFF);
		Out.UniformRotation = static_cast<uint8>((Entry.PayloadBytes >> 8) & 0xFF);
		Out.UniformFlags = static_cast<uint8>((Entry.PayloadBytes >> 16) & 0xFF);

		if (Out.UniformBlockId.IsNone())
		{
			OutError = FString::Printf(
				TEXT("slot %d is a uniform chunk referencing string table entry %u, which does not exist"),
				Slot, Entry.OffsetSectors);
			return false;
		}

		return true;
	}

	TArray<uint8> Raw;
	if (!ReadBytes(static_cast<int64>(Entry.OffsetSectors) * MadFall::RegionSectorBytes,
		Entry.PayloadBytes, Raw, OutError))
	{
		return false;
	}

	const uint32 ActualCrc = FCrc::MemCrc32(Raw.GetData(), Raw.Num());
	if (ActualCrc != Entry.PayloadCrc32)
	{
		// Named precisely so a corrupt world can be quarantined chunk by chunk
		// rather than losing the whole region.
		OutError = FString::Printf(
			TEXT("slot %d failed its CRC check (stored 0x%08X, computed 0x%08X) - %d bytes at sector %u of %s"),
			Slot, Entry.PayloadCrc32, ActualCrc, Raw.Num(), Entry.OffsetSectors, *FilePath);
		return false;
	}

	FMemoryReader PrefixReader(Raw);

	uint32 Magic = 0;
	uint8 Version = 0;
	uint8 Compression = 0;
	uint16 Reserved = 0;
	uint32 UncompressedBytes = 0;
	int64 LastModifiedMs = 0;
	uint32 Reserved2 = 0;

	PrefixReader << Magic << Version << Compression << Reserved << UncompressedBytes << LastModifiedMs << Reserved2;
	check(PrefixReader.Tell() == MadFall::ChunkPrefixBytes);

	if (Magic != MadFall::ChunkMagic)
	{
		OutError = FString::Printf(TEXT("slot %d has a bad chunk magic (0x%08X)"), Slot, Magic);
		return false;
	}

	if (Version > MadFall::ChunkPayloadVersion)
	{
		OutError = FString::Printf(
			TEXT("slot %d is chunk payload version %u; this build understands up to %u"),
			Slot, Version, MadFall::ChunkPayloadVersion);
		return false;
	}

	const uint8* CompressedData = Raw.GetData() + MadFall::ChunkPrefixBytes;
	const int32 CompressedSize = Raw.Num() - MadFall::ChunkPrefixBytes;

	if (Compression == 0)
	{
		Out.Payload.Append(CompressedData, CompressedSize);
	}
	else
	{
		Out.Payload.SetNumUninitialized(UncompressedBytes);
		if (!FCompression::UncompressMemory(NAME_LZ4, Out.Payload.GetData(), UncompressedBytes,
			CompressedData, CompressedSize))
		{
			OutError = FString::Printf(TEXT("slot %d failed LZ4 decompression (%d -> %u bytes)"),
				Slot, CompressedSize, UncompressedBytes);
			return false;
		}
	}

	return true;
}

bool FMadRegionFile::WriteChunk(int32 Slot, const FMadSerializedChunk& In, bool bPlayerModified, FString& OutError)
{
	SCOPE_CYCLE_COUNTER(STAT_MadRegionWrite);

	if (!Index.IsValidIndex(Slot))
	{
		OutError = FString::Printf(TEXT("slot %d is out of range"), Slot);
		return false;
	}

	FMadRegionIndexEntry& Entry = Index[Slot];

	const int32 OldSector = Entry.IsPresent() && !Entry.IsUniform() ? static_cast<int32>(Entry.OffsetSectors) : 0;
	const int32 OldSectorCount = OldSector != 0 ? SectorsFor(Entry.PayloadBytes) : 0;

	EMadRegionEntryFlags NewFlags = EMadRegionEntryFlags::Present;
	if (bPlayerModified)
	{
		NewFlags |= EMadRegionEntryFlags::PlayerModified;
	}

	// --- uniform: no payload at all ---
	if (In.bUniform)
	{
		const uint32 StringIndex = InternString(In.UniformBlockId);

		Entry.OffsetSectors = StringIndex;
		Entry.PayloadBytes = static_cast<uint32>(In.UniformDensity)
			| (static_cast<uint32>(In.UniformRotation) << 8)
			| (static_cast<uint32>(In.UniformFlags) << 16);
		Entry.PayloadCrc32 = 0;
		Entry.Compression = 0;
		Entry.ChunkVersion = MadFall::ChunkPayloadVersion;
		Entry.Flags = NewFlags | EMadRegionEntryFlags::Uniform;

		ReleaseSectorsAfterFlush(OldSector, OldSectorCount);

		bIndexDirty = true;
		return true;
	}

	// --- compress ---
	int32 CompressedBound = FCompression::CompressMemoryBound(NAME_LZ4, In.Payload.Num());
	TArray<uint8> Compressed;
	Compressed.SetNumUninitialized(CompressedBound);

	int32 CompressedSize = CompressedBound;
	uint8 CompressionId = 1;

	if (!FCompression::CompressMemory(NAME_LZ4, Compressed.GetData(), CompressedSize,
		In.Payload.GetData(), In.Payload.Num()))
	{
		// Storing it raw is strictly better than failing the save. The payload
		// is self-describing and the prefix records which path was taken.
		UE_LOG(LogMadFallVoxel, Warning,
			TEXT("LZ4 compression failed for slot %d; storing the chunk uncompressed."), Slot);
		Compressed = In.Payload;
		CompressedSize = In.Payload.Num();
		CompressionId = 0;
	}
	else
	{
		Compressed.SetNum(CompressedSize, EAllowShrinking::No);
	}

	// --- assemble prefix + body ---
	TArray<uint8> Raw;
	Raw.Reserve(MadFall::ChunkPrefixBytes + CompressedSize);
	{
		FMemoryWriter Writer(Raw);

		uint32 Magic = MadFall::ChunkMagic;
		uint8 Version = MadFall::ChunkPayloadVersion;
		uint8 Compression = CompressionId;
		uint16 Reserved = 0;
		uint32 UncompressedBytes = static_cast<uint32>(In.Payload.Num());
		int64 LastModifiedMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
		uint32 Reserved2 = 0;

		Writer << Magic << Version << Compression << Reserved << UncompressedBytes << LastModifiedMs << Reserved2;
		check(Writer.Tell() == MadFall::ChunkPrefixBytes);
		Writer.Serialize(Compressed.GetData(), CompressedSize);
	}

	check(Raw.Num() == MadFall::ChunkPrefixBytes + CompressedSize);

	const int32 NeededSectors = SectorsFor(Raw.Num());

	// Always allocate fresh sectors. Overwriting the live payload in place would
	// mean a crash mid-write destroys the only copy; this way the worst case is
	// losing the newest version of one chunk.
	const int32 NewSector = AllocateSectors(NeededSectors);

	TArray<uint8> Padded = MoveTemp(Raw);
	const int32 UnpaddedSize = Padded.Num();
	Padded.SetNumZeroed(NeededSectors * MadFall::RegionSectorBytes);

	if (!WriteBytes(static_cast<int64>(NewSector) * MadFall::RegionSectorBytes,
		Padded.GetData(), Padded.Num(), OutError))
	{
		FreeSectors(NewSector, NeededSectors);
		return false;
	}

	Entry.OffsetSectors = static_cast<uint32>(NewSector);
	Entry.PayloadBytes = static_cast<uint32>(UnpaddedSize);
	Entry.PayloadCrc32 = FCrc::MemCrc32(Padded.GetData(), UnpaddedSize);
	Entry.Compression = CompressionId;
	Entry.ChunkVersion = MadFall::ChunkPayloadVersion;
	Entry.Flags = NewFlags;

	// Not free yet: the index on disk still names these sectors.
	ReleaseSectorsAfterFlush(OldSector, OldSectorCount);

	bIndexDirty = true;
	return true;
}

bool FMadRegionFile::Flush(FString& OutError)
{
	SCOPE_CYCLE_COUNTER(STAT_MadRegionFlush);

	if (!bOpen)
	{
		OutError = TEXT("region is not open");
		return false;
	}

	if (bStringTableDirty && !WriteStringTable(OutError))
	{
		return false;
	}

	// The index is written last on purpose: it is the pointer to everything
	// else, so it must never name a sector whose contents are not yet on disk.
	if ((bIndexDirty || (bClosing && bUncleanOnDisk)) && !WriteHeaderAndIndex(OutError))
	{
		return false;
	}

	return true;
}

// ===========================================================================
// Sector allocation
// ===========================================================================

int32 FMadRegionFile::AllocateSectors(int32 SectorCount)
{
	check(SectorCount > 0);

	for (int32 RunIndex = 0; RunIndex < FreeRuns.Num(); ++RunIndex)
	{
		FFreeRun& Run = FreeRuns[RunIndex];
		if (Run.SectorCount >= SectorCount)
		{
			const int32 Start = Run.StartSector;
			Run.StartSector += SectorCount;
			Run.SectorCount -= SectorCount;

			if (Run.SectorCount == 0)
			{
				FreeRuns.RemoveAt(RunIndex);
			}

			return Start;
		}
	}

	const int32 Start = NextFreeSector;
	NextFreeSector += SectorCount;
	return Start;
}

void FMadRegionFile::FreeSectors(int32 StartSector, int32 SectorCount)
{
	if (SectorCount <= 0)
	{
		return;
	}

	FreeRuns.Add({ StartSector, SectorCount });
	FreeRuns.Sort([](const FFreeRun& A, const FFreeRun& B) { return A.StartSector < B.StartSector; });

	// Coalesce, so a region that is rewritten many times does not accumulate
	// thousands of one-sector holes that no future payload can use.
	for (int32 RunIndex = FreeRuns.Num() - 1; RunIndex > 0; --RunIndex)
	{
		FFreeRun& Current = FreeRuns[RunIndex];
		FFreeRun& Previous = FreeRuns[RunIndex - 1];

		if (Previous.StartSector + Previous.SectorCount == Current.StartSector)
		{
			Previous.SectorCount += Current.SectorCount;
			FreeRuns.RemoveAt(RunIndex);
		}
	}
}

int32 FMadRegionFile::GetPendingFreeSectorCount() const
{
	int32 Count = 0;
	for (const FFreeRun& Run : PendingFrees)
	{
		Count += Run.SectorCount;
	}
	return Count;
}

bool FMadRegionFile::ShouldCompact() const
{
	const int32 Free = GetFreeSectorCount() + GetPendingFreeSectorCount();
	return Free >= 64 && Free * 10 >= NextFreeSector * 3;
}

bool FMadRegionFile::Compact(FString& OutError)
{
	if (!bOpen)
	{
		OutError = TEXT("region is not open");
		return false;
	}
	if (!Flush(OutError))
	{
		return false;
	}

	// Payloads in their current order, so the copy reads the old file front to back.
	TArray<int32> Slots;
	for (int32 Slot = 0; Slot < Index.Num(); ++Slot)
	{
		if (Index[Slot].IsPresent() && !Index[Slot].IsUniform())
		{
			Slots.Add(Slot);
		}
	}
	Slots.Sort([this](int32 A, int32 B) { return Index[A].OffsetSectors < Index[B].OffsetSectors; });

	const FString TempPath = FilePath + TEXT(".compact");
	TArray<FMadRegionIndexEntry> NewIndex = Index;
	int32 Cursor = MadFall::RegionDataStartSector;
	int32 NewStringSector = 0;
	{
		TUniquePtr<IFileHandle> Out(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TempPath, /*bAppend*/ false, /*bAllowRead*/ false));
		if (!Out.IsValid())
		{
			OutError = FString::Printf(TEXT("could not create %s"), *TempPath);
			return false;
		}

		auto CopySectors = [&](int32 FromSector, int32 Count) -> bool
		{
			TArray<uint8> Bytes;
			if (!ReadBytes(static_cast<int64>(FromSector) * MadFall::RegionSectorBytes, static_cast<int64>(Count) * MadFall::RegionSectorBytes, Bytes, OutError))
			{
				return false;
			}
			if (!Out->Seek(static_cast<int64>(Cursor) * MadFall::RegionSectorBytes) || !Out->Write(Bytes.GetData(), Bytes.Num()))
			{
				OutError = FString::Printf(TEXT("could not write %s"), *TempPath);
				return false;
			}
			Cursor += Count;
			return true;
		};

		for (const int32 Slot : Slots)
		{
			const int32 From = static_cast<int32>(Index[Slot].OffsetSectors);
			NewIndex[Slot].OffsetSectors = static_cast<uint32>(Cursor);
			if (!CopySectors(From, SectorsFor(Index[Slot].PayloadBytes)))
			{
				Out.Reset();
				IFileManager::Get().Delete(*TempPath, false, true, true);
				return false;
			}
		}
		if (StringTableSectorCount > 0)
		{
			NewStringSector = Cursor;
			if (!CopySectors(StringTableSector, StringTableSectorCount))
			{
				Out.Reset();
				IFileManager::Get().Delete(*TempPath, false, true, true);
				return false;
			}
		}

		// Header last, clean: a file with a header is a complete file.
		const TArray<uint8> Header = BuildHeaderAndIndex(NewIndex, NewStringSector, StringTableSectorCount, StringTableCrc, Cursor, 0, /*bUnclean*/ false);
		if (!Out->Seek(0) || !Out->Write(Header.GetData(), Header.Num()) || !Out->Flush(/*bFullFlush*/ true))
		{
			Out.Reset();
			IFileManager::Get().Delete(*TempPath, false, true, true);
			OutError = FString::Printf(TEXT("could not write %s"), *TempPath);
			return false;
		}
	}

	// The swap. Until the rename the old file is the region; after, the new one.
	Unmap();
	if (!IFileManager::Get().Move(*FilePath, *TempPath, /*Replace*/ true, /*EvenIfReadOnly*/ true))
	{
		IFileManager::Get().Delete(*TempPath, false, true, true);
		OutError = FString::Printf(TEXT("could not replace %s with its compacted copy"), *FilePath);
		return false;
	}

	Index = MoveTemp(NewIndex);
	StringTableSector = NewStringSector;
	NextFreeSector = Cursor;
	FreeRuns.Reset();
	PendingFrees.Reset();
	bUncleanOnDisk = false;
	bIndexDirty = false;
	return true;
}

int32 FMadRegionFile::GetFreeSectorCount() const
{
	int32 Count = 0;
	for (const FFreeRun& Run : FreeRuns)
	{
		Count += Run.SectorCount;
	}
	return Count;
}

// ===========================================================================
// I/O
// ===========================================================================

bool FMadRegionFile::EnsureMapped()
{
	if (MappedRegion.IsValid())
	{
		return true;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FOpenMappedResult Result = PlatformFile.OpenMappedEx2(*FilePath);

	if (Result.HasError())
	{
		return false;
	}

	MappedHandle = Result.StealValue();
	if (!MappedHandle.IsValid())
	{
		return false;
	}

	MappedRegion.Reset(MappedHandle->MapRegion(0, MappedHandle->GetFileSize()));
	return MappedRegion.IsValid();
}

void FMadRegionFile::Unmap()
{
	MappedRegion.Reset();
	MappedHandle.Reset();
}

bool FMadRegionFile::ReadBytes(int64 Offset, int64 Size, TArray<uint8>& Out, FString& OutError)
{
	if (Size <= 0)
	{
		Out.Reset();
		return true;
	}

	if (EnsureMapped())
	{
		const int64 MappedSize = MappedRegion->GetMappedSize();
		if (Offset + Size <= MappedSize)
		{
			Out.SetNumUninitialized(Size);
			FMemory::Memcpy(Out.GetData(), MappedRegion->GetMappedPtr() + Offset, Size);
			return true;
		}

		OutError = FString::Printf(
			TEXT("%s: read of %lld bytes at %lld runs past the end of the file (%lld bytes)"),
			*FilePath, Size, Offset, MappedSize);
		return false;
	}

	// Mapping can fail for ordinary reasons - the file was just created and is
	// still zero-length, or the platform declines. Buffered read is the fallback,
	// not an error path.
	TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*FilePath));
	if (!Handle.IsValid())
	{
		OutError = FString::Printf(TEXT("could not open %s for reading"), *FilePath);
		return false;
	}

	if (Offset + Size > Handle->Size())
	{
		OutError = FString::Printf(
			TEXT("%s: read of %lld bytes at %lld runs past the end of the file (%lld bytes)"),
			*FilePath, Size, Offset, Handle->Size());
		return false;
	}

	Out.SetNumUninitialized(Size);
	if (!Handle->Seek(Offset) || !Handle->Read(Out.GetData(), Size))
	{
		OutError = FString::Printf(TEXT("%s: failed to read %lld bytes at %lld"), *FilePath, Size, Offset);
		return false;
	}

	return true;
}

bool FMadRegionFile::FlushFileToDisk(FString& OutError)
{
	if (!IFileManager::Get().FileExists(*FilePath))
	{
		return true;
	}
	Unmap();
	TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*FilePath, /*bAppend*/ true, /*bAllowRead*/ true));
	if (!Handle.IsValid() || !Handle->Flush(/*bFullFlush*/ true))
	{
		OutError = FString::Printf(TEXT("could not flush %s to disk"), *FilePath);
		return false;
	}
	return true;
}

bool FMadRegionFile::WriteBytes(int64 Offset, const uint8* Data, int64 Size, FString& OutError, bool bToDisk)
{
	// A file mapping pins the file's length on Windows, so it has to go before
	// any write that might extend it.
	Unmap();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*FilePath, /*bAppend*/ true, /*bAllowRead*/ true));
	if (!Handle.IsValid())
	{
		OutError = FString::Printf(TEXT("could not open %s for writing"), *FilePath);
		return false;
	}

	// Writing past the current end is legal and zero-fills the gap; that is how
	// a freshly created region gets its 68 KiB of header and index.
	if (!Handle->Seek(Offset))
	{
		OutError = FString::Printf(TEXT("%s: could not seek to %lld"), *FilePath, Offset);
		return false;
	}

	if (!Handle->Write(Data, Size))
	{
		OutError = FString::Printf(TEXT("%s: failed to write %lld bytes at %lld"), *FilePath, Size, Offset);
		return false;
	}

	Handle->Flush(bToDisk);
	return true;
}

FString FMadRegionFile::Describe() const
{
	TStringBuilder<1024> Builder;

	Builder.Appendf(TEXT("Region %s  file=%s\n"), *Coord.ToString(), *FilePath);
	Builder.Appendf(TEXT("  chunks: %d of %d slots\n"), NumChunks(), MadFall::RegionChunkSlots);

	int32 UniformCount = 0;
	int64 PayloadBytes = 0;
	for (const FMadRegionIndexEntry& Entry : Index)
	{
		if (!Entry.IsPresent()) { continue; }
		if (Entry.IsUniform()) { ++UniformCount; }
		else { PayloadBytes += Entry.PayloadBytes; }
	}

	Builder.Appendf(TEXT("  uniform (zero-cost): %d\n"), UniformCount);
	Builder.Appendf(TEXT("  payload bytes: %lld\n"), PayloadBytes);
	Builder.Appendf(TEXT("  sectors: %d used, %d free in %d runs, %d awaiting the next index write\n"),
		NextFreeSector, GetFreeSectorCount(), FreeRuns.Num(), GetPendingFreeSectorCount());
	if (bWasUnclean || bRecoveredIndex)
	{
		Builder.Appendf(TEXT("  opened after an unclean shutdown%s\n"), bRecoveredIndex ? TEXT(", index recovered from the journal") : TEXT(""));
	}
	Builder.Appendf(TEXT("  string table: %d ids at sector %d\n"), Strings.Num(), StringTableSector);

	return Builder.ToString();
}

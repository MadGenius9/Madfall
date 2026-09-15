// Copyright MadFall. All Rights Reserved.

#include "MadChunkStorage.h"

#include "MadFallCore.h"
#include "MadFallStats.h"

DECLARE_CYCLE_STAT(TEXT("Chunk Repack"), STAT_MadChunkRepack, STATGROUP_MadFallVoxel);
DECLARE_CYCLE_STAT(TEXT("Chunk Compact"), STAT_MadChunkCompact, STATGROUP_MadFallVoxel);

FMadChunkStorage::FMadChunkStorage()
{
	// One palette entry, every voxel referencing it, no index array at all.
	Palette.Add(FMadBlockPaletteEntry{ MadFall::BlockTypeAir, 0, static_cast<uint32>(MadFall::ChunkVoxelCount) });
}

FMadChunkStorage::FMadChunkStorage(const FMadChunkStorage& Other)
	: Palette(Other.Palette)
	, Indices(Other.Indices)
	, Density(CloneArray(Other.Density))
	, Rotation(CloneArray(Other.Rotation))
	, Flags(CloneArray(Other.Flags))
	, DamageMap(Other.DamageMap)
	, DefaultDensity(Other.DefaultDensity)
	, DefaultRotation(Other.DefaultRotation)
	, DefaultFlags(Other.DefaultFlags)
{
}

FMadChunkStorage& FMadChunkStorage::operator=(const FMadChunkStorage& Other)
{
	if (this != &Other)
	{
		Palette = Other.Palette;
		Indices = Other.Indices;
		Density = CloneArray(Other.Density);
		Rotation = CloneArray(Other.Rotation);
		Flags = CloneArray(Other.Flags);
		DamageMap = Other.DamageMap;
		DefaultDensity = Other.DefaultDensity;
		DefaultRotation = Other.DefaultRotation;
		DefaultFlags = Other.DefaultFlags;
	}
	return *this;
}

TUniquePtr<uint8[]> FMadChunkStorage::CloneArray(const TUniquePtr<uint8[]>& Source)
{
	if (!Source)
	{
		return nullptr;
	}

	TUniquePtr<uint8[]> Copy = MakeUnique<uint8[]>(MadFall::ChunkVoxelCount);
	FMemory::Memcpy(Copy.Get(), Source.Get(), MadFall::ChunkVoxelCount);
	return Copy;
}

uint8* FMadChunkStorage::EnsureArray(TUniquePtr<uint8[]>& Array, uint8 DefaultValue)
{
	if (!Array)
	{
		Array = MakeUnique<uint8[]>(MadFall::ChunkVoxelCount);
		FMemory::Memset(Array.Get(), DefaultValue, MadFall::ChunkVoxelCount);
	}
	return Array.Get();
}

FMadVoxel FMadChunkStorage::GetVoxel(int32 Index) const
{
	checkSlow(Index >= 0 && Index < MadFall::ChunkVoxelCount);

	FMadVoxel Voxel;
	Voxel.BlockTypeID = GetBlockId(Index);
	Voxel.Density = Density ? Density[Index] : DefaultDensity;
	Voxel.Rotation = Rotation ? Rotation[Index] : DefaultRotation;
	Voxel.Flags = Flags ? Flags[Index] : DefaultFlags;

	const uint8* const Damage = DamageMap.Find(Index);
	Voxel.Damage = Damage ? *Damage : static_cast<uint8>(0);

	return Voxel;
}

void FMadChunkStorage::MaterializeIndices()
{
	if (!Indices.IsEmpty())
	{
		return;
	}

	const int32 Bits = FMadBitPackedArray::BitsForValueCount(FMath::Max(Palette.Num(), 1));
	Indices.Reset(MadFall::ChunkVoxelCount, Bits);
	// Reset() zeroes, and every voxel of a uniform chunk points at palette slot 0.
}

int32 FMadChunkStorage::FindOrAddPaletteEntry(uint16 RuntimeId)
{
	for (int32 Slot = 0; Slot < Palette.Num(); ++Slot)
	{
		if (Palette[Slot].RuntimeId == RuntimeId && Palette[Slot].RefCount > 0)
		{
			return Slot;
		}
	}

	// Reuse an emptied slot before growing: this is what keeps a chunk that is
	// repeatedly built and demolished from ratcheting its bit width upward.
	for (int32 Slot = 0; Slot < Palette.Num(); ++Slot)
	{
		if (Palette[Slot].RefCount == 0)
		{
			Palette[Slot].RuntimeId = RuntimeId;
			return Slot;
		}
	}

	const int32 NewSlot = Palette.Add(FMadBlockPaletteEntry{ RuntimeId, 0, 0 });

	const int32 RequiredBits = FMadBitPackedArray::BitsForValueCount(Palette.Num());
	if (!Indices.IsEmpty() && RequiredBits > Indices.GetBitsPerValue())
	{
		SCOPE_CYCLE_COUNTER(STAT_MadChunkRepack);
		Indices.Repack(RequiredBits);
	}

	return NewSlot;
}

void FMadChunkStorage::SetVoxel(int32 Index, const FMadVoxel& Voxel)
{
	checkSlow(Index >= 0 && Index < MadFall::ChunkVoxelCount);

	// --- block id ---
	// Unchanged id touches neither the palette nor the index array. Placing the
	// same block type repeatedly - which is what building a wall looks like -
	// then costs only the side-array writes below.
	const uint16 PreviousId = GetBlockId(Index);
	if (PreviousId != Voxel.BlockTypeID)
	{
		MaterializeIndices();

		const int32 OldSlot = static_cast<int32>(Indices.Get(Index));
		if (Palette[OldSlot].RefCount > 0)
		{
			--Palette[OldSlot].RefCount;
		}

		const int32 NewSlot = FindOrAddPaletteEntry(Voxel.BlockTypeID);
		++Palette[NewSlot].RefCount;
		Indices.Set(Index, static_cast<uint32>(NewSlot));
	}

	// --- side arrays ---
	// Only materialize an array when the written value actually differs from
	// the chunk's default. Placing 500 identical blocks in a fresh chunk should
	// not allocate three 32 KiB arrays.
	if (Voxel.Density != DefaultDensity || Density)
	{
		EnsureArray(Density, DefaultDensity)[Index] = Voxel.Density;
	}

	if (Voxel.Rotation != DefaultRotation || Rotation)
	{
		EnsureArray(Rotation, DefaultRotation)[Index] = Voxel.Rotation;
	}

	if (Voxel.Flags != DefaultFlags || Flags)
	{
		EnsureArray(Flags, DefaultFlags)[Index] = Voxel.Flags;
	}

	if (Voxel.Damage != 0)
	{
		DamageMap.Add(Index, Voxel.Damage);
	}
	else
	{
		DamageMap.Remove(Index);
	}
}

void FMadChunkStorage::Fill(const FMadVoxel& Voxel)
{
	Palette.Reset();
	Palette.Add(FMadBlockPaletteEntry{ Voxel.BlockTypeID, 0, static_cast<uint32>(MadFall::ChunkVoxelCount) });

	Indices.Empty();
	Density.Reset();
	Rotation.Reset();
	Flags.Reset();
	DamageMap.Reset();

	DefaultDensity = Voxel.Density;
	DefaultRotation = Voxel.Rotation;
	DefaultFlags = Voxel.Flags;

	// Damage has no "default" representation - a uniformly damaged chunk is not
	// a thing worldgen or an edit can produce, and pretending otherwise would
	// mean a fourth default field nothing ever sets.
	checkf(Voxel.Damage == 0, TEXT("Fill() cannot express a uniform non-zero damage value"));
}

bool FMadChunkStorage::IsUniform() const
{
	return Indices.IsEmpty() && !Density && !Rotation && !Flags && DamageMap.IsEmpty();
}

FMadVoxel FMadChunkStorage::GetUniformVoxel() const
{
	check(IsUniform());
	return FMadVoxel{ Palette[0].RuntimeId, DefaultDensity, 0, DefaultRotation, DefaultFlags };
}

int32 FMadChunkStorage::GetDistinctBlockCount() const
{
	int32 Count = 0;
	for (const FMadBlockPaletteEntry& Entry : Palette)
	{
		if (Entry.RefCount > 0)
		{
			++Count;
		}
	}
	return FMath::Max(Count, 1);
}

void FMadChunkStorage::Compact()
{
	SCOPE_CYCLE_COUNTER(STAT_MadChunkCompact);

	if (Indices.IsEmpty())
	{
		// Already uniform. Still drop any stale palette entries.
		if (Palette.Num() > 1)
		{
			const FMadBlockPaletteEntry Kept = Palette[0];
			Palette.Reset();
			Palette.Add(Kept);
		}
		return;
	}

	// Recount from the index array rather than trusting RefCount: a bug in an
	// edit path that leaked a refcount would otherwise silently pin a palette
	// slot forever and block every future re-tier.
	TArray<uint32> Counts;
	Counts.SetNumZeroed(Palette.Num());

	for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
	{
		++Counts[static_cast<int32>(Indices.Get(Index))];
	}

	TArray<FMadBlockPaletteEntry> NewPalette;
	TArray<int32> Remap;
	Remap.SetNumUninitialized(Palette.Num());

	for (int32 Slot = 0; Slot < Palette.Num(); ++Slot)
	{
		if (Counts[Slot] > 0)
		{
			Remap[Slot] = NewPalette.Num();
			NewPalette.Add(FMadBlockPaletteEntry{ Palette[Slot].RuntimeId, 0, Counts[Slot] });
		}
		else
		{
			Remap[Slot] = INDEX_NONE;
		}
	}

	const bool bPaletteChanged = NewPalette.Num() != Palette.Num();
	const int32 NewBits = FMadBitPackedArray::BitsForValueCount(FMath::Max(NewPalette.Num(), 1));

	if (bPaletteChanged)
	{
		FMadBitPackedArray NewIndices(MadFall::ChunkVoxelCount, NewBits);
		for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
		{
			const int32 OldSlot = static_cast<int32>(Indices.Get(Index));
			checkSlow(Remap[OldSlot] != INDEX_NONE);
			NewIndices.Set(Index, static_cast<uint32>(Remap[OldSlot]));
		}

		Palette = MoveTemp(NewPalette);
		Indices = MoveTemp(NewIndices);
	}
	else
	{
		Palette = MoveTemp(NewPalette);
		if (NewBits < Indices.GetBitsPerValue())
		{
			Indices.Repack(NewBits);
		}
	}

	// A chunk that has been demolished back to a single block type should give
	// its 16 KiB of indices back rather than staying "non-uniform forever".
	if (Palette.Num() == 1)
	{
		Indices.Empty();
	}
}

int64 FMadChunkStorage::GetAllocatedSize() const
{
	int64 Size = sizeof(FMadChunkStorage);
	Size += Palette.GetAllocatedSize();
	Size += Indices.GetAllocatedSize();
	Size += Density ? MadFall::ChunkVoxelCount : 0;
	Size += Rotation ? MadFall::ChunkVoxelCount : 0;
	Size += Flags ? MadFall::ChunkVoxelCount : 0;
	Size += DamageMap.GetAllocatedSize();
	return Size;
}

bool FMadChunkStorage::EqualsVoxelwise(const FMadChunkStorage& Other) const
{
	for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
	{
		if (GetVoxel(Index) != Other.GetVoxel(Index))
		{
			return false;
		}
	}
	return true;
}

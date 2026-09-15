// Copyright MadFall. All Rights Reserved.

#include "MadChunkSerializer.h"

#include "MadBlockRegistry.h"
#include "MadFallCore.h"
#include "MadFallStats.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

DECLARE_CYCLE_STAT(TEXT("Chunk Serialize"), STAT_MadChunkSerialize, STATGROUP_MadFallVoxel);
DECLARE_CYCLE_STAT(TEXT("Chunk Deserialize"), STAT_MadChunkDeserialize, STATGROUP_MadFallVoxel);

namespace MadFall::ChunkSerializer
{
	namespace
	{
		/**
		 * Run-length encode 32768 bytes as {uint16 RunLength, uint8 Value} pairs.
		 *
		 * Worth it because the arrays this runs over are overwhelmingly constant:
		 * a density array is long stretches of 0x00 above the surface and 0xFF
		 * below it, and a flags array is almost entirely zero. LZ4 would find
		 * most of this too, but RLE first cuts what LZ4 has to scan by 50-100x
		 * on a typical chunk, and the decode is a memset loop.
		 *
		 * A run never exceeds 32768, so uint16 is sufficient.
		 */
		void RunLengthEncode(const uint8* Values, int32 Count, TArray<uint8>& Out)
		{
			TArray<uint8> Body;
			int32 PairCount = 0;

			FMemoryWriter BodyWriter(Body);

			int32 Index = 0;
			while (Index < Count)
			{
				const uint8 Value = Values[Index];
				int32 RunEnd = Index + 1;
				while (RunEnd < Count && Values[RunEnd] == Value && (RunEnd - Index) < 0xFFFF)
				{
					++RunEnd;
				}

				uint16 RunLength = static_cast<uint16>(RunEnd - Index);
				uint8 WrittenValue = Value;
				BodyWriter << RunLength;
				BodyWriter << WrittenValue;

				++PairCount;
				Index = RunEnd;
			}

			FMemoryWriter Writer(Out);
			uint32 Pairs = static_cast<uint32>(PairCount);
			Writer << Pairs;
			Writer.Serialize(Body.GetData(), Body.Num());
		}

		bool RunLengthDecode(FMemoryReader& Reader, uint8* OutValues, int32 Count, FString& OutError)
		{
			uint32 PairCount = 0;
			Reader << PairCount;

			int32 Written = 0;
			for (uint32 Pair = 0; Pair < PairCount; ++Pair)
			{
				if (Reader.AtEnd())
				{
					OutError = FString::Printf(TEXT("run-length data ended after %u of %u pairs"), Pair, PairCount);
					return false;
				}

				uint16 RunLength = 0;
				uint8 Value = 0;
				Reader << RunLength;
				Reader << Value;

				if (Written + static_cast<int32>(RunLength) > Count)
				{
					// Refusing rather than clamping: a run that overflows means
					// the data is not what it claims, and writing a truncated
					// chunk into a live world is worse than failing the load.
					OutError = FString::Printf(
						TEXT("run-length data describes more than %d values (%d + %u)"), Count, Written, RunLength);
					return false;
				}

				FMemory::Memset(OutValues + Written, Value, RunLength);
				Written += RunLength;
			}

			if (Written != Count)
			{
				OutError = FString::Printf(TEXT("run-length data covered %d of %d values"), Written, Count);
				return false;
			}

			return true;
		}

		void WriteSection(FMemoryWriter& Writer, EMadChunkSection Section, const TArray<uint8>& Body)
		{
			uint8 Id = static_cast<uint8>(Section);
			uint32 Length = static_cast<uint32>(Body.Num());
			Writer << Id;
			Writer << Length;
			Writer.Serialize(const_cast<uint8*>(Body.GetData()), Body.Num());
		}
	}

	void Serialize(
		const FMadChunkStorage& Chunk,
		const IMadBlockRegistry& Registry,
		FInternString InternString,
		FMadSerializedChunk& Out)
	{
		SCOPE_CYCLE_COUNTER(STAT_MadChunkSerialize);

		Out = FMadSerializedChunk();

		// --- uniform fast path ---
		if (Chunk.IsUniform())
		{
			const FMadVoxel Voxel = Chunk.GetUniformVoxel();
			Out.bUniform = true;
			Out.UniformBlockId = Registry.GetStringId(Voxel.BlockTypeID);
			Out.UniformDensity = Voxel.Density;
			Out.UniformRotation = Voxel.Rotation;
			Out.UniformFlags = static_cast<uint8>(Voxel.Flags & ~MadFall::TransientVoxelFlagMask);

			// Interning here even though no payload references it keeps the
			// region's string table the single place block ids live.
			InternString(Out.UniformBlockId);
			return;
		}

		FMemoryWriter Writer(Out.Payload);

		// --- DEFAULTS ---
		{
			TArray<uint8> Body;
			FMemoryWriter Body_(Body);
			uint8 Density = Chunk.GetDefaultDensity();
			uint8 Rotation = Chunk.GetDefaultRotation();
			uint8 Flags = static_cast<uint8>(Chunk.GetDefaultFlags() & ~MadFall::TransientVoxelFlagMask);
			Body_ << Density;
			Body_ << Rotation;
			Body_ << Flags;
			WriteSection(Writer, EMadChunkSection::Defaults, Body);
		}

		// --- PALETTE ---
		{
			const TArray<FMadBlockPaletteEntry>& Palette = Chunk.GetPalette();

			TArray<uint8> Body;
			FMemoryWriter Body_(Body);
			uint16 Count = static_cast<uint16>(Palette.Num());
			Body_ << Count;

			for (const FMadBlockPaletteEntry& Entry : Palette)
			{
				const FName BlockId = Registry.GetStringId(Entry.RuntimeId);
				uint32 StringIndex = InternString(BlockId);
				Body_ << StringIndex;
			}

			WriteSection(Writer, EMadChunkSection::Palette, Body);
		}

		// --- BLOCK_INDICES ---
		// Written as plain uint16 values, run-length encoded. The bit width is
		// deliberately NOT stored: it is an in-memory packing detail derived
		// from the palette size, and writing it would let a file disagree with
		// the palette it ships with.
		if (!Chunk.GetIndices().IsEmpty())
		{
			const FMadBitPackedArray& Indices = Chunk.GetIndices();

			TArray<uint8> Body;
			FMemoryWriter Body_(Body);

			TArray<uint8> Pairs;
			FMemoryWriter Pairs_(Pairs);
			uint32 PairCount = 0;

			int32 Index = 0;
			while (Index < MadFall::ChunkVoxelCount)
			{
				const uint32 Value = Indices.Get(Index);
				int32 RunEnd = Index + 1;
				while (RunEnd < MadFall::ChunkVoxelCount
					&& Indices.Get(RunEnd) == Value
					&& (RunEnd - Index) < 0xFFFF)
				{
					++RunEnd;
				}

				uint16 RunLength = static_cast<uint16>(RunEnd - Index);
				uint16 Written = static_cast<uint16>(Value);
				Pairs_ << RunLength;
				Pairs_ << Written;

				++PairCount;
				Index = RunEnd;
			}

			Body_ << PairCount;
			Body_.Serialize(Pairs.GetData(), Pairs.Num());

			WriteSection(Writer, EMadChunkSection::BlockIndices, Body);
		}

		// --- DENSITY / ROTATION / FLAGS ---
		// Only written when the side array actually exists. A chunk whose every
		// voxel matches the defaults writes nothing at all for that field.
		if (const uint8* Density = Chunk.GetDensityArray())
		{
			TArray<uint8> Body;
			RunLengthEncode(Density, MadFall::ChunkVoxelCount, Body);
			WriteSection(Writer, EMadChunkSection::Density, Body);
		}

		if (const uint8* Rotation = Chunk.GetRotationArray())
		{
			TArray<uint8> Body;
			RunLengthEncode(Rotation, MadFall::ChunkVoxelCount, Body);
			WriteSection(Writer, EMadChunkSection::Rotation, Body);
		}

		if (const uint8* Flags = Chunk.GetFlagsArray())
		{
			// Strip transient bits before encoding rather than after, so the
			// runs actually coalesce.
			TArray<uint8> Persisted;
			Persisted.SetNumUninitialized(MadFall::ChunkVoxelCount);
			for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
			{
				Persisted[Index] = static_cast<uint8>(Flags[Index] & ~MadFall::TransientVoxelFlagMask);
			}

			TArray<uint8> Body;
			RunLengthEncode(Persisted.GetData(), MadFall::ChunkVoxelCount, Body);
			WriteSection(Writer, EMadChunkSection::Flags, Body);
		}

		// --- DAMAGE ---
		// Sparse. An untouched chunk has none; even a heavily fought-over base
		// has a few hundred out of 32768.
		if (Chunk.GetDamageMap().Num() > 0)
		{
			TArray<uint8> Body;
			FMemoryWriter Body_(Body);

			uint32 Count = static_cast<uint32>(Chunk.GetDamageMap().Num());
			Body_ << Count;

			// Sorted so the file is byte-identical for identical chunk state -
			// TMap iteration order is not stable, and a save that differs run to
			// run makes every "did this change?" question unanswerable.
			TArray<int32> Keys;
			Chunk.GetDamageMap().GenerateKeyArray(Keys);
			Keys.Sort();

			for (int32 Key : Keys)
			{
				uint16 VoxelIndex = static_cast<uint16>(Key);
				uint8 Damage = Chunk.GetDamageMap()[Key];
				Body_ << VoxelIndex;
				Body_ << Damage;
			}

			WriteSection(Writer, EMadChunkSection::Damage, Body);
		}
	}

	bool Deserialize(
		const FMadSerializedChunk& In,
		FMadBlockRegistry& Registry,
		FResolveString ResolveString,
		FMadChunkStorage& OutChunk,
		FString& OutError)
	{
		SCOPE_CYCLE_COUNTER(STAT_MadChunkDeserialize);

		OutChunk = FMadChunkStorage();

		if (In.bUniform)
		{
			FMadVoxel Voxel;
			Voxel.BlockTypeID = Registry.GetOrCreateUnresolvedId(In.UniformBlockId);
			Voxel.Density = In.UniformDensity;
			Voxel.Damage = 0;
			Voxel.Rotation = In.UniformRotation;
			Voxel.Flags = In.UniformFlags;
			OutChunk.Fill(Voxel);
			return true;
		}

		FMemoryReader Reader(In.Payload);

		TArray<uint16> PaletteRuntimeIds;
		bool bSawPalette = false;
		bool bSawIndices = false;
		TArray<uint16> IndexValues;

		uint8 DefaultDensity = 0;
		uint8 DefaultRotation = 0;
		uint8 DefaultFlags = 0;

		TArray<uint8> Density;
		TArray<uint8> Rotation;
		TArray<uint8> Flags;
		TMap<int32, uint8> Damage;

		while (!Reader.AtEnd())
		{
			uint8 SectionId = 0;
			uint32 SectionLength = 0;
			Reader << SectionId;

			if (Reader.AtEnd())
			{
				OutError = TEXT("payload ended inside a section header");
				return false;
			}

			Reader << SectionLength;

			const int64 SectionStart = Reader.Tell();
			const int64 SectionEnd = SectionStart + static_cast<int64>(SectionLength);

			if (SectionEnd > Reader.TotalSize())
			{
				OutError = FString::Printf(
					TEXT("section %u claims %u bytes but only %lld remain"),
					SectionId, SectionLength, Reader.TotalSize() - SectionStart);
				return false;
			}

			switch (static_cast<EMadChunkSection>(SectionId))
			{
			case EMadChunkSection::Defaults:
			{
				Reader << DefaultDensity;
				Reader << DefaultRotation;
				Reader << DefaultFlags;
				break;
			}

			case EMadChunkSection::Palette:
			{
				uint16 Count = 0;
				Reader << Count;

				PaletteRuntimeIds.Reset(Count);
				for (uint16 Slot = 0; Slot < Count; ++Slot)
				{
					uint32 StringIndex = 0;
					Reader << StringIndex;

					const FName BlockId = ResolveString(StringIndex);
					if (BlockId.IsNone())
					{
						OutError = FString::Printf(
							TEXT("palette slot %u references string table entry %u, which does not exist"),
							Slot, StringIndex);
						return false;
					}

					// A missing mod is NOT a load failure. The registry hands
					// back a placeholder id that keeps this exact string, and
					// the block is written back out unchanged on the next save.
					PaletteRuntimeIds.Add(Registry.GetOrCreateUnresolvedId(BlockId));
				}

				bSawPalette = true;
				break;
			}

			case EMadChunkSection::BlockIndices:
			{
				uint32 PairCount = 0;
				Reader << PairCount;

				IndexValues.Reset(MadFall::ChunkVoxelCount);
				for (uint32 Pair = 0; Pair < PairCount; ++Pair)
				{
					uint16 RunLength = 0;
					uint16 Value = 0;
					Reader << RunLength;
					Reader << Value;

					if (IndexValues.Num() + RunLength > MadFall::ChunkVoxelCount)
					{
						OutError = TEXT("block index runs describe more than 32768 voxels");
						return false;
					}

					for (uint16 Step = 0; Step < RunLength; ++Step)
					{
						IndexValues.Add(Value);
					}
				}

				if (IndexValues.Num() != MadFall::ChunkVoxelCount)
				{
					OutError = FString::Printf(TEXT("block index runs covered %d of %d voxels"),
						IndexValues.Num(), MadFall::ChunkVoxelCount);
					return false;
				}

				bSawIndices = true;
				break;
			}

			case EMadChunkSection::Density:
			{
				Density.SetNumUninitialized(MadFall::ChunkVoxelCount);
				if (!RunLengthDecode(Reader, Density.GetData(), MadFall::ChunkVoxelCount, OutError))
				{
					OutError = FString::Printf(TEXT("density section: %s"), *OutError);
					return false;
				}
				break;
			}

			case EMadChunkSection::Rotation:
			{
				Rotation.SetNumUninitialized(MadFall::ChunkVoxelCount);
				if (!RunLengthDecode(Reader, Rotation.GetData(), MadFall::ChunkVoxelCount, OutError))
				{
					OutError = FString::Printf(TEXT("rotation section: %s"), *OutError);
					return false;
				}
				break;
			}

			case EMadChunkSection::Flags:
			{
				Flags.SetNumUninitialized(MadFall::ChunkVoxelCount);
				if (!RunLengthDecode(Reader, Flags.GetData(), MadFall::ChunkVoxelCount, OutError))
				{
					OutError = FString::Printf(TEXT("flags section: %s"), *OutError);
					return false;
				}
				break;
			}

			case EMadChunkSection::Damage:
			{
				uint32 Count = 0;
				Reader << Count;

				for (uint32 Entry = 0; Entry < Count; ++Entry)
				{
					uint16 VoxelIndex = 0;
					uint8 Value = 0;
					Reader << VoxelIndex;
					Reader << Value;

					if (VoxelIndex >= MadFall::ChunkVoxelCount)
					{
						OutError = FString::Printf(TEXT("damage entry addresses voxel %u, outside the chunk"), VoxelIndex);
						return false;
					}

					Damage.Add(static_cast<int32>(VoxelIndex), Value);
				}
				break;
			}

			default:
			{
				// Forward compatibility: a section this build does not know
				// about is skipped so an older client can still open a newer
				// world. It is NOT preserved on rewrite - the payload version
				// gates anything that would actually break.
				UE_LOG(LogMadFallVoxel, Warning,
					TEXT("Skipping unknown chunk section %u (%u bytes). It will be dropped on the next save."),
					SectionId, SectionLength);
				break;
			}
			}

			// Seek to the declared end regardless of how much the handler read.
			// A handler that reads less than the section claims must not leave
			// the reader misaligned for every section after it.
			Reader.Seek(SectionEnd);
		}

		if (!bSawPalette)
		{
			OutError = TEXT("payload has no palette section");
			return false;
		}

		if (PaletteRuntimeIds.Num() == 0)
		{
			OutError = TEXT("payload has an empty palette");
			return false;
		}

		// --- rebuild ---
		{
			FMadVoxel Base;
			Base.BlockTypeID = PaletteRuntimeIds[0];
			Base.Density = DefaultDensity;
			Base.Damage = 0;
			Base.Rotation = DefaultRotation;
			Base.Flags = DefaultFlags;
			OutChunk.Fill(Base);
		}

		if (bSawIndices)
		{
			for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
			{
				const int32 Slot = IndexValues[Index];
				if (!PaletteRuntimeIds.IsValidIndex(Slot))
				{
					OutError = FString::Printf(TEXT("voxel %d references palette slot %d of %d"),
						Index, Slot, PaletteRuntimeIds.Num());
					return false;
				}

				FMadVoxel Voxel;
				Voxel.BlockTypeID = PaletteRuntimeIds[Slot];
				Voxel.Density = Density.Num() ? Density[Index] : DefaultDensity;
				Voxel.Rotation = Rotation.Num() ? Rotation[Index] : DefaultRotation;
				Voxel.Flags = Flags.Num() ? Flags[Index] : DefaultFlags;
				Voxel.Damage = 0;
				OutChunk.SetVoxel(Index, Voxel);
			}
		}
		else if (Density.Num() || Rotation.Num() || Flags.Num())
		{
			// Single block type but varying density - a smooth terrain chunk of
			// one material, which is the common case underground.
			for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
			{
				FMadVoxel Voxel;
				Voxel.BlockTypeID = PaletteRuntimeIds[0];
				Voxel.Density = Density.Num() ? Density[Index] : DefaultDensity;
				Voxel.Rotation = Rotation.Num() ? Rotation[Index] : DefaultRotation;
				Voxel.Flags = Flags.Num() ? Flags[Index] : DefaultFlags;
				Voxel.Damage = 0;
				OutChunk.SetVoxel(Index, Voxel);
			}
		}

		for (const TPair<int32, uint8>& Pair : Damage)
		{
			FMadVoxel Voxel = OutChunk.GetVoxel(Pair.Key);
			Voxel.Damage = Pair.Value;
			OutChunk.SetVoxel(Pair.Key, Voxel);
		}

		return true;
	}
}

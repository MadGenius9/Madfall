// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A fixed-length array of small unsigned integers packed at 1, 2, 4, 8 or 16
 * bits each.
 *
 * Used for a chunk's per-voxel palette indices. A 32^3 chunk with six distinct
 * block types needs 3 bits of information per voxel; storing a uint16 there
 * would waste 13 of every 16 bits across 32768 entries.
 *
 * WHY ONLY POWER-OF-TWO WIDTHS THAT DIVIDE 32:
 * With 1/2/4/8/16 bits, an exact number of values fits in each uint32 word and
 * no value ever straddles a word boundary. Get and Set are one load, one shift
 * and one mask - no branch, no second word fetch. Arbitrary widths (3, 5, 7)
 * would save a further ~15% of index memory and cost a straddle check on the
 * single hottest read in the mesher. Indices are already a small fraction of
 * chunk memory next to the density array, so the trade is not close.
 */
class MADFALLCORE_API FMadBitPackedArray
{
public:
	FMadBitPackedArray() = default;

	/** Allocates InNum entries at InBitsPerValue bits, all zero. */
	FMadBitPackedArray(int32 InNum, int32 InBitsPerValue)
	{
		Reset(InNum, InBitsPerValue);
	}

	/** The smallest supported width that can represent InDistinctValues distinct values. */
	static int32 BitsForValueCount(int32 InDistinctValues)
	{
		if (InDistinctValues <= 2)   { return 1; }
		if (InDistinctValues <= 4)   { return 2; }
		if (InDistinctValues <= 16)  { return 4; }
		if (InDistinctValues <= 256) { return 8; }
		return 16;
	}

	static bool IsSupportedWidth(int32 InBitsPerValue)
	{
		return InBitsPerValue == 1 || InBitsPerValue == 2 || InBitsPerValue == 4
			|| InBitsPerValue == 8 || InBitsPerValue == 16;
	}

	void Reset(int32 InNum, int32 InBitsPerValue)
	{
		check(InNum >= 0);
		check(IsSupportedWidth(InBitsPerValue));

		Num = InNum;
		BitsPerValue = InBitsPerValue;

		const int32 ValuesPerWord = 32 / BitsPerValue;
		const int32 WordCount = (Num + ValuesPerWord - 1) / ValuesPerWord;

		Words.Reset();
		Words.SetNumZeroed(WordCount);
	}

	void Empty()
	{
		Words.Empty();
		Num = 0;
		BitsPerValue = 0;
	}

	bool IsEmpty() const { return Num == 0; }
	int32 GetNum() const { return Num; }
	int32 GetBitsPerValue() const { return BitsPerValue; }
	int32 GetMaxValue() const { return (1 << BitsPerValue) - 1; }

	FORCEINLINE uint32 Get(int32 Index) const
	{
		checkSlow(Index >= 0 && Index < Num);

		const int32 ValuesPerWord = 32 / BitsPerValue;
		const int32 WordIndex = Index / ValuesPerWord;
		const int32 Shift = (Index % ValuesPerWord) * BitsPerValue;
		const uint32 Mask = (BitsPerValue == 32) ? 0xFFFFFFFFu : ((1u << BitsPerValue) - 1u);

		return (Words[WordIndex] >> Shift) & Mask;
	}

	FORCEINLINE void Set(int32 Index, uint32 Value)
	{
		checkSlow(Index >= 0 && Index < Num);
		checkSlow(Value <= static_cast<uint32>(GetMaxValue()));

		const int32 ValuesPerWord = 32 / BitsPerValue;
		const int32 WordIndex = Index / ValuesPerWord;
		const int32 Shift = (Index % ValuesPerWord) * BitsPerValue;
		const uint32 Mask = (1u << BitsPerValue) - 1u;

		uint32& Word = Words[WordIndex];
		Word = (Word & ~(Mask << Shift)) | ((Value & Mask) << Shift);
	}

	/**
	 * Re-packs every value at a new width, preserving contents.
	 *
	 * O(Num). Runs on a worker thread when a chunk's palette grows past its
	 * current tier - never on the game thread, and never inside an edit that
	 * has already taken the write lock for a single voxel.
	 */
	void Repack(int32 NewBitsPerValue)
	{
		check(IsSupportedWidth(NewBitsPerValue));

		if (NewBitsPerValue == BitsPerValue)
		{
			return;
		}

		FMadBitPackedArray Repacked(Num, NewBitsPerValue);
		const uint32 NewMax = static_cast<uint32>(Repacked.GetMaxValue());

		for (int32 Index = 0; Index < Num; ++Index)
		{
			const uint32 Value = Get(Index);
			// Narrowing must never silently truncate a palette index: that
			// would remap every voxel of one block type onto another.
			checkf(Value <= NewMax, TEXT("Value %u does not fit in %d bits"), Value, NewBitsPerValue);
			Repacked.Set(Index, Value);
		}

		*this = MoveTemp(Repacked);
	}

	/** Sets every entry to the same value without re-deriving shifts per index. */
	void Fill(uint32 Value)
	{
		checkSlow(Value <= static_cast<uint32>(GetMaxValue()));

		const int32 ValuesPerWord = 32 / BitsPerValue;
		const uint32 Mask = (1u << BitsPerValue) - 1u;

		uint32 Pattern = 0;
		for (int32 Slot = 0; Slot < ValuesPerWord; ++Slot)
		{
			Pattern |= (Value & Mask) << (Slot * BitsPerValue);
		}

		for (uint32& Word : Words)
		{
			Word = Pattern;
		}
	}

	/** Raw word storage, for the serializer. */
	const TArray<uint32>& GetWords() const { return Words; }
	TArray<uint32>& GetWords() { return Words; }

	int64 GetAllocatedSize() const { return Words.GetAllocatedSize(); }

	bool operator==(const FMadBitPackedArray& Other) const
	{
		return Num == Other.Num && BitsPerValue == Other.BitsPerValue && Words == Other.Words;
	}

	bool operator!=(const FMadBitPackedArray& Other) const { return !(*this == Other); }

private:
	TArray<uint32> Words;
	int32 Num = 0;
	int32 BitsPerValue = 0;
};

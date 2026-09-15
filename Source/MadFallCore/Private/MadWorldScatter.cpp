// Copyright MadFall. All Rights Reserved.

#include "MadWorldScatter.h"

#include "MadNoise.h"

void MadFall::Scatter::BuildTree(uint32 Seed, int32 Height, float Radius, bool bLeaves,
	TArray<FIntVector>& OutTrunk, TArray<FIntVector>& OutLeaves, int32 LeafSpan)
{
	OutTrunk.Reset();
	OutLeaves.Reset();

	Height = FMath::Max(Height, 1);
	for (int32 Z = 1; Z <= Height; ++Z)
	{
		OutTrunk.Add(FIntVector(0, 0, Z));
	}

	if (!bLeaves || Radius <= 0.0f)
	{
		return;
	}

	// Two full layers around the top of the trunk, a narrower one above it and
	// a single cap: reads as a crown from the ground, and every leaf is within
	// Radius horizontal steps of the trunk, which is what the leaves' structural
	// span has to cover.
	const int32 Reach = FMath::CeilToInt(Radius);
	const float RadiusSq = Radius * Radius + 0.5f;
	const float InnerSq = FMath::Square(FMath::Max(Radius - 1.0f, 0.0f)) + 0.5f;

	for (int32 Layer = -1; Layer <= 2; ++Layer)
	{
		const int32 Z = Height + Layer;
		const float LayerSq = Layer <= 0 ? RadiusSq : (Layer == 1 ? InnerSq : 0.5f);

		for (int32 DY = -Reach; DY <= Reach; ++DY)
		{
			for (int32 DX = -Reach; DX <= Reach; ++DX)
			{
				const float DistSq = static_cast<float>(DX * DX + DY * DY);
				if (DistSq > LayerSq)
				{
					continue;
				}
				if (DX == 0 && DY == 0 && Z <= Height)
				{
					continue;   // the trunk
				}

				// Thin the outer ring about a third of the time, from the seed.
				const bool bOuter = Layer <= 0 && DistSq > InnerSq;
				if (bOuter && MadFall::Noise::HashToUnitFloat(MadFall::Noise::Hash3(DX, DY, Layer, Seed)) < 0.35f)
				{
					continue;
				}

				OutLeaves.Add(FIntVector(DX, DY, Z));
			}
		}
	}

	// Thinning can cut an outer leaf off, or leave it reachable only by a long
	// way round. An unsupported leaf is a structural failure the load-time check
	// drops the moment the chunk streams in, so keep only leaves the solver
	// would hold: its costs, from the trunk (grounded, cost 0), as a 0-1 BFS.
	TMap<FIntVector, int32> Cost;
	for (const FIntVector& Leaf : OutLeaves)
	{
		Cost.Add(Leaf, MAX_int32);
	}
	TArray<FIntVector> Current = OutTrunk;
	TArray<FIntVector> Next;
	struct FStep { FIntVector Delta; int32 Cost; };
	static const FStep Steps[6] = {
		{ {0, 0, 1}, 0 },                                        // resting on it
		{ {1, 0, 0}, 1 }, { {-1, 0, 0}, 1 }, { {0, 1, 0}, 1 }, { {0, -1, 0}, 1 },   // beside it
		{ {0, 0, -1}, 1 }                                        // hanging under it
	};
	for (int32 Level = 0; Level <= LeafSpan && Current.Num() > 0; ++Level)
	{
		// Zero-cost moves stay on this level until nothing new is reached.
		for (int32 Index = 0; Index < Current.Num(); ++Index)
		{
			const FIntVector From = Current[Index];
			for (const FStep& Step : Steps)
			{
				int32* Found = Cost.Find(From + Step.Delta);
				const int32 NewCost = Level + Step.Cost;
				if (Found == nullptr || *Found <= NewCost || NewCost > LeafSpan)
				{
					continue;
				}
				*Found = NewCost;
				(Step.Cost == 0 ? Current : Next).Add(From + Step.Delta);
			}
		}
		Current = MoveTemp(Next);
		Next.Reset();
	}
	// Filtered in place so the order stays deterministic.
	OutLeaves.RemoveAll([&Cost](const FIntVector& Leaf) { return Cost.FindChecked(Leaf) == MAX_int32; });
}

float MadFall::Scatter::RollColumn(uint32 Seed, int32 WorldX, int32 WorldY)
{
	return MadFall::Noise::HashToUnitFloat(MadFall::Noise::Hash3(WorldX, WorldY, 0x5CA7, Seed));
}

// Copyright MadFall. All Rights Reserved.

#include "MadDebris.h"

namespace MadFall::Debris
{
	TArray<FMadDebrisCluster> BuildClusters(const TArray<FMadStructuralFailureRecord>& Failures,
		const FMadStructuralMaterials& Materials)
	{
		TMap<FIntVector, int32> ByPosition;
		ByPosition.Reserve(Failures.Num());
		for (int32 Index = 0; Index < Failures.Num(); ++Index)
		{
			ByPosition.Add(Failures[Index].Position, Index);
		}

		static const FIntVector Offsets[6] =
		{
			FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
			FIntVector(0, 1, 0), FIntVector(0, -1, 0),
			FIntVector(0, 0, 1), FIntVector(0, 0, -1)
		};

		TArray<bool> Visited;
		Visited.Init(false, Failures.Num());

		TArray<FMadDebrisCluster> Clusters;
		TArray<int32> Queue;

		for (int32 Start = 0; Start < Failures.Num(); ++Start)
		{
			if (Visited[Start])
			{
				continue;
			}

			FMadDebrisCluster& Cluster = Clusters.AddDefaulted_GetRef();
			TSet<FIntVector> Members;

			Queue.Reset();
			Queue.Add(Start);
			Visited[Start] = true;

			for (int32 Cursor = 0; Cursor < Queue.Num(); ++Cursor)
			{
				const FMadStructuralFailureRecord& Failure = Failures[Queue[Cursor]];

				FMadDebrisBlock& Block = Cluster.Blocks.AddDefaulted_GetRef();
				Block.Position = Failure.Position;
				Block.Voxel = Failure.Voxel;
				Block.MassKg = Materials.Get(Failure.Voxel.BlockTypeID).MassKg;
				Cluster.MassKg += Block.MassKg;
				Members.Add(Failure.Position);

				for (const FIntVector& Offset : Offsets)
				{
					if (const int32* Neighbour = ByPosition.Find(Failure.Position + Offset))
					{
						if (!Visited[*Neighbour])
						{
							Visited[*Neighbour] = true;
							Queue.Add(*Neighbour);
						}
					}
				}
			}

			for (int32 Index = 0; Index < Cluster.Blocks.Num(); ++Index)
			{
				if (!Members.Contains(Cluster.Blocks[Index].Position - FIntVector(0, 0, 1)))
				{
					Cluster.BottomBlocks.Add(Index);
				}
			}
		}

		return Clusters;
	}

	namespace
	{
		/** True if every bottom block can move one more voxel down. */
		bool CanDropOneMore(const FMadDebrisCluster& Cluster, TFunctionRef<bool(const FIntVector&)> IsFree)
		{
			for (int32 Bottom : Cluster.BottomBlocks)
			{
				if (!IsFree(Cluster.GetLandedPosition(Bottom) - FIntVector(0, 0, 1)))
				{
					return false;
				}
			}
			return true;
		}
	}

	bool Advance(FMadDebrisCluster& Cluster, float DeltaSeconds, TFunctionRef<bool(const FIntVector&)> IsFree, int32 MaxDrop)
	{
		if (Cluster.bLanded)
		{
			return true;
		}

		// Semi-implicit Euler. Error against the analytic fall is O(dt) in the
		// landing speed; at 60 Hz that is a few percent, well inside how
		// precisely impact damage needs to be tuned.
		Cluster.Velocity += Gravity * DeltaSeconds;
		Cluster.FallDistance += Cluster.Velocity * DeltaSeconds;

		while (static_cast<float>(Cluster.Dropped + 1) <= Cluster.FallDistance)
		{
			if (Cluster.Dropped >= MaxDrop || !CanDropOneMore(Cluster, IsFree))
			{
				// The step overshot the contact by (FallDistance - Dropped). Rewind
				// the speed to what it was AT the contact (v^2 = u^2 - 2gs), or
				// every landing would carry up to a voxel's worth of extra energy.
				const float Overshoot = Cluster.FallDistance - static_cast<float>(Cluster.Dropped);
				Cluster.Velocity = FMath::Sqrt(FMath::Max(0.0f,
					Cluster.Velocity * Cluster.Velocity - 2.0f * Gravity * Overshoot));
				Cluster.FallDistance = static_cast<float>(Cluster.Dropped);
				Cluster.bLanded = true;
				return true;
			}
			++Cluster.Dropped;
		}

		// Also land a cluster that cannot move at all, without waiting for it to
		// accumulate a whole voxel of fall first.
		if (Cluster.Dropped == 0 && !CanDropOneMore(Cluster, IsFree))
		{
			Cluster.FallDistance = 0.0f;
			Cluster.Velocity = 0.0f;
			Cluster.bLanded = true;
			return true;
		}

		return false;
	}

	void AdvanceToLanding(FMadDebrisCluster& Cluster, TFunctionRef<bool(const FIntVector&)> IsFree, int32 MaxDrop)
	{
		constexpr float Step = 1.0f / 60.0f;

		// Bounded: a free fall of MaxDrop voxels takes sqrt(2 * MaxDrop / g)
		// seconds, so this is far beyond what any valid fall needs.
		const int32 MaxSteps = FMath::CeilToInt(FMath::Sqrt(2.0f * (MaxDrop + 2) / Gravity) / Step) + 60;
		for (int32 Index = 0; Index < MaxSteps && !Advance(Cluster, Step, IsFree, MaxDrop); ++Index)
		{
		}

		if (!Cluster.bLanded)
		{
			Cluster.FallDistance = static_cast<float>(Cluster.Dropped);
			Cluster.bLanded = true;
		}
	}

	void ComputeImpacts(const FMadDebrisCluster& Cluster, TFunctionRef<bool(const FIntVector&)> IsFree,
		TArray<FMadDebrisImpact>& OutImpacts)
	{
		OutImpacts.Reset();
		if (Cluster.Dropped == 0 || Cluster.Velocity <= 0.0f)
		{
			return;
		}

		for (int32 Bottom : Cluster.BottomBlocks)
		{
			const FIntVector Below = Cluster.GetLandedPosition(Bottom) - FIntVector(0, 0, 1);
			if (!IsFree(Below))
			{
				OutImpacts.Add(FMadDebrisImpact{ Below, 0.0f });
			}
		}

		if (OutImpacts.Num() == 0)
		{
			return;
		}

		const float Energy = 0.5f * Cluster.MassKg * Cluster.Velocity * Cluster.Velocity;
		const float PerContact = Energy / static_cast<float>(OutImpacts.Num());
		for (FMadDebrisImpact& Impact : OutImpacts)
		{
			Impact.EnergyJ = PerContact;
		}
	}

	void ComputeRubble(const FMadDebrisCluster& Cluster, int32 KeepOneIn, TArray<TPair<FIntVector, int32>>& OutBlockIndexAtPosition)
	{
		OutBlockIndexAtPosition.Reset();
		KeepOneIn = FMath::Max(1, KeepOneIn);

		struct FColumn
		{
			int32 LowestZ = MAX_int32;
			TArray<int32> Kept;
		};

		// TMap iteration order is insertion order here because nothing is
		// removed, which keeps the output deterministic.
		TMap<FIntPoint, FColumn> Columns;

		for (int32 Index = 0; Index < Cluster.Blocks.Num(); ++Index)
		{
			const FIntVector Landed = Cluster.GetLandedPosition(Index);
			FColumn& Column = Columns.FindOrAdd(FIntPoint(Landed.X, Landed.Y));
			Column.LowestZ = FMath::Min(Column.LowestZ, Landed.Z);

			const FIntVector& P = Cluster.Blocks[Index].Position;
			const uint32 Hash = HashCombineFast(HashCombineFast(GetTypeHash(P.X), GetTypeHash(P.Y)), GetTypeHash(P.Z) * 2654435761u);
			if ((Hash % static_cast<uint32>(KeepOneIn)) == 0)
			{
				Column.Kept.Add(Index);
			}
		}

		for (const TPair<FIntPoint, FColumn>& Pair : Columns)
		{
			for (int32 K = 0; K < Pair.Value.Kept.Num(); ++K)
			{
				OutBlockIndexAtPosition.Emplace(
					FIntVector(Pair.Key.X, Pair.Key.Y, Pair.Value.LowestZ + K), Pair.Value.Kept[K]);
			}
		}
	}

	void GetPawnVoxels(const FVector& Location, float HalfHeight, float Radius, TArray<FIntVector>& OutVoxels, float Skin)
	{
		Radius += Skin;
		// A few centimetres of slack on every side: a capsule brushing a voxel
		// boundary does not claim the voxel beyond it, or a survivor standing next
		// to a wall would keep rubble out of the wall's own column.
		constexpr double Slack = 5.0;
		const double Size = MadFall::VoxelSizeUU;
		const FIntVector Min(
			FMath::FloorToInt32((Location.X - Radius + Slack) / Size),
			FMath::FloorToInt32((Location.Y - Radius + Slack) / Size),
			FMath::FloorToInt32((Location.Z - HalfHeight + Slack) / Size));
		const FIntVector Max(
			FMath::FloorToInt32((Location.X + Radius - Slack) / Size),
			FMath::FloorToInt32((Location.Y + Radius - Slack) / Size),
			FMath::FloorToInt32((Location.Z + HalfHeight + Skin - Slack) / Size));
		for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				for (int32 X = Min.X; X <= Max.X; ++X)
				{
					OutVoxels.Add(FIntVector(X, Y, Z));
				}
			}
		}
	}

	float ComputeSweptHitEnergy(const FMadDebrisCluster& Cluster, int32 FromDropped, const FIntVector& Feet, int32 HeightVoxels)
	{
		if (Cluster.Dropped <= FromDropped || HeightVoxels <= 0)
		{
			return 0.0f;
		}

		const int32 PawnTop = Feet.Z + HeightVoxels - 1;
		for (const int32 Index : Cluster.BottomBlocks)
		{
			const FIntVector& P = Cluster.Blocks[Index].Position;
			if (P.X != Feet.X || P.Y != Feet.Y)
			{
				continue;
			}

			// Voxels this block newly entered since FromDropped: [P.Z - Dropped, P.Z - FromDropped - 1].
			const int32 EnteredLow = P.Z - Cluster.Dropped;
			const int32 EnteredHigh = P.Z - FromDropped - 1;
			if (EnteredHigh < Feet.Z || EnteredLow > PawnTop)
			{
				continue;
			}

			float ColumnMass = 0.0f;
			for (const FMadDebrisBlock& Block : Cluster.Blocks)
			{
				if (Block.Position.X == Feet.X && Block.Position.Y == Feet.Y)
				{
					ColumnMass += Block.MassKg;
				}
			}
			return 0.5f * ColumnMass * FMath::Square(Cluster.Velocity);
		}
		return 0.0f;
	}

	void RollCollapseDrops(const FMadDebrisCluster& Cluster, const TArray<TPair<FIntVector, int32>>& Rubble,
		TFunctionRef<const FMadLootTableDefinition*(uint16 BlockTypeId)> CollapseTableOf,
		const FMadGameplayDefinitions& Definitions, FRandomStream& Random, TArray<FMadItemStack>& OutStacks)
	{
		OutStacks.Reset();

		TSet<int32> BecameRubble;
		for (const TPair<FIntVector, int32>& Entry : Rubble)
		{
			BecameRubble.Add(Entry.Value);
		}

		// Merged through an inventory so stack limits and equipment rules are the
		// usual ones. Sized generously; overflow is kept rather than lost.
		FMadInventory Merged(64);
		TArray<FMadItemStack> Rolled;
		for (int32 Index = 0; Index < Cluster.Blocks.Num(); ++Index)
		{
			if (BecameRubble.Contains(Index))
			{
				continue;
			}
			const FMadLootTableDefinition* Table = CollapseTableOf(Cluster.Blocks[Index].Voxel.BlockTypeID);
			if (Table == nullptr)
			{
				continue;
			}

			MadFall::Loot::Roll(*Table, Definitions, FMadLootContext(), Random, Rolled);
			for (const FMadItemStack& Stack : Rolled)
			{
				if (const int32 Left = Merged.Add(Stack, Definitions); Left > 0)
				{
					FMadItemStack Overflow = Stack;
					Overflow.Count = Left;
					OutStacks.Add(Overflow);
				}
			}
		}

		for (const FMadItemStack& Stack : Merged.GetSlots())
		{
			if (!Stack.IsEmpty())
			{
				OutStacks.Add(Stack);
			}
		}
	}
}

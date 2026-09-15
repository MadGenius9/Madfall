// Copyright MadFall. All Rights Reserved.

#include "MadTraps.h"

#include "MadBlockDefinition.h"
#include "MadBlockRegistry.h"
#include "MadVoxelWorldSubsystem.h"

namespace
{
	/**
	 * Runtime id -> passes-through, built on first use. The registry does not
	 * change after load, and a flat array beats a map lookup in A*.
	 */
	const TBitArray<>& WalkThroughIds()
	{
		static TBitArray<> Ids;
		static int32 BuiltFor = -1;
		const FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();
		if (BuiltFor != Registry.Num())
		{
			Ids.Init(false, 65536);
			for (const FMadBlockEntry& Entry : Registry.GetEntries())
			{
				if (Entry.Definition.Collision == EMadBlockCollisionKind::None && !Entry.Definition.bLiquid)
				{
					Ids[Entry.RuntimeId] = true;
				}
			}
			BuiltFor = Registry.Num();
		}
		return Ids;
	}
}

bool MadFall::Traps::IsWalkThrough(uint16 RuntimeId)
{
	return WalkThroughIds()[RuntimeId];
}

FMadVoxel MadFall::Traps::ForPathing(const FMadVoxel& Voxel)
{
	if (Voxel.BlockTypeID == MadFall::BlockTypeAir || !IsWalkThrough(Voxel.BlockTypeID))
	{
		return Voxel;
	}
	FMadVoxel Open = Voxel;
	Open.Density = 0;
	return Open;
}

FMadTrapContact MadFall::Traps::FindContact(const UMadVoxelWorldSubsystem& VoxelWorld, const FVector& Location, float HalfHeightCm)
{
	const FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();
	for (const float Height : { -HalfHeightCm + 20.0f, 0.0f })
	{
		const FVector At = (Location + FVector(0.0, 0.0, Height)) / MadFall::VoxelSizeUU;
		const FIntVector V(FMath::FloorToInt32(At.X), FMath::FloorToInt32(At.Y), FMath::FloorToInt32(At.Z));
		const FMadVoxel Voxel = VoxelWorld.GetVoxel(V.X, V.Y, V.Z);
		if (Voxel.BlockTypeID == MadFall::BlockTypeAir || !IsWalkThrough(Voxel.BlockTypeID))
		{
			continue;
		}
		const FMadBlockDefinitionData* Block = Registry.FindDefinition(Voxel.BlockTypeID);
		if (Block != nullptr && Block->HasTrap())
		{
			return { Block, V };
		}
	}
	return FMadTrapContact();
}

int32& MadFall::Traps::TotalHits()
{
	static int32 Hits = 0;
	return Hits;
}

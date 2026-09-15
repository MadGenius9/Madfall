// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UMadVoxelWorldSubsystem;
struct FMadBlockDefinitionData;
struct FMadVoxel;

/** A trap block a creature is standing in. */
struct FMadTrapContact
{
	const FMadBlockDefinitionData* Block = nullptr;
	FIntVector Voxel = FIntVector::ZeroValue;

	bool IsValid() const { return Block != nullptr; }
};

/**
 * Traps: blocks with `trap` stats that hurt, slow, and wear out under whatever
 * walks through them.
 *
 * A trap only works if creatures walk INTO it, so trap blocks - and every other
 * block with no collision, like an open door or a ladder - read as open space to
 * the pathfinder. Before this, an open door was a wall to be broken down.
 */
namespace MadFall::Traps
{
	/** True for a block creatures pass through: collision none. Cheap enough for the pathfinder's inner loop. */
	MADFALLGAMEPLAY_API bool IsWalkThrough(uint16 RuntimeId);

	/**
	 * The voxel as a walker sees it: walk-through blocks become air, so paths
	 * lead into traps and doorways instead of digging through them.
	 */
	MADFALLGAMEPLAY_API FMadVoxel ForPathing(const FMadVoxel& Voxel);

	/** The trap at a creature's feet or chest, if any. Location is the capsule centre, cm. */
	MADFALLGAMEPLAY_API FMadTrapContact FindContact(const UMadVoxelWorldSubsystem& VoxelWorld, const FVector& Location, float HalfHeightCm);

	/** Lifetime count of trap hits on creatures, for CI. */
	MADFALLGAMEPLAY_API int32& TotalHits();
}

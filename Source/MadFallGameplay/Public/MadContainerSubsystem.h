// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadInventory.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadContainerSubsystem.generated.h"

class UMadVoxelWorldSubsystem;

/** One container's state. */
struct MADFALLGAMEPLAY_API FMadContainer
{
	FMadInventory Contents{ 16 };

	/** The loot table the contents came from, NAME_None for a player-placed box. */
	FName LootTable;
	int32 Tier = 1;
	bool bRolled = false;
};

/**
 * Containers: every voxel whose block has the `block.container` tag.
 *
 * LOOT IS ROLLED ON FIRST OPEN, NOT AT GENERATION
 *   A POI's loot markers are data in the prefab. When a player first opens the
 *   container standing on a marker, its table is rolled with a seed derived
 *   from the world seed and the voxel position, at the game stage of that
 *   moment. So chunks never store loot they have not been asked for, the same
 *   world seed gives the same crate contents to anyone who opens it on the same
 *   day, and a crate opened on day 20 is better than it would have been on day 1
 *   - the 7 Days to Die "game stage" rule.
 *
 * A container block with no marker under it (one the player placed) is simply
 * empty storage.
 *
 * Contents are kept in memory keyed by voxel position and dropped if the block
 * is destroyed. Persisting them with the region file is a known Phase 4 gap.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadContainerSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** True if the voxel holds a container block. */
	bool IsContainer(const FIntVector& Voxel) const;

	/**
	 * Opens a container, rolling its loot the first time. Returns nullptr if the
	 * voxel is not a container.
	 */
	FMadContainer* Open(const FIntVector& Voxel, int32 GameStage);

	/** A container already opened (or restored from a save), without rolling anything. */
	FMadContainer* Find(const FIntVector& Voxel) { return Containers.Find(Voxel); }
	const FMadContainer* Find(const FIntVector& Voxel) const { return Containers.Find(Voxel); }

	/** Moves everything that fits into Inventory. Returns the number of items moved. */
	int32 TakeAll(const FIntVector& Voxel, FMadInventory& Inventory, int32 GameStage);

	int32 NumKnownContainers() const { return Containers.Num(); }

	/** Save support: every container that has been opened or holds items. */
	void ExportState(TArray<struct FMadContainerSaveData>& Out) const;
	void ImportState(const TArray<struct FMadContainerSaveData>& In);

private:
	void HandleVoxelChanged(const FIntVector& Position, const struct FMadVoxel& Before, const struct FMadVoxel& After);

	/** The loot marker at a voxel, from the POI planner. */
	bool FindLootMarker(const FIntVector& Voxel, FName& OutTable, int32& OutTier) const;

	UPROPERTY(Transient)
	TObjectPtr<UMadVoxelWorldSubsystem> VoxelWorld;

	TMap<FIntVector, FMadContainer> Containers;
	FDelegateHandle VoxelChangedHandle;
};

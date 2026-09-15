// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallCoordinates.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadChunkStreamingSubsystem.generated.h"

class UMadVoxelWorldSubsystem;

/**
 * Keeps the chunks around every player loaded, and releases the rest.
 *
 * Sources are player pawns (or their controllers' view point when there is no
 * pawn yet), plus any explicit sources a system registers - the horde director
 * keeps its spawn ring loaded that way. Load requests go out nearest-first
 * through UMadVoxelWorldSubsystem::RequestLoadChunk (worker threads); unloads
 * happen past a hysteresis margin so walking back and forth across a chunk
 * border does not thrash the disk.
 *
 * Game worlds only: the editor world is driven by console commands and tests,
 * and silently unloading what a test just loaded would be baffling.
 */
UCLASS()
class MADFALLCORE_API UMadChunkStreamingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Adds a named streaming source at a world location (cm). Re-adding updates it. */
	void SetExtraSource(FName Name, const FVector& WorldLocation, int32 RadiusChunks);
	void RemoveExtraSource(FName Name);

	/**
	 * True when every chunk within RadiusChunks horizontally and one layer
	 * vertically of a location is loaded. The player waits for this before
	 * gravity is enabled at spawn.
	 */
	bool IsAreaLoaded(const FVector& WorldLocation, int32 RadiusChunks) const;

	int32 NumPendingLoads() const { return LastPendingLoads; }
	FString DescribeStatus() const;

private:
	struct FSource
	{
		FMadChunkCoord Centre;
		int32 Radius = 0;
	};

	void GatherSources(TArray<FSource>& OutSources) const;
	void RebuildQueues(const TArray<FSource>& Sources, int32 Vertical);
	const TArray<FIntVector>& GetSortedOffsets(int32 Radius, int32 Vertical);

	UPROPERTY(Transient)
	TObjectPtr<UMadVoxelWorldSubsystem> VoxelWorld;

	TMap<FName, TPair<FVector, int32>> ExtraSources;

	int32 LastPendingLoads = 0;

	/** Chunks to load, nearest first, and chunks to unload, rebuilt on a source change. */
	TArray<FMadChunkCoord> LoadQueue;
	int32 LoadCursor = 0;
	TArray<FMadChunkCoord> UnloadQueue;
	int32 UnloadCursor = 0;
	TArray<FSource> LastSources;
	int32 LastVertical = -1;
	double NextRescanSeconds = 0.0;
	int64 TotalRescans = 0;
	TMap<FIntPoint, TArray<FIntVector>> OffsetCache;
	int64 TotalRequested = 0;
	int64 TotalUnloaded = 0;
};

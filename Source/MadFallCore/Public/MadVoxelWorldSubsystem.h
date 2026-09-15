// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "MadChunkStorage.h"
#include "MadFallCoordinates.h"
#include "MadRegionFile.h"
#include "MadWorldGenerator.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include "Templates/SharedPointer.h"
#include "MadVoxelWorldSubsystem.generated.h"

class FMadBlockRegistry;
class FMadPrefabRegistry;

/**
 * One loaded chunk.
 *
 * The lock guards Storage. Worker threads (meshing, and later the structural
 * solver) take it for read; the game thread takes it for write during an edit.
 * Edits are small and bounded, so a reader-writer lock beats the full
 * copy-on-write swap here - the swap is what Phase 2 will need once a mesher
 * job can be mid-flight for tens of milliseconds.
 */
struct MADFALLCORE_API FMadChunk
{
	FMadChunkCoord Coord;
	FMadChunkStorage Storage;

	/** Differs from what is on disk. */
	bool bDirty = false;

	/** Contains at least one player edit, so it must be persisted even if it looks generatable. */
	bool bPlayerModified = false;

	mutable FRWLock Lock;
};

using FMadChunkPtr = TSharedPtr<FMadChunk, ESPMode::ThreadSafe>;

/**
 * Fired on the game thread whenever a chunk's contents change - a load, an
 * edit, or a fill.
 *
 * This is the hook the SetVoxel funnel was built for. The mesher subscribes to
 * it in Phase 2, the structural solver in Phase 4, and the replication delta
 * encoder in Phase 6, none of which need to know about each other.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FMadOnChunkChanged, const FMadChunkCoord& /*Coord*/);

/** Fired when a chunk leaves memory, so listeners can release derived data. */
DECLARE_MULTICAST_DELEGATE_OneParam(FMadOnChunkUnloaded, const FMadChunkCoord& /*Coord*/);

/**
 * Fired when a chunk enters memory, whether loaded from disk or generated.
 *
 * Separate from OnChunkChanged because some listeners need to do whole-chunk
 * work exactly once on arrival - the structural solver seeds every construction
 * block in the chunk - and must not repeat a 32768-voxel scan for every single
 * block edit afterwards.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FMadOnChunkLoaded, const FMadChunkCoord& /*Coord*/);

/**
 * Fired for every individual voxel write that goes through SetVoxel, with the
 * value before and after.
 *
 * OnChunkChanged says "something in this chunk moved", which is enough to
 * remesh. The structural solver needs to know exactly which block moved and
 * what it used to be - removing a steel beam and adding a wood plank are very
 * different events for the structure around them.
 */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FMadOnVoxelChanged,
	const FIntVector& /*WorldPosition*/, const FMadVoxel& /*Before*/, const FMadVoxel& /*After*/);

/**
 * The voxel world.
 *
 * Owns loaded chunks, the region files behind them, and the single entry point
 * through which voxels change.
 *
 * WHY EVERY EDIT GOES THROUGH SetVoxel:
 * MadFall is single-player today and multiplayer in Phase 6. Letting arbitrary
 * call sites reach into FMadChunkStorage would make server authority a rewrite
 * rather than an addition - the validation, the dirty marking, the structural
 * re-solve and (later) the replication delta all hang off this one function.
 * It costs nothing now to keep the funnel.
 */
UCLASS()
class MADFALLCORE_API UMadVoxelWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem

	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject

	// --- world configuration ------------------------------------------------

	/** Absolute path of the world folder. Defaults to Saved/MadFallWorlds/DevWorld. */
	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	FString GetWorldDirectory() const { return WorldDirectory; }

	/** Closes every open region and points the subsystem at a different world folder. */
	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	void SetWorldDirectory(const FString& InDirectory);

	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	int64 GetSeed() const { return static_cast<int64>(WorldSeed); }

	/** True for a world that is never saved (the title screen's backdrop). */
	bool IsReadOnly() const { return bReadOnly; }

	/** From world.json; normal for the title backdrop and worlds from before difficulty. */
	FName GetDifficulty() const { return Difficulty; }

	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	void SetSeed(int64 InSeed);

	// --- chunks -------------------------------------------------------------

	/**
	 * Loads a chunk from disk, or creates an empty one if the region has never
	 * held it. Synchronous - intended for console commands, tests and the
	 * initial world load. Streaming (Phase 6) uses the async path.
	 */
	bool LoadChunkSync(const FMadChunkCoord& Coord, FString& OutError);

	/** Queues an off-thread load. The chunk appears in a later Tick. */
	void RequestLoadChunk(const FMadChunkCoord& Coord);

	/** True while an async load for this chunk is in flight. */
	bool IsChunkLoading(const FMadChunkCoord& Coord) const { return InFlightLoads.Contains(Coord); }

	int32 NumLoadsInFlight() const { return InFlightLoads.Num(); }

	/** Saves if dirty, then drops the chunk from memory. */
	bool UnloadChunk(const FMadChunkCoord& Coord, bool bSave, FString& OutError);

	/** The loaded chunk, or null. */
	FMadChunkPtr FindChunk(const FMadChunkCoord& Coord) const;

	/** Every resident chunk coordinate. The mesher uses this for a full rebuild. */
	void GetLoadedChunkCoords(TArray<FMadChunkCoord>& OutCoords) const;

	/**
	 * Loads a cube of chunks centred on Coord, generating any that are absent.
	 * Synchronous: this is a tool for tests and console work, not streaming.
	 */
	int32 LoadArea(const FMadChunkCoord& Centre, int32 RadiusInChunks, int32 VerticalRadius, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	int32 NumLoadedChunks() const;

	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	int32 NumPendingLoads() const;

	/** Bytes of voxel storage currently resident. */
	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	int64 GetLoadedVoxelBytes() const;

	// --- voxels -------------------------------------------------------------

	/**
	 * Reads a voxel. Returns air for an unloaded chunk or an out-of-range Z -
	 * callers that need to distinguish those use IsChunkLoaded.
	 */
	FMadVoxel GetVoxel(int32 WorldX, int32 WorldY, int32 WorldZ) const;

	/**
	 * Writes a voxel. The single funnel every edit passes through.
	 *
	 * Returns false if Z is outside the world or the chunk is not loaded;
	 * MadFall never silently loads a chunk as a side effect of a write, because
	 * that turns one misplaced edit into a synchronous disk read on the game
	 * thread.
	 */
	bool SetVoxel(int32 WorldX, int32 WorldY, int32 WorldZ, const FMadVoxel& Voxel);

	/** Blueprint-friendly wrappers over the packed struct. */
	UFUNCTION(BlueprintCallable, Category = "MadFall|Voxel")
	FMadVoxelState GetVoxelState(int32 WorldX, int32 WorldY, int32 WorldZ) const;

	UFUNCTION(BlueprintCallable, Category = "MadFall|Voxel")
	bool SetVoxelState(int32 WorldX, int32 WorldY, int32 WorldZ, const FMadVoxelState& State);

	/** Replaces every voxel of a loaded chunk. Used by console commands and tests. */
	bool FillChunk(const FMadChunkCoord& Coord, const FMadVoxel& Voxel);

	// --- persistence --------------------------------------------------------

	/** Writes every dirty chunk and flushes every open region. */
	bool SaveAll(FString& OutError);

	/**
	 * The autosave: copies every dirty chunk on the game thread and writes and
	 * disk-flushes the copies on a worker. Returns the number of chunks copied.
	 *
	 * WHY: SaveAll on the game thread serialises every dirty chunk and skips the
	 * disk flush (a 5-20 ms stall per region), so a mid-play save was either a
	 * hitch or not safe against power loss. A copy costs a memcpy of each chunk's
	 * packed storage; compaction, serialisation and FlushFileBuffers all happen
	 * on the worker, where the region file flushes each step before the next.
	 * The chunks are marked clean when copied - an edit after that dirties them
	 * again for the next save - and a failed write marks them dirty again on the
	 * next tick. Loads of a chunk with a write outstanding wait for it, and a
	 * later save of the same chunk (an unload) runs after it, so older bytes can
	 * never land over newer ones.
	 */
	int32 SaveAllAsync();

	/** Blocks until every outstanding background write has finished. Shutdown, tests and `mad.world.autosave wait`. */
	void WaitForSaves() { WaitForAllPendingSaves(); }

	/** Saves one chunk without unloading it. */
	bool SaveChunk(const FMadChunkCoord& Coord, FString& OutError);

	/** Closes every open region file, flushing first. */
	bool CloseAllRegions(FString& OutError);

	// --- diagnostics --------------------------------------------------------

	FString DescribeWorld() const;
	FString DescribeChunk(const FMadChunkCoord& Coord) const;
	FString DescribeRegion(const FMadRegionCoord& Coord);

	/** The process-wide block registry, built on first use. */
	static FMadBlockRegistry& GetBlockRegistry();

	/** The process-wide biome registry, built on first use. */
	static FMadBiomeRegistry& GetBiomeRegistry();

	/** The process-wide prefab registry, built on first use. */
	static FMadPrefabRegistry& GetPrefabRegistry();

	// --- world generation ---------------------------------------------------

	/**
	 * Rebuilds the generator with new settings and re-derives the world's
	 * generation version.
	 *
	 * Changing these on a world that already has chunks on disk is what the
	 * region loader's generation-version check exists to catch.
	 */
	void SetWorldGenSettings(const FMadWorldGenSettings& InSettings);

	const FMadWorldGenSettings& GetWorldGenSettings() const { return WorldGenSettings; }

	const FMadWorldGenerator* GetWorldGenerator() const { return WorldGenerator.Get(); }

	/** Turns generation off, so an ungenerated chunk loads as empty air. For tests. */
	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	void SetGenerationEnabled(bool bEnabled) { bGenerationEnabled = bEnabled; }

	UFUNCTION(BlueprintCallable, Category = "MadFall|World")
	bool IsGenerationEnabled() const { return bGenerationEnabled; }

	// --- change notification ------------------------------------------------

	FMadOnChunkChanged& OnChunkChanged() { return ChunkChangedDelegate; }
	FMadOnChunkUnloaded& OnChunkUnloaded() { return ChunkUnloadedDelegate; }
	FMadOnChunkLoaded& OnChunkLoaded() { return ChunkLoadedDelegate; }
	FMadOnVoxelChanged& OnVoxelChanged() { return VoxelChangedDelegate; }

	/** True if the chunk containing a world voxel is resident. */
	bool IsVoxelLoaded(int32 WorldX, int32 WorldY, int32 WorldZ) const;

	/**
	 * Snapshots a chunk and its immediate neighbours into a padded sample grid.
	 *
	 * The mesher needs one voxel of margin on every side to find surface
	 * crossings and compute gradients at chunk boundaries. A copy, rather than
	 * letting the worker hold read locks for the whole job, means an edit never
	 * waits on meshing.
	 *
	 * Returns false if the centre chunk is not loaded. Absent neighbours are
	 * filled with air, which is the correct assumption for an unloaded edge.
	 */
	bool SnapshotChunkWithMargin(const FMadChunkCoord& Coord, struct FMadChunkSampleGrid& OutGrid) const;

	/** The 27 chunks a snapshot reads, [x][y][z] offset by one. Absent = unloaded. */
	struct FSnapshotSources
	{
		FMadChunkCoord Coord;
		FMadChunkPtr Chunks[3][3][3];
	};

	/**
	 * The game-thread half of a snapshot: 27 map lookups, microseconds. The
	 * shared pointers keep the chunks alive even if they unload before the copy.
	 * Returns false if the centre chunk is not loaded.
	 */
	bool GatherSnapshotSources(const FMadChunkCoord& Coord, FSnapshotSources& OutSources) const;

	/**
	 * The copying half, safe on any thread: takes each chunk's read lock for
	 * the ~0.2 ms copy. An edit to one of those chunks at that moment waits for
	 * the lock - bounded by one copy - instead of every launch costing the game
	 * thread the copy.
	 */
	static void SnapshotFromSources(const FSnapshotSources& Sources, struct FMadChunkSampleGrid& OutGrid);

private:
	/** Opens (creating if needed) the region holding Coord. Caller holds RegionLock. */
	FMadRegionFile* GetOrOpenRegion(const FMadRegionCoord& Coord, FString& OutError);

	/** Does the region read and deserialize. Safe to call from a worker thread. */
	bool LoadChunkStorage(const FMadChunkCoord& Coord, FMadChunkStorage& OutStorage, FString& OutError);

	bool WriteChunkStorage(const FMadChunkCoord& Coord, const FMadChunkStorage& Storage,
		bool bPlayerModified, FString& OutError);

	FString WorldDirectory;
	uint64 WorldSeed = 0;

	/** Never written to disk: the title screen's backdrop world. */
	bool bReadOnly = false;
	FName Difficulty = FName(TEXT("normal"));
	uint64 WorldGenVersion = 0;

	TMap<FMadChunkCoord, FMadChunkPtr> LoadedChunks;

	/**
	 * Region files are plain file handles with mutable in-memory indices, so
	 * exactly one thread may touch them at a time. Chunk loads and saves are
	 * the only callers, and both are already off the frame's critical path.
	 */
	mutable FCriticalSection RegionLock;
	TMap<FMadRegionCoord, TUniquePtr<FMadRegionFile>> OpenRegions;

	/** Completed async loads waiting to be published on the game thread. */
	struct FPendingLoad
	{
		FMadChunkCoord Coord;
		FMadChunkStorage Storage;
		FString Error;
		bool bSucceeded = false;
	};

	mutable FCriticalSection PendingLock;
	TArray<FPendingLoad> CompletedLoads;
	TSet<FMadChunkCoord> InFlightLoads;

	/**
	 * Chunks being written by a worker - unloaded dirty chunks, and the copies
	 * an autosave took. Game thread only. A load of a chunk with a save
	 * outstanding waits (sync) or is deferred (async), so nothing ever reads a
	 * region slot older than the chunk that just left it.
	 */
	TMap<FMadChunkCoord, UE::Tasks::FTask> PendingSaves;

	/** Chunks whose background write failed, to mark dirty again on the game thread. Guarded by PendingLock. */
	TArray<TSharedPtr<FMadChunk, ESPMode::ThreadSafe>> FailedSaves;

	/** Waits for an outstanding save of Coord, if any. */
	void WaitForPendingSave(const FMadChunkCoord& Coord);
	void WaitForAllPendingSaves();
	bool HasPendingSave(const FMadChunkCoord& Coord);

	/** Rebuilt whenever the seed or generation settings change. */
	TUniquePtr<FMadWorldGenerator> WorldGenerator;
	FMadWorldGenSettings WorldGenSettings;
	bool bGenerationEnabled = true;

	FMadOnChunkChanged ChunkChangedDelegate;
	FMadOnChunkUnloaded ChunkUnloadedDelegate;
	FMadOnChunkLoaded ChunkLoadedDelegate;
	FMadOnVoxelChanged VoxelChangedDelegate;
};

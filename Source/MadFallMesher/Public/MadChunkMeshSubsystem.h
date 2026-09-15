// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "MadChunkMesher.h"
#include "MadFallCoordinates.h"
#include "MadMeshBuffers.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadChunkMeshSubsystem.generated.h"

class AActor;
class UMadChunkMeshComponent;
class UMadVoxelWorldSubsystem;
class UMaterialInterface;

/**
 * Keeps chunk geometry in sync with chunk contents.
 *
 * Listens to UMadVoxelWorldSubsystem::OnChunkChanged, snapshots the affected
 * chunk plus its margin, meshes it on a worker thread, and publishes the result
 * to a component on the game thread.
 *
 * THE GAME-THREAD BUDGET:
 * Exactly two things happen on the game thread per rebuild - the sample-grid
 * snapshot and the component apply - and both are rate limited, because both
 * scale with chunk size rather than with edit size. Everything between them
 * runs on a task. Mining one block therefore costs a snapshot (a ~157 KB copy)
 * this frame and an apply in some later frame, not a meshing pass inline.
 *
 * COALESCING:
 * Breaking a block fires OnChunkChanged once, but breaking a wall fires it
 * dozens of times in one frame. Dirty chunks go into a set, not a queue, so a
 * chunk that changed forty times this frame is meshed once.
 */
UCLASS()
class MADFALLMESHER_API UMadChunkMeshSubsystem : public UTickableWorldSubsystem
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

	/** Queues a chunk for remeshing. Idempotent within a frame. */
	void MarkChunkDirty(const FMadChunkCoord& Coord);

	/** Meshes a chunk immediately on the calling thread and publishes it. Console and tests only. */
	bool RebuildChunkNow(const FMadChunkCoord& Coord, FString& OutError);

	/** Drops the component for a chunk that left memory. */
	void ReleaseChunk(const FMadChunkCoord& Coord);

	/** Queues every currently loaded chunk. */
	int32 RebuildAll();

	/**
	 * Meshes every loaded chunk synchronously, blocking until done.
	 *
	 * For screenshots, tests and offline captures, where a partially meshed
	 * world is worse than a long frame. Never for gameplay.
	 */
	int32 RebuildAllNow();

	UFUNCTION(BlueprintCallable, Category = "MadFall|Mesh")
	int32 NumMeshedChunks() const { return Components.Num(); }

	UFUNCTION(BlueprintCallable, Category = "MadFall|Mesh")
	int32 NumDirtyChunks() const { return DirtyChunks.Num(); }

	UFUNCTION(BlueprintCallable, Category = "MadFall|Mesh")
	int32 NumMeshJobsInFlight() const { return InFlight.Num(); }

	/**
	 * True when a chunk has no pending or running rebuild, so its current mesh
	 * (or deliberate lack of one) matches its voxels. Collision cooks
	 * asynchronously and may still lag this by a few frames.
	 */
	bool IsChunkMeshSettled(const FMadChunkCoord& Coord) const { return !DirtyChunks.Contains(Coord) && !InFlight.Contains(Coord); }

	/** True once a chunk has a mesh component. A rebuild keeps the old one, and its collision, until the new mesh lands. */
	bool HasChunkMesh(const FMadChunkCoord& Coord) const { return Components.Contains(Coord); }

	FString DescribeStats() const;

	/** Zeroes worst-case timings and counters (not throughput totals). */
	void ResetStats();

	/**
	 * Meshes a chunk and reports what came out, without touching any component.
	 *
	 * Answers the questions a screenshot cannot: which mesher produced which
	 * section, how many voxels each path saw, and what material class each
	 * section resolved to.
	 */
	FString InspectChunkMesh(const FMadChunkCoord& Coord, FString& OutError);

	/** The default voxel material (mad.mesh.SectionMaterial), for classes whose surface names none. */
	UMaterialInterface* GetSectionMaterial();

	/**
	 * The material for a material class: its surface definition's material if it
	 * names one that loads, otherwise the default. Resolved once per class and
	 * cached. A surface whose asset is missing - a content mod whose pak did not
	 * mount - warns once and falls back rather than rendering nothing.
	 */
	UMaterialInterface* GetMaterialForClass(FName MaterialClass);

private:
	void HandleChunkChanged(const FMadChunkCoord& Coord);
	void HandleChunkUnloaded(const FMadChunkCoord& Coord);

	UMadVoxelWorldSubsystem* GetVoxelWorld() const;

	/** Spawns (once) the actor every chunk component attaches to. */
	AActor* GetOrCreateMeshActor();

	UMadChunkMeshComponent* GetOrCreateComponent(const FMadChunkCoord& Coord);

	/** Snapshot + launch. Game thread. */
	void LaunchMeshJob(const FMadChunkCoord& Coord);

	/** Apply finished meshes, up to the per-frame budget. Game thread. */
	void PublishCompletedMeshes();

	UPROPERTY(Transient)
	TObjectPtr<AActor> MeshActor;

	UPROPERTY(Transient)
	TMap<FMadChunkCoord, TObjectPtr<UMadChunkMeshComponent>> Components;

	/**
	 * Released components kept registered and empty for reuse. Registering a new
	 * component (render state, physics state, attachment) was measured at up to
	 * 2.9 ms on the game thread; streaming releases about as many as it creates,
	 * so a pool turns most of those into a move.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMadChunkMeshComponent>> ComponentPool;
	int32 PoolReuses = 0;
	int32 DeferredApplies = 0;

	/** Running cost of applying a mesh, per vertex, for deciding whether one fits in this frame. */
	double ApplyMsPerVertex = 0.0002;

	/** Consecutive frames an apply was held back for the frame budget; past MaxStarvedFrames it goes ahead. */
	int32 StarvedFrames = 0;
	static constexpr int32 MaxStarvedFrames = 6;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SectionMaterial;

	bool bTriedLoadingMaterial = false;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UMaterialInterface>> ClassMaterials;

	/** A set, not a queue: forty edits to one chunk in one frame produce one rebuild. */
	TSet<FMadChunkCoord> DirtyChunks;
	TSet<FMadChunkCoord> InFlight;

	mutable FCriticalSection CompletedLock;
	TArray<FMadChunkMeshPtr> Completed;

	// --- rolling diagnostics, surfaced by mad.mesh.stats ---
	int32 TotalRebuilds = 0;
	double TotalBuildMilliseconds = 0.0;
	double WorstBuildMilliseconds = 0.0;
	double WorstApplyMilliseconds = 0.0;
	double WorstSnapshotMilliseconds = 0.0;

	/** Whole-frame game-thread cost of meshing (publish + launch), which is what the 2 ms rule is about. */
	double WorstFrameMilliseconds = 0.0;
	double WorstPublishMilliseconds = 0.0;
	double WorstLaunchMilliseconds = 0.0;
	double WorstRegisterMilliseconds = 0.0;
	int32 FramesWithWork = 0;
	int32 FramesOverBudget = 0;
};

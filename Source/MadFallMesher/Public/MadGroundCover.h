// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallCoordinates.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadGroundCover.generated.h"

class FMadChunkStorage;
class IMadBlockRegistry;
class UInstancedStaticMeshComponent;
class UMadVoxelWorldSubsystem;

namespace MadFall::GroundCover
{
	/** One tuft of grass or wildflowers: where its base stands and how it grows. */
	struct FTuft
	{
		/** World position of the base, on the surface, Unreal units. */
		FVector Location = FVector::ZeroVector;
		float Yaw = 0.0f;
		/** Unreal units. */
		float Height = 0.0f;
		float Width = 0.0f;
		/** The surface (material class) it grows from; its colour. */
		FName Surface;
		bool bFlower = false;
	};

	/**
	 * Every tuft growing in a chunk: at most one grass tuft and one flower per
	 * column whose top solid voxel in the chunk has open air above it and a
	 * surface with `cover`, placed by a hash of the world column so the same
	 * ground always grows the same grass. On smooth terrain the base follows the
	 * isosurface between the two density samples, not the voxel's top. Palette
	 * first: a chunk with no covered surface costs a palette scan. Caller holds
	 * the chunk's read lock.
	 */
	MADFALLMESHER_API void Collect(const FMadChunkStorage& Storage, const FMadChunkCoord& Coord, const IMadBlockRegistry& Registry, TArray<FTuft>& OutTufts);

	/**
	 * The two crossed quads of a tuft on the engine's 100 uu plane mesh, turned
	 * upright (the plane's +Y becomes up) and centred over the base.
	 */
	MADFALLMESHER_API void MakeTransforms(const FTuft& Tuft, FTransform& OutA, FTransform& OutB);
}

/**
 * Draws ground cover (grass tufts, wildflowers) near the survivor.
 *
 * WHY: open ground was a flat green carpet to the horizon; a few hundred
 * swaying tufts a chunk make it read as grass. Decoration only, so it is not
 * voxels: nothing to save, mine or collide with, and a surface opts in with
 * `cover` data.
 *
 * Chunks within mad.cover.Radius of the camera get one instanced component per
 * surface and kind, built on the game thread within mad.cover.BudgetMs and
 * culled past mad.cover.Distance. Rebuilt when a chunk changes, released when
 * it leaves the radius or unloads. Absent without a renderer.
 */
UCLASS()
class MADFALLMESHER_API UMadGroundCoverSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	FString DescribeStatus() const;

private:
	void HandleChunkChanged(const FMadChunkCoord& Coord);
	void HandleChunkUnloaded(const FMadChunkCoord& Coord);
	void UpdateWanted();
	void BuildChunk(const FMadChunkCoord& Coord);
	void ReleaseChunk(const FMadChunkCoord& Coord);
	UInstancedStaticMeshComponent* MakeComponent(FName Surface, bool bFlower);

	UPROPERTY(Transient)
	TObjectPtr<UMadVoxelWorldSubsystem> VoxelWorld;

	UPROPERTY(Transient)
	TObjectPtr<AActor> CoverActor;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> AllComponents;

	UPROPERTY(Transient)
	TObjectPtr<class UStaticMesh> PlaneMesh;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> BaseMaterial;

	/** Surface and kind -> the shared material instance. */
	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<class UMaterialInstanceDynamic>> Materials;

	TMap<FMadChunkCoord, TArray<TWeakObjectPtr<UInstancedStaticMeshComponent>>> Built;
	TSet<FMadChunkCoord> Wanted;
	TSet<FMadChunkCoord> Pending;
	FMadChunkCoord CentreChunk = FMadChunkCoord(TNumericLimits<int32>::Max(), 0, 0);
	float WantedTimer = 0.0f;
	int64 TotalBuilds = 0;
	double WorstBuildMs = 0.0;
};

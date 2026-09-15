// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadDebris.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadDebrisSubsystem.generated.h"

class AActor;
class UMadStructuralSubsystem;
class UMadVoxelWorldSubsystem;

/** Fired when a cluster lands, after impact damage and rubble are written. For audio, particles and camera shake. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FMadOnDebrisLanded, const FMadDebrisCluster& /*Cluster*/, const TArray<FMadDebrisImpact>& /*Impacts*/);

struct MADFALLGAMEPLAY_API FMadDebrisStats
{
	int64 ClustersSpawned = 0;
	int64 ClustersLanded = 0;
	int64 BlocksFallen = 0;
	int64 Impacts = 0;
	int64 RubblePlaced = 0;
	/** Clusters that broke off a landing one over open air and fell on. */
	int64 ClustersSheared = 0;
	/** Rubble not written because a pawn stood in its voxel; the block drops as salvage instead. */
	int64 RubbleSparedPawns = 0;
	int64 PawnHits = 0;
	int64 ItemsDropped = 0;
	double TotalEnergyKJ = 0.0;
	double LargestImpactKJ = 0.0;
};

/**
 * Turns structural collapses into falling debris.
 *
 *   UMadStructuralSubsystem::OnStructureCollapsed
 *     -> clusters (MadFall::Debris::BuildClusters)
 *     -> fall each tick on the voxel grid
 *     -> land: crush damage into the voxels underneath (which can cascade into
 *        another collapse), rubble compacted onto the landing surface
 *
 * The falling geometry is an instanced cube per voxel on a transient actor.
 * That is deliberately plain: it is on screen for about a second, and the
 * chunk mesher's surfaces would need a mesh rebuild per cluster for no
 * gameplay difference. Past MaxVisualClusters, clusters fall without visuals
 * so a demolished city block cannot spawn thousands of actors.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadDebrisSubsystem : public UTickableWorldSubsystem
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

	/** Lands every falling cluster now. Tests and console. */
	void FlushNow();

	int32 NumFalling() const { return Falling.Num(); }
	const FMadDebrisStats& GetStats() const { return Stats; }
	FString DescribeStatus() const;

	FMadOnDebrisLanded& OnDebrisLanded() { return LandedDelegate; }

private:
	struct FFalling
	{
		FMadDebrisCluster Cluster;
		TWeakObjectPtr<AActor> Visual;

		/** Pawns already hit by this cluster: one hit each, however many voxels it falls through them. */
		TArray<TWeakObjectPtr<AActor>> HitPawns;
	};

	void HandleCollapse(const TArray<FMadStructuralFailureRecord>& Failures);

	/** Adds a cluster that sheared off a landing one, still falling, with its own visual. */
	void KeepFalling(FMadDebrisCluster&& Cluster, TArray<TWeakObjectPtr<AActor>> HitPawns);

	/** Voices a strained member: a creak in its surface's impact kind, louder the nearer it is to failing. */
	void HandleStrain(const FMadStressSample& Member, const FMadVoxel& Voxel);
	void Land(FFalling& Item);
	void DamagePawns(FFalling& Item, int32 DroppedBefore);
	void DropCollapseLoot(const FMadDebrisCluster& Cluster, const TArray<TPair<FIntVector, int32>>& Rubble);
	bool IsFree(const FIntVector& Position) const;
	AActor* SpawnVisual(const FMadDebrisCluster& Cluster);

	UPROPERTY(Transient)
	TObjectPtr<UMadVoxelWorldSubsystem> VoxelWorld;

	UPROPERTY(Transient)
	TObjectPtr<UMadStructuralSubsystem> Structural;

	TArray<FFalling> Falling;
	FDelegateHandle CollapseHandle;
	FDelegateHandle StrainHandle;
	FMadOnDebrisLanded LandedDelegate;
	FMadDebrisStats Stats;
};

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallCoordinates.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadModelInstances.generated.h"

class FMadBlockRegistry;
class FMadChunkStorage;
class UInstancedStaticMeshComponent;
class UPointLightComponent;
class UMaterialInterface;
class UMadVoxelWorldSubsystem;
class UStaticMesh;

/**
 * Model blocks: a voxel whose block has `shape.kind: "model"` is drawn as a
 * static mesh (`render.mesh`) instead of a cube - barrels, crates, lamps,
 * anything a content mod ships.
 *
 * WHY INSTANCES PER CHUNK AND BLOCK TYPE
 *   One UInstancedStaticMeshComponent per (chunk, block type) means a chunk's
 *   hundred barrels are one draw call, a chunk that unloads drops exactly its
 *   components, and an edit rebuilds only its own chunk's instance list. An
 *   actor per model would be a UObject, a scene proxy and a physics body per
 *   barrel. The chunk mesher gives model voxels no faces and does not let them
 *   hide neighbouring faces (MadChunkMesher's IsModel).
 *
 * Gameplay does not change: the voxel is still solid, targetable, structural
 * and breakable through voxel data; the instance is its look and its collision.
 */
namespace MadFall::Models
{
	/**
	 * World transform (Unreal units) of a model voxel's mesh: voxel centre, the
	 * voxel's orientation (bits 0-4 of Rotation), then the block's render offset
	 * (voxels, rotated with the block) and scale.
	 */
	MADFALLMESHER_API FTransform MakeTransform(const FIntVector& WorldVoxel, uint8 Rotation, const FVector& OffsetVoxels, const FVector& Scale);

	/**
	 * Every model voxel in a chunk, as transforms grouped by runtime block id.
	 * A chunk whose palette has no model block returns in O(palette). Caller
	 * holds the chunk's read lock.
	 */
	MADFALLMESHER_API void CollectInstances(const FMadChunkStorage& Storage, const FMadChunkCoord& Coord,
		const FMadBlockRegistry& Registry, TMap<uint16, TArray<FTransform>>& OutByBlock);

	/** A light-emitting voxel: its block and the light's world position (Unreal units). */
	struct FBlockLight
	{
		uint16 BlockId = 0;
		FVector Location = FVector::ZeroVector;
	};

	/** Every voxel in a chunk whose block gives off light (render.light). Palette-checked like CollectInstances. */
	MADFALLMESHER_API void CollectLights(const FMadChunkStorage& Storage, const FMadChunkCoord& Coord,
		const FMadBlockRegistry& Registry, TArray<FBlockLight>& OutLights);
}

UCLASS()
class MADFALLMESHER_API UMadModelInstanceSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Rebuilds every dirty chunk now, ignoring the budget. Tests and console. */
	void FlushNow();

	int32 NumComponents() const;
	int32 NumInstances() const;
	int32 NumLights() const;
	/** Flames drawn, one per light-giving voxel, including those past the light cap. */
	int32 NumFlames() const;
	FString DescribeStats() const;

private:
	void HandleChunkChanged(const FMadChunkCoord& Coord);
	void HandleChunkUnloaded(const FMadChunkCoord& Coord);
	void RebuildChunk(const FMadChunkCoord& Coord);
	void ReleaseChunk(const FMadChunkCoord& Coord);
	struct FChunkModels;
	void UpdateLights(FChunkModels& Models, const TArray<MadFall::Models::FBlockLight>& Lights);
	void UpdateFlames(FChunkModels& Models, const TArray<MadFall::Models::FBlockLight>& Lights);

	UStaticMesh* GetMesh(uint16 BlockId);
	UMaterialInterface* GetMaterial(uint16 BlockId);
	AActor* GetOrCreateActor();

	UPROPERTY(Transient)
	TObjectPtr<UMadVoxelWorldSubsystem> VoxelWorld;

	UPROPERTY(Transient)
	TObjectPtr<AActor> ModelActor;

	struct FChunkModels
	{
		TMap<uint16, TWeakObjectPtr<UInstancedStaticMeshComponent>> ByBlock;
		TArray<TWeakObjectPtr<UPointLightComponent>> Lights;
		TWeakObjectPtr<UInstancedStaticMeshComponent> Flames;
	};
	TMap<FMadChunkCoord, FChunkModels> Chunks;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> AllComponents;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPointLightComponent>> AllLights;

	/** Kept apart from AllComponents: flames are decoration, not model blocks. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> AllFlames;

	UPROPERTY(Transient)
	TMap<uint16, TObjectPtr<UStaticMesh>> Meshes;

	UPROPERTY(Transient)
	TMap<uint16, TObjectPtr<UMaterialInterface>> Materials;

	TSet<FMadChunkCoord> Dirty;

	int64 TotalRebuilds = 0;
	double WorstRebuildMs = 0.0;
};

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadMeshBuffers.h"
#include "ProceduralMeshComponent.h"
#include "Templates/Function.h"
#include "MadChunkMeshComponent.generated.h"

class UMaterialInterface;

/**
 * Renders one chunk's generated geometry.
 *
 * WHY UProceduralMeshComponent:
 * Runtime-generated voxel geometry is not Nanite-friendly - Nanite needs an
 * offline build step to produce its cluster hierarchy, which cannot run
 * per-edit inside a frame budget. That rules out static meshes for this
 * geometry and leaves the dynamic pipeline (see the Nanite boundary note in
 * docs/ARCHITECTURE.md). Among the dynamic options, ProceduralMeshComponent has
 * the thing that actually matters here: bUseAsyncCooking, which moves Chaos
 * collision cooking off the game thread. It is not the fastest updater in the
 * engine, but its costs are per-rebuild and a chunk rebuild is already async.
 *
 * The mesher writes FMadChunkMesh, not this component's types, so replacing it
 * later is a change to one file.
 */
UCLASS(ClassGroup = MadFall, meta = (BlueprintSpawnableComponent))
class MADFALLMESHER_API UMadChunkMeshComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	UMadChunkMeshComponent(const FObjectInitializer& ObjectInitializer);

	/**
	 * Replaces this component's geometry.
	 *
	 * Must run on the game thread. This is the only part of a chunk rebuild
	 * that does, which is why the mesh subsystem budgets how many of these it
	 * performs per frame.
	 */
	void ApplyChunkMesh(const FMadChunkMesh& Mesh, TFunctionRef<UMaterialInterface*(FName MaterialClass)> MaterialFor);

	/** Chunk this component draws. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MadFall")
	FMadChunkCoord ChunkCoord;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MadFall")
	int32 LastVertexCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MadFall")
	int32 LastTriangleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MadFall")
	int32 LastSectionCount = 0;
};

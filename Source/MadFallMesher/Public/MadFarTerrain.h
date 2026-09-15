// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include "MadFarTerrain.generated.h"

class FMadWorldGenerator;
class UProceduralMeshComponent;

/** One far-terrain tile: a detail level and a tile index on that level's world-anchored grid. */
struct FMadFarTileKey
{
	int32 Level = 0;
	int32 X = 0;
	int32 Y = 0;

	bool operator==(const FMadFarTileKey& Other) const { return Level == Other.Level && X == Other.X && Y == Other.Y; }
	friend uint32 GetTypeHash(const FMadFarTileKey& Key) { return HashCombine(HashCombine(::GetTypeHash(Key.Level), ::GetTypeHash(Key.X)), ::GetTypeHash(Key.Y)); }
};

/** A level of detail: tile size and vertex spacing in voxels, how far it reaches, how far it sinks. */
struct FMadFarLevel
{
	int32 TileVoxels = 128;
	int32 SpacingVoxels = 8;

	/** Tiles whose centre is within this Chebyshev distance of the viewer are wanted, voxels. */
	int32 RangeVoxels = 640;

	/** Voxels the surface is lowered, so real terrain and finer levels always cover it where they overlap. */
	float DropVoxels = 2.0f;
};

/** A built tile, in centimetres relative to the tile's minimum corner at Z 0. */
struct FMadFarTileMesh
{
	FMadFarTileKey Key;
	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FColor> Colors;
	TArray<int32> Triangles;
};

namespace MadFall::FarTerrain
{
	/** Fine near the stream edge, coarse at the horizon. */
	MADFALLMESHER_API const TArray<FMadFarLevel>& GetLevels();

	/**
	 * The tiles to show around a viewer (voxel X, Y). A tile lying entirely
	 * inside what the level below covers - real chunks within HiddenHalfExtent
	 * for level 0, the finer level's guaranteed reach for the others - is left
	 * out: it would only ever be drawn underneath something better.
	 */
	MADFALLMESHER_API void GatherWanted(const FVector2D& Viewer, int32 HiddenHalfExtent, int32 RangeScalePercent, TArray<FMadFarTileKey>& Out);

	/**
	 * Samples the generator's analytic surface over a tile, with a skirt hanging
	 * from its edges so neighbouring levels never show a crack. Safe on any
	 * thread. BiomeColours maps biome index to vertex colour; WaterColour is
	 * used below sea level, where the surface is the water's.
	 */
	MADFALLMESHER_API void BuildTile(const FMadWorldGenerator& Generator, const FMadFarTileKey& Key, const TArray<FColor>& BiomeColours,
		FColor WaterColour, FMadFarTileMesh& Out);
}

/**
 * The land beyond the streamed chunks.
 *
 * WHY: chunks stream to 8 (at most 16) chunks, 256 m; past that the world just
 * stopped, and fog was the only thing hiding the edge. The generator's surface
 * height is an analytic function, so the rest of the landscape can be drawn
 * without generating a single voxel: coarse heightfield tiles sampled straight
 * from it, coloured by each spot's biome, out to the horizon.
 *
 * Tiles sit on a fixed world grid per level (128 voxels at 8 spacing, then 512
 * at 32, then 2048 at 128) rather than a clipmap recentred on the viewer, so a
 * tile is built once and kept while in range: walking never rebuilds or
 * swims the terrain, it only adds tiles ahead and drops tiles behind. Each
 * level sinks a little under the one inside it and every tile has a skirt, so
 * seams between levels and against real terrain stay hidden without stitching.
 * No trees, POIs or edits - at 300 m and beyond, fog and shape carry it.
 *
 * Tiles are sampled on workers; the game thread only creates components, a
 * few per frame within MadFall's frame budget.
 *
 *   `mad.far.Enabled`   0 hides it
 *   `mad.far.Range`     percent of each level's reach (default 100)
 *   `mad.far.status`
 */
UCLASS()
class MADFALLMESHER_API UMadFarTerrainSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	FString DescribeStatus() const;

	int32 NumTiles() const { return Tiles.Num(); }
	int32 NumBuilt() const;

private:
	void EnsureActor();
	void RefreshWanted(const FVector2D& Viewer);
	void LaunchBuilds();
	void ApplyFinished();

	struct FTile
	{
		TWeakObjectPtr<UProceduralMeshComponent> Component;
		bool bBuilding = false;
		bool bWanted = true;
	};
	TMap<FMadFarTileKey, FTile> Tiles;

	/** Wanted but not yet built, nearest first. */
	TArray<FMadFarTileKey> BuildQueue;

	FCriticalSection FinishedLock;
	TArray<TSharedPtr<FMadFarTileMesh>> Finished;
	int32 BuildsInFlight = 0;

	/** Waited on at shutdown: a build reads the voxel world's generator. */
	TArray<UE::Tasks::FTask> Tasks;

	UPROPERTY(Transient)
	TObjectPtr<AActor> TerrainActor;

	TArray<FColor> BiomeColours;
	FColor WaterColour = FColor(38, 77, 115);
	FVector2D LastViewer = FVector2D(TNumericLimits<float>::Max());
	int32 LastHidden = -1;
	int32 LastRange = -1;
	bool bLastEnabled = true;
	double LastCreateMs = 0.0;

	/** Running average cost of creating one tile component, ms. */
	double TileCostMs = 0.3;
	int32 TotalCreated = 0;
};

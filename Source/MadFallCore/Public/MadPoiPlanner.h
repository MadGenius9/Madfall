// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadChunkStorage.h"
#include "MadFallCoordinates.h"
#include "MadNoise.h"
#include "MadPrefab.h"

class FMadBiomeRegistry;
class FMadBlockRegistry;
class FMadPrefabRegistry;
class FMadWorldGenerator;
struct FMadWorldGenSettings;

/** One planned POI: a prefab, where it goes, which way it faces. */
struct MADFALLCORE_API FMadPoiInstance
{
	FIntPoint Cell = FIntPoint::ZeroValue;
	int32 PrefabIndex = INDEX_NONE;

	/** World voxel of the rotated footprint's minimum corner, at the prefab's bottom layer. */
	FIntVector Origin = FIntVector::ZeroValue;

	/** Quarter turns about +Z. */
	int32 Yaw = 0;

	FIntVector RotatedSize = FIntVector::ZeroValue;

	/** The cell's difficulty tier, which may exceed the prefab's own. */
	int32 CellTier = 1;

	FName BiomeId;

	bool IsValid() const { return PrefabIndex != INDEX_NONE; }

	/** World-space bounds including the deepest possible foundation. */
	void GetWorldBounds(int32 FoundationDepth, FIntVector& OutMin, FIntVector& OutMax) const;
};

/** A marker resolved into world space, for loot and spawn systems to consume. */
struct MADFALLCORE_API FMadPoiWorldMarker
{
	FName Type;
	FIntVector WorldPosition = FIntVector::ZeroValue;
	FName LootTable;
	FName SpawnGroup;
	int32 Count = 1;
	FName PrefabId;
	int32 Tier = 1;
	FName Trader;
	TArray<FName> Tags;
};

/** A road between two POI entrances. */
struct MADFALLCORE_API FMadRoadSegment
{
	FIntVector Start = FIntVector::ZeroValue;
	FIntVector End = FIntVector::ZeroValue;
	uint32 Seed = 0;
};

/**
 * Plans and stamps POIs and the roads between them.
 *
 * CELL-BASED, AND WHY:
 * A prefab can span several chunks, and a chunk can be generated before or after
 * its neighbours, on any thread. The only way to stamp a building consistently
 * across chunks generated independently is for every chunk to be able to
 * re-derive the same plan from the seed alone. So the world is divided into
 * cells of PoiCellChunks x PoiCellChunks chunks, each cell hosts at most one POI
 * placed wholly inside it, and a chunk asks its own cell - by hash of the seed
 * and cell coordinate - what, if anything, overlaps it. No POI placement is
 * ever stored; all of it is regenerated, identically, on demand.
 *
 * Cells align with chunk boundaries, so a POI can only ever touch chunks inside
 * its own cell. Roads cross cells, which is why they are looked up from the
 * neighbouring cells as well.
 */
class MADFALLCORE_API FMadPoiPlanner
{
public:
	void Initialize(const FMadWorldGenSettings& InSettings, const FMadPrefabRegistry* InPrefabs,
		const FMadBiomeRegistry& InBiomes, FMadBlockRegistry& InBlocks);

	bool HasPrefabs() const;

	FIntPoint ChunkToCell(const FMadChunkCoord& Coord) const;

	int32 GetCellSizeVoxels() const;

	/** The difficulty tier of a cell: rises with distance from the world origin. */
	int32 GetCellTier(int32 CellX, int32 CellY) const;

	/**
	 * The POI planned for a cell, if any. Deterministic and cached.
	 * Returns false for a cell with no POI.
	 */
	bool PlanCell(const FMadWorldGenerator& Generator, int32 CellX, int32 CellY, FMadPoiInstance& Out) const;

	void GetPoisOverlappingChunk(const FMadWorldGenerator& Generator, const FMadChunkCoord& Coord,
		TArray<FMadPoiInstance>& OutPois) const;

	/** Writes the part of a POI - prefab plus foundation - that falls inside one chunk. */
	void StampPoi(const FMadWorldGenerator& Generator, const FMadPoiInstance& Poi,
		const FMadChunkCoord& Coord, FMadChunkStorage& Storage) const;

	void GetRoadsNearChunk(const FMadWorldGenerator& Generator, const FMadChunkCoord& Coord,
		TArray<FMadRoadSegment>& OutRoads) const;

	/**
	 * True when a POI (with its deepest foundation) or a road passes within
	 * Reach voxels of a world column. Scatter uses it to keep trees out of
	 * buildings and off roads: a POI or road written over half a tree would
	 * leave the rest hanging, and the structural check on load would drop it.
	 * Depends only on the seed, so every chunk a tree touches gets the same answer.
	 */
	bool IsNearPoiOrRoad(const FMadWorldGenerator& Generator, int32 WorldX, int32 WorldY, int32 WorldZ, int32 Reach) const;

	/**
	 * Writes the part of a road that falls inside one chunk. SurfaceHeights, when
	 * given, is the chunk's 32x32 column heights GenerateChunk already computed
	 * (GetSurfaceHeight at each voxel corner); the banks would otherwise compute
	 * them again.
	 */
	void StampRoad(const FMadWorldGenerator& Generator, const FMadRoadSegment& Road,
		const FMadChunkCoord& Coord, FMadChunkStorage& Storage, const float* SurfaceHeights = nullptr) const;

	/**
	 * Eases the ground beside a road back to the terrain (MadFall::Roads::BankHeight),
	 * cutting into slopes and filling hollows, and clears earth above the road's
	 * clearance in a deep cutting. Runs before the road body, which then overwrites
	 * the road's own columns.
	 */
	void StampRoadBanks(const FMadWorldGenerator& Generator, const FMadRoadSegment& Road,
		const FMadChunkCoord& Coord, FMadChunkStorage& Storage, int32 Steps, TFunctionRef<FVector2f(float)> CentreAt,
		const float* SurfaceHeights) const;

	void GetWorldMarkers(const FMadPoiInstance& Poi, TArray<FMadPoiWorldMarker>& OutMarkers) const;

	/** World position of a POI's entrance, where roads connect. */
	FIntVector GetWorldEntrance(const FMadPoiInstance& Poi) const;

	const FMadPrefab* GetPrefab(const FMadPoiInstance& Poi) const;

	FString DescribeCell(const FMadWorldGenerator& Generator, int32 CellX, int32 CellY) const;

	/** How many cells have been planned this session. For diagnostics. */
	int32 NumCachedCells() const;

	/**
	 * The cell a near_spawn prefab was given (see FMadPrefabPlacement::bNearSpawn).
	 * False if the prefab is not near_spawn or no cell around the spawn fits it.
	 */
	bool GetNearSpawnCell(const FMadWorldGenerator& Generator, FName PrefabId, FIntPoint& OutCell) const;

	/** Rings of cells searched for a near_spawn prefab, beyond the four cells around the spawn itself. */
	static constexpr int32 NearSpawnRings = 3;

private:
	bool PlanCellUncached(const FMadWorldGenerator& Generator, int32 CellX, int32 CellY, FMadPoiInstance& Out) const;

	/** The site search for one prefab in one cell, drawing yaw and sites from Random. */
	bool PlacePrefabInCell(const FMadWorldGenerator& Generator, int32 PrefabIndex, int32 CellX, int32 CellY,
		MadFall::Noise::FChunkRandom& Random, FMadPoiInstance& Out) const;

	/** Assigns every near_spawn prefab its cell, once. */
	void ResolveNearSpawn(const FMadWorldGenerator& Generator) const;

	/** Maps a world voxel back into prefab-local space, or returns false if it is outside. */
	bool WorldToLocal(const FMadPoiInstance& Poi, const FIntVector& World, FIntVector& OutLocal) const;

	/** Road surface height at a parameter along a segment, smoothed and blended into both endpoints. */
	float RoadHeightAt(const FMadWorldGenerator& Generator, const FMadRoadSegment& Road,
		const FVector2f& Point, float T) const;

	const FMadWorldGenSettings* Settings = nullptr;
	const FMadPrefabRegistry* Prefabs = nullptr;
	const FMadBiomeRegistry* Biomes = nullptr;

	/** Per prefab, per palette entry: the runtime block id, or MAX_uint16 for void. */
	TArray<TArray<uint16>> PaletteRuntimeIds;

	/** Per prefab: its foundation block, already falling back to FoundationFallback. */
	TArray<uint16> FoundationIds;

	uint16 FoundationFallback = 0;
	uint16 RoadBlock = 0;
	uint16 RoadBedBlock = 0;

	uint32 PoiSeed = 0;
	uint32 RoadSeed = 0;

	/**
	 * Cell plans are cached because every chunk in a cell - up to 8*8*16 of them -
	 * asks the same question, and answering it costs a few dozen height samples.
	 * Guarded by a lock because chunks generate on many worker threads at once.
	 */
	mutable FRWLock CacheLock;
	mutable TMap<FIntPoint, FMadPoiInstance> PlanCache;

	mutable FRWLock NearSpawnLock;
	mutable bool bNearSpawnResolved = false;

	/** Cell -> prefab index, for near_spawn prefabs. */
	mutable TMap<FIntPoint, int32> NearSpawnCells;
};

namespace MadFall::Roads
{
	/**
	 * Horizontal voxels of bank per voxel of height between road and terrain.
	 * The bank profile is a smoothstep, whose steepest point is 1.5x its mean
	 * slope, so 1.5 keeps every bank at or under 45 degrees - walkable, and it
	 * reads as a cutting instead of a trench.
	 */
	inline constexpr float BankRun = 1.5f;

	/**
	 * Widest bank, in voxels. A cutting deeper than MaxBank / BankRun (about 7
	 * voxels) gets steeper rather than wider: an unbounded bank would shave a
	 * hillside flat for a road that only skirts it, and would widen the band
	 * every chunk has to search for roads.
	 */
	inline constexpr float MaxBank = 10.0f;

	/**
	 * Ground height Beyond voxels outside a road's edge: the road's height at the
	 * edge, easing to the untouched terrain height at the bank's far side. Beyond
	 * at or below 0 is the road itself.
	 */
	MADFALLCORE_API float BankHeight(float RoadHeight, float TerrainHeight, float Beyond);
}

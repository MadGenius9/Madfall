// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBiomeRegistry.h"
#include "MadChunkStorage.h"
#include "MadFallCoordinates.h"
#include "MadNoise.h"
#include "MadPoiPlanner.h"

class FMadBlockRegistry;
class FMadPrefabRegistry;

namespace MadFall
{
	/**
	 * Converts a signed distance below a surface, in voxels, into the 0-255 density
	 * byte the voxel format stores.
	 *
	 * The one-voxel soft band is the whole reason terrain meshes smoothly: a hard
	 * 0/255 step carries no sub-voxel information, so Surface Nets would place
	 * every crossing at the midpoint of an edge and the result would be a
	 * staircase. Shared by terrain AND roads - roads written with a hard step
	 * rendered as a notched trench beside smooth ground.
	 */
	FORCEINLINE uint8 DistanceToDensity(float DistanceBelowSurface)
	{
		const float Normalized = FMath::Clamp(DistanceBelowSurface * 0.5f + 0.5f, 0.0f, 1.0f);
		return static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
	}
}

/**
 * World generation parameters.
 *
 * Everything that shapes the world at a scale larger than a biome. Biome-scale
 * decisions live in biome definitions, where mods can reach them.
 */
struct MADFALLCORE_API FMadWorldGenSettings
{
	uint32 Seed = 1337;

	/** World Z at and below which air becomes water. */
	int32 SeaLevel = 12;

	/** Bedrock fills everything at or below this Z, unconditionally. */
	int32 BedrockTop = MadFall::WorldMinZ + 2;

	/** Frequency of the continental shape field. Lower means bigger landmasses. */
	float ContinentFrequency = 0.0009f;

	/** Frequency of the temperature and moisture fields. */
	float ClimateFrequency = 0.0016f;

	/**
	 * Expands the climate fields so biomes at the extremes are reachable.
	 *
	 * Fractal Brownian motion sums octaves of decreasing amplitude and divides
	 * by their total, which produces a distribution clustered tightly around
	 * its mean - a survey of 37,249 columns measured continentalness spanning
	 * only 0.31 to 0.74 and temperature only 0.15 to 0.64. Any biome whose
	 * climate range sat outside that band could never be selected: highlands
	 * (continentalness >= 0.78) and desert (temperature >= 0.70) covered
	 * exactly 0% of the world, and nothing in the biome histogram said why -
	 * they simply were not listed.
	 *
	 * Multiplying the signed field by this before clamping spreads it back
	 * across the full range. Values that saturate become plateaus, which is the
	 * right shape anyway: deep ocean and high plateau really are large regions
	 * of near-constant character rather than single points.
	 */
	float ClimateContrast = 2.2f;

	/**
	 * How far the 3D warp displaces the height field, in voxels.
	 *
	 * This is what produces overhangs and undercut cliffs. Pure heightmaps
	 * cannot make them at all - the surface is a function of (x, y), so it can
	 * never fold back over itself.
	 */
	float WarpStrength = 9.0f;

	float WarpFrequency = 0.012f;

	/** Ridged 3D noise above this threshold becomes a cave. */
	float CaveThreshold = 0.82f;

	float CaveFrequency = 0.022f;

	/** Caves stop this far below the surface, so they do not open the terrain skin everywhere. */
	int32 CaveSurfaceMargin = 6;

	// --- points of interest ---------------------------------------------

	/** POI cell edge length, in chunks. A cell hosts at most one POI, placed wholly inside it. */
	int32 PoiCellChunks = 8;

	/** Probability that a cell contains a POI at all. */
	float PoiDensity = 0.6f;

	/** Voxels kept clear between a POI and its cell edge, so neighbouring POIs never touch. */
	int32 PoiCellMargin = 12;

	/** World distance, in voxels, per step up in difficulty tier. Tier 1 surrounds spawn. */
	int32 TierDistanceStep = 1200;

	// --- roads ------------------------------------------------------------

	bool bRoads = true;

	/** Full road width in voxels. */
	int32 RoadWidth = 3;

	/** Neighbouring POIs further apart than this are not connected. */
	int32 RoadMaxLength = 520;

	/** Sideways wander of a road at its midpoint, in voxels. Zero at both ends. */
	float RoadMeander = 10.0f;

	/**
	 * Bumped whenever generation changes shape.
	 *
	 * Stored in every region header. A mismatch means chunks on disk were made
	 * by a different generator, which is a seam the player would see and could
	 * not explain - the region loader refuses rather than mixing them.
	 */
	uint64 GetGenerationVersion() const;
};

/**
 * The world generator.
 *
 * Layered, in the order the brief calls for: continental shape, then climate,
 * then biome selection, then terrain height, then the 3D density field with
 * warp, then caves, then surface composition, then ores.
 *
 * PURE AND DETERMINISTIC. GenerateChunk is a function of (settings, coord,
 * registries) and nothing else - no global RNG, no dependence on which chunks
 * were generated before it, no engine state. Two players with the same seed and
 * the same mods get the same world, and a chunk regenerates identically whether
 * it is the first or the ten-thousandth the session has produced.
 */
class MADFALLCORE_API FMadWorldGenerator
{
public:
	FMadWorldGenerator(const FMadWorldGenSettings& InSettings,
		const FMadBiomeRegistry& InBiomes,
		FMadBlockRegistry& InBlocks,
		const FMadPrefabRegistry* InPrefabs = nullptr);

	// Held by reference from long-lived owners, and the POI planner caches cell
	// plans against this instance. A copy would silently share nothing with the
	// original, so copying is not allowed. C++17 guaranteed elision still lets
	// factory functions return one by value.
	FMadWorldGenerator(const FMadWorldGenerator&) = delete;
	FMadWorldGenerator& operator=(const FMadWorldGenerator&) = delete;
	FMadWorldGenerator(FMadWorldGenerator&&) = delete;
	FMadWorldGenerator& operator=(FMadWorldGenerator&&) = delete;

	/** Fills a chunk. Safe to call from any thread. */
	void GenerateChunk(const FMadChunkCoord& Coord, FMadChunkStorage& OutStorage) const;

	// --- the individual fields, exposed for tooling and tests ---------------

	/** 0 = deep ocean, 1 = deep continent. */
	float GetContinentalness(float WorldX, float WorldY) const;

	/** 0 = cold, 1 = hot. Falls with altitude as well as latitude-like noise. */
	float GetTemperature(float WorldX, float WorldY, float Height) const;

	/** 0 = arid, 1 = wet. */
	float GetMoisture(float WorldX, float WorldY) const;

	/** Blended terrain surface height at a column, in world Z. */
	float GetSurfaceHeight(float WorldX, float WorldY) const;

	/** The biome that dominates a column. */
	int32 GetDominantBiome(float WorldX, float WorldY) const;

	/**
	 * Nearest column (searching outward in rings of Step voxels) where BiomeIndex
	 * dominates the column and four points Margin voxels around it - somewhere
	 * well inside the biome rather than on its border. False if none within MaxRadius.
	 */
	bool FindBiomeNear(int32 BiomeIndex, const FIntPoint& Near, int32 MaxRadius, FIntPoint& OutColumn, int32 Step = 96, int32 Margin = 40) const;

	/**
	 * Whether generated terrain is solid at a voxel, ignoring caves, POIs and roads.
	 *
	 * A pure function of the coordinate, which is what lets a POI foundation or a
	 * road embankment find the ground under a column that lies in a chunk that
	 * has not been generated - or is being generated on another thread.
	 */
	bool IsTerrainSolid(int32 WorldX, int32 WorldY, int32 WorldZ) const;

	/** 3D domain warp displacement, in voxels: generated ground is solid where GetSurfaceHeight + GetWarp - Z >= 0. Road banks read it to find the real ground. */
	float GetWarp(float WorldX, float WorldY, float WorldZ) const;

	/** Highest solid terrain voxel at or below StartZ, searching at most MaxDepth. INDEX_NONE if none. */
	int32 FindTerrainTopBelow(int32 WorldX, int32 WorldY, int32 StartZ, int32 MaxDepth) const;

	const FMadPoiPlanner& GetPoiPlanner() const { return PoiPlanner; }

	const FMadBiomeRegistry& GetBiomes() const { return Biomes; }

	const FMadWorldGenSettings& GetSettings() const { return Settings; }

	/** Human-readable dump for `mad.worldgen.probe`. */
	FString ProbeColumn(int32 WorldX, int32 WorldY) const;

private:
	/** Resolved block ids, looked up once instead of per voxel. */
	struct FResolvedBlocks
	{
		uint16 Air = 0;
		uint16 Bedrock = 0;
		uint16 Water = 0;
	};

	void ComputeColumnBiomes(float WorldX, float WorldY, TArray<FMadBiomeSample>& OutSamples) const;

	/** The single implementation of the blended height field. Scratch avoids an allocation per column. */
	float ComputeSurfaceHeight(float WorldX, float WorldY, TArray<FMadBiomeSample>& Scratch) const;

	uint16 ResolveBlock(FName BlockId) const;

	/** Pass 4: trees, boulders and plants rooted in or near this chunk. */
	void GenerateScatter(const FMadChunkCoord& Coord, const float* SurfaceHeights, const int32* ColumnBiomes, FMadChunkStorage& Storage) const;

	FMadWorldGenSettings Settings;
	const FMadBiomeRegistry& Biomes;
	FMadBlockRegistry& Blocks;

	FMadPoiPlanner PoiPlanner;

	FResolvedBlocks Resolved;

	/** Per-biome resolved block ids, parallel to the biome registry. */
	struct FBiomeBlocks
	{
		uint16 Surface = 0;
		uint16 Subsurface = 0;
		uint16 Stone = 0;
		uint16 UnderwaterSurface = 0;
		TArray<uint16> Ores;
		/** Parallel to the biome's Scatter list. Leaves are air for a bare trunk. */
		TArray<uint16> ScatterBlocks;
		TArray<uint16> ScatterLeaves;
		/** The leaves block's max_horizontal_span: how far a canopy may reach and still be held up. */
		TArray<int32> ScatterLeafSpans;
	};

	TArray<FBiomeBlocks> BiomeBlocks;

	// Derived seeds, one per field, so changing one field's noise does not
	// reshuffle the others.
	uint32 ContinentSeed = 0;
	uint32 TemperatureSeed = 0;
	uint32 MoistureSeed = 0;
	uint32 HeightSeed = 0;
	uint32 WarpSeed = 0;
	uint32 CaveSeed = 0;
	uint32 OreSeed = 0;
	uint32 ScatterSeed = 0;

	/** Largest per-column scatter chance of any biome: the cheap pre-check before a column's biome is computed. */
	float MaxScatterChance = 0.0f;

	/** How far any scatter feature reaches from its root column, and how tall one can be. */
	int32 MaxScatterReach = 0;
	int32 MaxScatterHeight = 0;
};

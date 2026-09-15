// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBlockDefinitionJson.h"
#include "MadBiomeDefinition.generated.h"

/** The schema string every biome definition file must declare. */
namespace MadFall
{
	inline const TCHAR* BiomeSchemaV1 = TEXT("madfall.biome/1");
}

/** An inclusive range, used for every climate axis. */
USTRUCT(BlueprintType)
struct MADFALLCORE_API FMadClimateRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	float Min = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	float Max = 1.0f;

	bool Contains(float Value) const { return Value >= Min && Value <= Max; }

	float Centre() const { return (Min + Max) * 0.5f; }

	float Width() const { return Max - Min; }

	/**
	 * 1 inside the range, falling off smoothly outside it.
	 *
	 * Smooth rather than binary because biome selection blends the top few
	 * candidates: a hard in/out test produces a visible cliff wherever two
	 * biomes' terrain heights differ, and the world ends up looking like a
	 * patchwork of tiles rather than a landscape.
	 *
	 * THE FALLOFF IS PROPORTIONAL TO THE RANGE, NOT A FIXED WIDTH.
	 * A fixed falloff punishes narrow biomes: a beach claiming
	 * continentalness [0.34, 0.46] with a flat 0.25 falloff would still score
	 * above zero across [0.09, 0.71] - most of the world - and a survey showed
	 * exactly that, with beach covering 20% of the map. Scaling the falloff by
	 * the range's own width means a narrow biome stays narrow and a broad one
	 * still blends broadly.
	 */
	float Score(float Value, float FalloffScale = 0.5f) const
	{
		if (Contains(Value))
		{
			return 1.0f;
		}

		// A floor so a zero-width range (a biome pinned to one exact value) is
		// still reachable rather than mathematically impossible.
		const float Falloff = FMath::Max(Width() * FalloffScale, 0.04f);
		const float Distance = (Value < Min) ? (Min - Value) : (Value - Max);

		return FMath::Max(0.0f, 1.0f - Distance / Falloff);
	}
};

/** One ore type's distribution within a biome. */
USTRUCT(BlueprintType)
struct MADFALLCORE_API FMadOreDistribution
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	FName Block;

	/** Cluster placement attempts per chunk. Not every attempt succeeds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome", meta = (ClampMin = "0"))
	int32 AttemptsPerChunk = 4;

	/** Voxels per cluster, before the shape is eroded by the surrounding stone test. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome", meta = (ClampMin = "1"))
	int32 ClusterSize = 6;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	int32 MinZ = -128;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	int32 MaxZ = 64;

	/** 0 = never, 1 = every attempt places a cluster. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Probability = 0.5f;
};

UENUM(BlueprintType)
enum class EMadScatterKind : uint8
{
	/** A trunk of `block` with an optional canopy of `leaves`. */
	Tree,
	/** A rough sphere of `block`, written as terrain (smooth, never collapses). */
	Boulder,
	/** One `block` standing on the surface - a bush, a rock, a flower. */
	Plant
};

/**
 * One kind of thing a biome scatters over its surface.
 *
 * Every column rolls once against the biome's scatter list, so `chance` is
 * "fraction of surface columns this feature grows from" - 0.01 is one tree per
 * hundred columns, about ten per chunk.
 */
USTRUCT(BlueprintType)
struct MADFALLCORE_API FMadScatterFeature
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	EMadScatterKind Kind = EMadScatterKind::Plant;

	/** Trunk (tree), rock (boulder) or the plant itself. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	FName Block;

	/** Tree canopy. None grows a bare trunk - a cactus, a dead tree. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	FName Leaves;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Chance = 0.01f;

	/** Tree trunk height range, in voxels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	int32 MinHeight = 4;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	int32 MaxHeight = 6;

	/** Canopy radius (tree) or sphere radius range (boulder), in voxels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	float MinRadius = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Biome")
	float MaxRadius = 2.0f;

	/** How far from its root column this feature can write, in voxels. */
	int32 GetReach() const { return Kind == EMadScatterKind::Plant ? 0 : FMath::CeilToInt(MaxRadius) + 1; }
};

/**
 * A biome: what the terrain does here, and what it is made of.
 *
 * Loaded from `Definitions/biomes/*.json` and `Mods/<id>/definitions/biomes/*.json`
 * through the same loader, the same validation and the same `extends`
 * inheritance as blocks. A mod that wants a new biome writes JSON; nothing
 * about biomes is compiled in.
 */
USTRUCT(BlueprintType)
struct MADFALLCORE_API FMadBiomeDefinitionData
{
	GENERATED_BODY()

	// --- identity ----------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Identity")
	FName Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Identity")
	FName Extends;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Identity")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Identity")
	TArray<FName> Tags;

	// --- climate -----------------------------------------------------------
	// All three axes are noise fields normalised to [0, 1] over the whole world.

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Climate")
	FMadClimateRange Temperature;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Climate")
	FMadClimateRange Moisture;

	/** How far inland. Low values are ocean, high values are deep continent. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Climate")
	FMadClimateRange Continentalness;

	/**
	 * Multiplies this biome's climate score.
	 *
	 * The escape hatch for "this biome should win ties" without distorting its
	 * climate ranges into something that no longer describes the biome.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Climate", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

	// --- terrain -----------------------------------------------------------

	/** World Z the terrain sits around before variation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Terrain")
	float BaseHeight = 16.0f;

	/** Peak-to-trough amplitude of the height noise, in voxels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Terrain", meta = (ClampMin = "0.0"))
	float HeightVariation = 8.0f;

	/** Noise frequency for the height field. Higher is busier terrain. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Terrain", meta = (ClampMin = "0.0"))
	float Roughness = 0.008f;

	/**
	 * Blends the height field toward ridged noise.
	 *
	 * 0 is rolling hills, 1 is sharp mountain ridges. Ridged noise creases
	 * where fBm blobs, which is the difference between a lumpy field and
	 * something that reads as a mountain range.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Terrain", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Ridging = 0.0f;

	// --- composition -------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Blocks")
	FName SurfaceBlock = FName(TEXT("madfall:grass"));

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Blocks")
	FName SubsurfaceBlock = FName(TEXT("madfall:dirt"));

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Blocks", meta = (ClampMin = "0"))
	int32 SubsurfaceDepth = 4;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Blocks")
	FName StoneBlock = FName(TEXT("madfall:stone"));

	/** Replaces the surface block below sea level. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Blocks")
	FName UnderwaterSurfaceBlock = FName(TEXT("madfall:sand"));

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Blocks")
	TArray<FMadOreDistribution> Ores;

	/** Trees, boulders and plants on the surface. Rolled in order, so earlier entries win a column. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Blocks")
	TArray<FMadScatterFeature> Scatter;

	// --- provenance --------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Provenance")
	FName SourceModId;

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Provenance")
	FString SourcePath;

	/** How well this biome fits a climate sample. Zero means it does not apply here. */
	float ScoreClimate(float InTemperature, float InMoisture, float InContinentalness) const
	{
		const float Score = Temperature.Score(InTemperature)
			* Moisture.Score(InMoisture)
			* Continentalness.Score(InContinentalness);

		return Score * Weight;
	}
};

namespace MadFall::BiomeDefinitionJson
{
	MADFALLCORE_API bool ParseObject(
		const TSharedRef<FJsonObject>& Object,
		const FString& SourcePath,
		FName ModId,
		FMadBiomeDefinitionData& OutData,
		TArray<FMadDefinitionError>& OutErrors);
}

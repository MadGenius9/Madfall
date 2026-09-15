// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBiomeDefinition.h"

class FJsonObject;

/** A biome and its blend weight at one point. */
struct MADFALLCORE_API FMadBiomeSample
{
	int32 BiomeIndex = INDEX_NONE;
	float Weight = 0.0f;
};

/**
 * The biome registry.
 *
 * Same lifecycle, same `extends` inheritance and same validation as the block
 * registry, because biomes are loaded from JSON by the same machinery. The
 * difference is that biome identity is never written to a save file - a chunk
 * stores blocks, and the biome that produced them is re-derived from the seed.
 * That means removing a biome mod changes what UNGENERATED terrain will look
 * like but cannot corrupt terrain that already exists.
 */
class MADFALLCORE_API FMadBiomeRegistry
{
public:
	FMadBiomeRegistry();

	void BeginLoad();

	int32 AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors);

	void FinishLoad(TArray<FMadDefinitionError>& OutErrors);

	/** Applies mod patches to every staged definition. Call between staging and FinishLoad. */
	void ApplyPatches(const class FMadPatchSet& Patches, TArray<FMadDefinitionError>& OutErrors);

	int32 Num() const { return Biomes.Num(); }

	bool IsEmpty() const { return Biomes.Num() == 0; }

	const FMadBiomeDefinitionData& Get(int32 Index) const { return Biomes[Index]; }

	int32 FindIndex(FName BiomeId) const;

	const TArray<FMadBiomeDefinitionData>& GetAll() const { return Biomes; }

	/**
	 * Ranks biomes by how well they fit a climate sample and returns the top
	 * few, weights normalised to sum to 1.
	 *
	 * Blending rather than picking one: terrain height is interpolated across
	 * the returned biomes, which is what turns the boundary between a plain and
	 * a mountain range into a slope instead of a cliff. MaxResults of 3 is
	 * enough for a smooth transition and cheap enough to run per column.
	 */
	void SampleBiomes(float Temperature, float Moisture, float Continentalness,
		TArray<FMadBiomeSample>& OutSamples, int32 MaxResults = 3) const;

	/** The single best-fitting biome, or INDEX_NONE if none is registered. */
	int32 PickDominant(float Temperature, float Moisture, float Continentalness) const;

	FString DescribeContents() const;

private:
	bool ResolveDefinition(FName Id, TSet<FName>& Visiting,
		TMap<FName, FMadBiomeDefinitionData>& Resolved, TArray<FMadDefinitionError>& OutErrors);

	struct FPending
	{
		FName Id;
		FName Extends;
		FName ModId;
		FString SourcePath;
		TSharedPtr<FJsonObject> Json;
	};

	TArray<FMadBiomeDefinitionData> Biomes;
	TMap<FName, int32> IdToIndex;

	TArray<FPending> Pending;
	TMap<FName, int32> PendingByIndex;

	bool bLoaded = false;
};

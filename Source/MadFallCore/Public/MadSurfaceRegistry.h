// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBlockDefinitionJson.h"
#include "UObject/SoftObjectPath.h"

class FJsonObject;
class FMadPatchSet;

namespace MadFall
{
	inline const TCHAR* SurfaceSchemaV1 = TEXT("madfall.surface/1");
}

namespace MadFall::Surfaces
{
	/**
	 * Procedural surface patterns the default voxel material draws, by index.
	 * The index travels to the GPU in vertex colour alpha (255 - index * 16), so
	 * pattern 0 - plain colour - is alpha 255, which is what every vertex had
	 * before patterns existed. Order is part of the material: append only.
	 */
	inline const TCHAR* PatternNames[] = {
		TEXT("plain"), TEXT("stone"), TEXT("dirt"), TEXT("grass"), TEXT("sand"), TEXT("planks"), TEXT("bark"), TEXT("leaves"),
		TEXT("concrete"), TEXT("brick"), TEXT("metal"), TEXT("ore"), TEXT("farmland"), TEXT("cloth"), TEXT("water"), TEXT("gravel")
	};
	inline constexpr int32 NumPatterns = UE_ARRAY_COUNT(PatternNames);

	/** Pattern index for a name; INDEX_NONE if unknown. */
	MADFALLCORE_API int32 FindPattern(const FString& Name);

	/** The vertex colour alpha byte that selects a pattern. */
	inline uint8 PatternToAlpha(int32 Pattern) { return static_cast<uint8>(255 - FMath::Clamp(Pattern, 0, NumPatterns - 1) * 16); }
}

/** How one material class looks. */
struct MADFALLCORE_API FMadSurfaceDefinition
{
	/** The material class blocks name in material.class, e.g. "madfall:concrete". */
	FName Id;

	/** Render material for every block of this class. Null path: the default voxel material. */
	FSoftObjectPath Material;

	/**
	 * Base colour, LINEAR 0..1. Authored in JSON as sRGB (what a colour picker
	 * shows) and converted on load: the voxel material reads vertex colour as
	 * linear, and a palette authored as if it were sRGB but used as linear came
	 * out washed-out and pastel - grass at 46% albedo, sand at 76%.
	 */
	FLinearColor Color = FLinearColor(0.32f, 0.32f, 0.32f);
	bool bHasColor = false;

	/** What the class sounds like struck, walked on and broken: stone, wood, dirt, metal or foliage. */
	FName Impact = FName(TEXT("stone"));

	/** Procedural pattern index (MadFall::Surfaces::PatternNames) the default voxel material draws over Color. */
	int32 Pattern = 0;

	/**
	 * Ground cover ("cover"): grass tufts and wildflowers drawn on top of this
	 * surface near the survivor. Decoration only - not voxels, no collision, not
	 * saved. Density and Flowers are the chance per exposed column, 0..1.
	 */
	float CoverDensity = 0.0f;
	float CoverFlowers = 0.0f;
	/** Tuft height range, voxels. */
	float CoverHeightMin = 0.25f;
	float CoverHeightMax = 0.55f;
	/** Blade colour, linear; the surface's own colour unless the cover names one. */
	FLinearColor CoverColor = FLinearColor(0.1f, 0.25f, 0.05f);

	bool HasCover() const { return CoverDensity > 0.0f || CoverFlowers > 0.0f; }

	FString SourcePath;
};

/**
 * Material class -> appearance, from `definitions/surfaces/`.
 *
 *   { "schema": "madfall.surface/1", "id": "madfall:concrete",
 *     "material": "/Game/Materials/M_MadVoxel.M_MadVoxel", "color": [0.55, 0.55, 0.52] }
 *
 * WHY A SEPARATE KIND AND NOT A FIELD ON EVERY BLOCK
 *   Chunk meshes are split into one section per material class, so a class is
 *   what a draw call and a material slot correspond to. Twenty concrete block
 *   variants share one surface; a texture pack re-skins concrete by patching
 *   one definition instead of twenty blocks. Blocks still carry their own
 *   render.material for one-off models.
 *
 * This is also the Tier-2 bridge: a content mod's pak supplies a material
 * asset, and a surface (its own, or a patch to a first-party one) points a
 * material class at it.
 *
 * Immutable after FinishLoad, so mesher worker threads read it without locks.
 */
class MADFALLCORE_API FMadSurfaceRegistry
{
public:
	void BeginLoad();

	/** Stages one surface object. Parsing happens in FinishLoad, after patches. */
	bool AddJson(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId, TArray<FMadDefinitionError>& OutErrors);
	int32 AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors);

	/** Applies patches (kind "surface") to the staged JSON, then parses. */
	void FinishLoad(const FMadPatchSet* Patches, TArray<FMadDefinitionError>& OutErrors);

	const FMadSurfaceDefinition* Find(FName MaterialClass) const;
	const TArray<FMadSurfaceDefinition>& GetAll() const { return Surfaces; }

	/** The mesher's vertex colour for a class: the defined colour, or a stable placeholder. */
	FColor GetVertexColor(FName MaterialClass) const;

private:
	struct FStaged
	{
		TSharedPtr<FJsonObject> Object;
		FString SourcePath;
	};
	TMap<FName, FStaged> Staged;
	TArray<FName> StagedOrder;

	TArray<FMadSurfaceDefinition> Surfaces;
	TMap<FName, int32> Index;
};

namespace MadFall
{
	/** Loaded on first use from every source in load order, with patches applied. */
	MADFALLCORE_API const FMadSurfaceRegistry& GetSurfaces();

	namespace Surfaces
	{
		/** The exact sRGB transfer function, clamped to 0..1. */
		inline float SRGBToLinear(float Value)
		{
			Value = FMath::Clamp(Value, 0.0f, 1.0f);
			return Value <= 0.04045f ? Value / 12.92f : FMath::Pow((Value + 0.055f) / 1.055f, 2.4f);
		}
	}
}

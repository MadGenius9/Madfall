// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBlockDefinitionJson.h"

namespace MadFall
{
	inline const TCHAR* PrefabSchemaV1 = TEXT("madfall.prefab/1");

	/**
	 * The palette token meaning "leave whatever is already here".
	 *
	 * Distinct from madfall:air, which carves. A building's interior should be
	 * air - it has to push the terrain out - but the space beside a tower under
	 * its overhanging platform should be void, or every tower would stand in a
	 * square pit dug to the size of its widest floor.
	 */
	inline const TCHAR* PrefabVoidToken = TEXT("*");

	/** Largest prefab footprint. Bounded so a prefab always fits inside one POI cell with margin. */
	inline constexpr int32 PrefabMaxFootprint = 96;
	inline constexpr int32 PrefabMaxHeight = 96;
}

/** How a prefab sits on uneven terrain. */
enum class EMadPrefabConform : uint8
{
	/** Placed at the planned height exactly. Terrain is carved or left floating. */
	None,

	/**
	 * Placed at the median surface height of its footprint, sunk by EmbedDepth,
	 * with a foundation filled down to the terrain under every solid column of
	 * its bottom layer. A building on a slope then has a plinth on the low side
	 * instead of hovering over a gap.
	 */
	Base
};

/** One palette entry: a block in a specific state, or "void". */
struct MADFALLCORE_API FMadPrefabPaletteEntry
{
	FName Block;
	uint8 Orientation = 0;
	uint8 Variant = 0;
	uint8 Density = 255;

	/** Player-construction flag. Defaults on, because prefabs are built things. */
	bool bCubic = true;

	/** Leave the existing voxel alone. */
	bool bVoid = false;

	bool IsAir() const { return !bVoid && Block == FName(TEXT("madfall:air")); }
};

/**
 * A gameplay marker inside a prefab.
 *
 * Type is an FName rather than an enum on purpose: the brief names loot and
 * spawn markers, but a mod adding a trader or a quest giver needs its own
 * marker type without recompiling. Known types are documented in MODDING.md;
 * unknown types are carried through untouched for whatever mod consumes them.
 */
struct MADFALLCORE_API FMadPoiMarker
{
	/** "loot", "spawn", "entrance", or a mod's own type. */
	FName Type;

	/** Local position inside the unrotated prefab. */
	FIntVector Position = FIntVector::ZeroValue;

	/** Loot table id, for loot markers. */
	FName LootTable;

	/** Spawn group id, for spawn markers. */
	FName SpawnGroup;

	/** How many to spawn, for spawn markers. */
	int32 Count = 1;

	/** Trader definition id, for trader markers: who stands here. */
	FName Trader;

	TArray<FName> Tags;
};

/** Where and how often a prefab may be placed. */
struct MADFALLCORE_API FMadPrefabPlacement
{
	/** Relative selection weight among eligible prefabs. 0 means never placed by worldgen. */
	float Rarity = 1.0f;

	EMadPrefabConform Conform = EMadPrefabConform::Base;

	/** Voxels the prefab is sunk into the ground when conforming. */
	int32 EmbedDepth = 1;

	/** Largest surface height difference across the footprint that still accepts a placement. */
	int32 MaxSlope = 8;

	/** Block that fills under a conformed prefab. NAME_None disables foundations. */
	FName FoundationBlock;

	/** Deepest foundation, in voxels. A prefab that would need more is rejected rather than stilted. */
	int32 MaxFoundationDepth = 12;

	/** Biome ids this prefab may appear in. Empty means any land biome. */
	TArray<FName> Biomes;

	/** Allow placement below sea level. */
	bool bUnderwater = false;

	/**
	 * Placed exactly once per world, in the nearest cell around the spawn that
	 * has a site for it, and never anywhere else whatever its rarity. For the
	 * places a new player must be able to find: the first trader.
	 */
	bool bNearSpawn = false;
};

/**
 * A prefab: a block of voxels plus the markers and rules that make it a POI.
 *
 * Stored as `madfall.prefab/1` JSON. JSON is the canonical form, not an
 * editor asset, because the mod-first pillar applies to POIs too: a modder
 * must be able to ship a new ruin without the editor, and a prefab captured in
 * the editor must be diffable in a pull request. See mad.prefab.capture.
 */
struct MADFALLCORE_API FMadPrefab
{
	FName Id;
	FString DisplayName;
	FIntVector Size = FIntVector::ZeroValue;

	/** Difficulty tier 1-5. Drives loot quality and which prefabs appear far from spawn. */
	int32 Tier = 1;

	TArray<FName> Tags;

	TArray<FMadPrefabPaletteEntry> Palette;

	/** Palette index per voxel, X fastest then Y then Z. Size.X * Size.Y * Size.Z entries. */
	TArray<uint16> Voxels;

	TArray<FMadPoiMarker> Markers;

	FMadPrefabPlacement Placement;

	FName SourceModId;
	FString SourcePath;

	int32 VoxelIndex(int32 X, int32 Y, int32 Z) const
	{
		return X + Size.X * (Y + Size.Y * Z);
	}

	bool Contains(const FIntVector& Local) const
	{
		return Local.X >= 0 && Local.Y >= 0 && Local.Z >= 0
			&& Local.X < Size.X && Local.Y < Size.Y && Local.Z < Size.Z;
	}

	const FMadPrefabPaletteEntry& GetEntry(int32 X, int32 Y, int32 Z) const
	{
		return Palette[Voxels[VoxelIndex(X, Y, Z)]];
	}

	/** The first "entrance" marker, or the footprint centre at ground level. Roads aim here. */
	FIntVector GetEntrance() const;

	int32 CountSolidVoxels() const;
};

namespace MadFall::PrefabJson
{
	/** Parses and validates a prefab. Returns false if it is unusable. */
	MADFALLCORE_API bool ParseText(
		const FString& JsonText,
		const FString& SourcePath,
		FName ModId,
		FMadPrefab& OutPrefab,
		TArray<FMadDefinitionError>& OutErrors);

	/**
	 * Serialises a prefab to `madfall.prefab/1` JSON.
	 *
	 * Voxels are run-length encoded as a flat [count, palette_index, ...] array.
	 * A 32^3 building is typically a few hundred runs, which keeps captured
	 * prefabs small enough to review in a diff.
	 */
	MADFALLCORE_API FString WriteText(const FMadPrefab& Prefab);
}

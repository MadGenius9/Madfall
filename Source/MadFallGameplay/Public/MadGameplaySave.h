// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadInventory.h"
#include "MadScriptHost.h"
#include "MadSurvivalModel.h"

namespace MadFall
{
	inline const TCHAR* GameplaySaveSchemaV1 = TEXT("madfall.save/1");
}

/** An active quest's objective counts (FMadQuestLog). */
struct MADFALLGAMEPLAY_API FMadQuestProgress
{
	FName Quest;
	TArray<int32> Counts;
};

struct MADFALLGAMEPLAY_API FMadPlayerSaveData
{
	/** Actor location in centimetres; exact, so a player reloads where they stood, not snapped to a voxel. */
	FVector Location = FVector::ZeroVector;
	/** False when the saved location could not be restored (IsRestorableLocation): start at the spawn column. */
	bool bHasLocation = true;
	FRotator ViewRotation = FRotator::ZeroRotator;
	FIntPoint SpawnColumn = FIntPoint::ZeroValue;

	/** The bed slept in, if any (interact.spawn_point). */
	bool bHasBed = false;
	FIntVector BedVoxel = FIntVector::ZeroValue;

	FMadSurvivalStats Stats;
	int32 Level = 1;
	int32 Experience = 0;

	/** Perk id -> owned rank. Ids of perks from removed mods are kept. */
	TMap<FName, int32> PerkRanks;

	TArray<FMadItemStack> Inventory;

	/** Head, body, legs, feet. */
	TArray<FMadItemStack> Worn;

	TArray<FName> CompletedQuests;
	TArray<FMadQuestProgress> ActiveQuests;
	int32 SelectedSlot = 0;
};

struct MADFALLGAMEPLAY_API FMadContainerSaveData
{
	FIntVector Position = FIntVector::ZeroValue;
	FName LootTable;
	int32 Tier = 1;
	bool bRolled = false;
	TArray<FMadItemStack> Contents;
};

/** One growing plant (see FMadPlantTracker). */
struct MADFALLGAMEPLAY_API FMadPlantSaveData
{
	FIntVector Position = FIntVector::ZeroValue;
	FName Block;

	/** World hour (UMadWorldClockSubsystem::GetTotalHours) the current stage began. */
	double StageStartHour = 0.0;
};

/** A trader's shelves (see UMadTraderSubsystem). */
struct MADFALLGAMEPLAY_API FMadTraderSaveData
{
	FIntVector Position = FIntVector::ZeroValue;
	FName Trader;
	int32 RestockDay = 0;
	TArray<FMadItemStack> Stock;
};

/** A bag of items lying in the world. */
struct MADFALLGAMEPLAY_API FMadPickupSaveData
{
	FVector Location = FVector::ZeroVector;
	TArray<FMadItemStack> Stacks;
	float Lifetime = 900.0f;
	bool bIsBackpack = false;
};

/**
 * Everything about a world that is not voxels.
 *
 * Written as `gameplay.json` next to the region files. JSON rather than a
 * binary archive for the same reasons the definitions are JSON: it is small
 * (kilobytes, not the megabytes voxels are), a player or modder can read and fix
 * it, and items are stored by namespaced id so a save survives a mod being
 * removed - an unknown item stays in its slot, verbatim, until the mod returns.
 *
 * Zombies are deliberately not saved. They are cheap to recreate, the horde
 * director and POI sleepers re-derive them, and a saved zombie standing in a
 * chunk that has since changed is a source of bugs with no gameplay value.
 */
struct MADFALLGAMEPLAY_API FMadGameplaySave
{
	int64 WorldSeed = 0;

	bool bHasPlayer = false;
	FMadPlayerSaveData Player;

	int32 Day = 1;
	float TimeOfDay = 8.0f;

	TArray<FMadContainerSaveData> Containers;

	/** POI spawn marker -> game day its sleepers last woke. */
	TMap<FIntVector, int32> SleeperDays;

	/** Dropped items, including a dead survivor's backpack. */
	TArray<FMadPickupSaveData> Pickups;

	/** Crops part way through growing. */
	TArray<FMadPlantSaveData> Plants;

	/** Chunk columns the survivor has seen, for the map. */
	TArray<FIntPoint> Explored;

	TArray<FMadTraderSaveData> Traders;

	/** Script mods' madfall.store_set values, per mod. A removed mod's values are kept. */
	TMap<FName, TMap<FString, FMadScriptValue>> ModStore;
};

namespace MadFall::GameplaySave
{
	MADFALLGAMEPLAY_API FString ToJson(const FMadGameplaySave& Save);

	/**
	 * Parses a save. Returns false only when the file is unusable (not JSON,
	 * wrong schema); individual bad entries are skipped and reported in
	 * OutWarnings, because one corrupt container must not cost a player their
	 * whole inventory.
	 */
	MADFALLGAMEPLAY_API bool FromJson(const FString& Text, FMadGameplaySave& OutSave, TArray<FString>& OutWarnings);

	/**
	 * Whether a saved player location can be restored: finite, and inside the
	 * world's height range (a few voxels of headroom above the build ceiling).
	 * One that is not starts the player at their spawn column instead, keeping
	 * everything else they had: a location from a bug (a survivor pushed 10 km
	 * up by a colliding sky dome) must not strand them for good.
	 */
	MADFALLGAMEPLAY_API bool IsRestorableLocation(const FVector& Location);

	/**
	 * Writes atomically: to a temporary file, the previous save kept as .bak,
	 * then renamed into place. A crash mid-write leaves either the old save or
	 * the new one, never half of each.
	 */
	MADFALLGAMEPLAY_API bool WriteFile(const FString& Path, const FMadGameplaySave& Save, FString& OutError);

	/** Reads Path, falling back to Path.bak if Path is missing or unreadable. */
	MADFALLGAMEPLAY_API bool ReadFile(const FString& Path, FMadGameplaySave& OutSave, TArray<FString>& OutWarnings, bool& bOutUsedBackup);
}

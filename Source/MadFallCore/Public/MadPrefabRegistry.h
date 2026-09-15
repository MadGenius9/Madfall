// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadPrefab.h"

/**
 * Every loaded prefab.
 *
 * Loaded from `Definitions/prefabs/*.json` and `Mods/<id>/definitions/prefabs/*.json`.
 * Prefabs have no `extends` - a ruin that is "the bunker, but rotated and
 * missing a wall" is a different set of voxels, not a field override.
 */
class MADFALLCORE_API FMadPrefabRegistry
{
public:
	void Reset();

	int32 AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors);

	/** Adds an already-parsed prefab. Later additions with the same id override, with a warning. */
	void Add(FMadPrefab&& Prefab);

	/** Sorts by id so selection is independent of file enumeration order. Call once after loading. */
	void Finalize();

	int32 Num() const { return Prefabs.Num(); }
	const FMadPrefab& Get(int32 Index) const { return Prefabs[Index]; }
	const TArray<FMadPrefab>& GetAll() const { return Prefabs; }

	int32 FindIndex(FName Id) const;

	FString DescribeContents() const;

private:
	TArray<FMadPrefab> Prefabs;
	TMap<FName, int32> IdToIndex;
};

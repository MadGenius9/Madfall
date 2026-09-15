// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBlockDefinitionJson.h"
#include "Templates/Function.h"

class FJsonObject;

/**
 * Where definition files come from, and how they are read.
 *
 * Every definition kind - blocks, biomes, prefabs, items, recipes, loot - is
 * found the same way: first-party files under `Definitions/<kind>/` owned by
 * the `madfall` namespace, then `Mods/<modid>/definitions/<kind>/` for each mod
 * in sorted order. Phase 5 replaces "sorted order" with resolved mod load
 * order; it only has to change ForEachSource.
 */
namespace MadFall::Definitions
{
	/** Calls Visit(Directory, OwningModId) for the first-party folder, then each mod's. */
	MADFALLCORE_API void ForEachSource(const TCHAR* KindFolder,
		TFunctionRef<void(const FString& Directory, FName ModId)> Visit);

	/**
	 * Reads every *.json under Directory in sorted path order and calls
	 * Visit(File, Object) for the root object, or for each object in a root
	 * array. Unreadable files, invalid JSON and non-object entries are reported
	 * and skipped. Returns the number of objects visited.
	 */
	MADFALLCORE_API int32 ForEachJsonObject(const FString& Directory, TArray<FMadDefinitionError>& OutErrors,
		TFunctionRef<void(const FString& File, const TSharedRef<FJsonObject>& Object)> Visit);

	/**
	 * Id rule for definitions that are organised in folders, like loot tables
	 * ("madfall:loot/stone"): the block id rules, plus '/' between non-empty
	 * path segments.
	 */
	MADFALLCORE_API bool IsValidPathId(FName Id, FString& OutReason);
}

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * World difficulty: easy, normal or hard, chosen when a world is created and
 * kept in its world.json.
 *
 * The levels are names, and what they do is data: the tuning definition
 * `madfall:difficulty_<level>` holds multipliers the game reads by key
 * (zombie_damage, zombie_health, horde_size, animal_damage, survival_drain).
 * A key a level does not mention is 1, so a balance mod patches one number
 * and a new multiplier costs one line where it is used.
 */
namespace MadFall::Difficulty
{
	inline const FName Normal = FName(TEXT("normal"));

	/** easy, normal, hard - in menu order. */
	MADFALLCORE_API const TArray<FName>& GetLevels();

	MADFALLCORE_API bool IsValid(FName Level);

	/** A multiplier from `madfall:difficulty_<Level>`; 1 if the level or key is not defined. */
	MADFALLCORE_API float GetScale(FName Level, FName Key);

	/** The multiplier for the world being played. */
	MADFALLCORE_API float GetWorldScale(const UWorld* World, FName Key);

	/** True while playing a creative world (FMadWorldInfo::bCreative). */
	MADFALLCORE_API bool IsCreative(const UWorld* World);
}

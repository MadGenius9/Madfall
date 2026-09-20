// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadHordeSubsystem.generated.h"

class AMadPlayerCharacter;
class AMadZombie;
struct FMadZombieDefinition;

/**
 * Decides where and when zombies exist.
 *
 * HORDE NIGHTS
 *   Every 7th night (UMadWorldClockSubsystem). The horde is sized by game stage
 *   - days survived plus player level - and arrives in waves from a ring
 *   around the player, every member already knowing where the player is. They
 *   do not stumble onto a base: they path to it, and the voxel pathfinder sends
 *   them through the weakest wall. Survivors of the night revert to ordinary
 *   zombies at dawn.
 *
 * POI SLEEPERS
 *   Each POI spawn marker spawns its group once per game day, when the player
 *   first comes within range. Zombies are created on approach rather than at
 *   generation for the same reason loot is rolled on open: nothing is simulated
 *   or saved for places nobody is looking at.
 *
 * WANDERERS
 *   At night, a few zombies from the wander group appear out of sight.
 *
 * A hard cap on live zombies bounds the cost of all three.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadHordeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Spawns a variant at a voxel (feet). Null if the variant is unknown or the cap is reached. */
	AMadZombie* SpawnZombie(const FMadZombieDefinition& Definition, const FIntVector& Feet, bool bHorde);

	/** Starts a horde wave immediately, regardless of the clock. */
	int32 ForceWave();

	/** Horde size for a game stage: how many zombies a whole horde night sends. */
	static int32 HordeSizeForGameStage(int32 GameStage);

	/** A standable voxel near a column, searching the loaded voxel data. */
	bool FindStandableNear(int32 X, int32 Y, int32 AroundZ, FIntVector& OutFeet) const;

	int32 NumAlive() const;

	/** Every live zombie, for the console and tests. Entries may be stale. */
	const TArray<TWeakObjectPtr<AMadZombie>>& GetAlive() const { return Alive; }

	/** Sends every live zombie within RadiusVoxels after Player. Returns how many. */
	int32 AlertNear(const FVector& Location, float RadiusVoxels, AMadPlayerCharacter& Player);

	/** Wandering zombies that come running from out of sight (a screamer's call). Returns how many spawned. */
	int32 CallReinforcements(AMadPlayerCharacter& Player, int32 Count);
	int32 KillAll();
	bool IsHordeActive() const { return bHordeActive; }

	const TMap<FIntVector, int32>& GetSleeperDays() const { return SleeperSpawnDay; }
	void ImportSleeperDays(const TMap<FIntVector, int32>& Days) { SleeperSpawnDay = Days; }
	FString DescribeStatus() const;

private:
	void TickHorde(float DeltaTime, AMadPlayerCharacter& Player);
	void TickSleepers(AMadPlayerCharacter& Player);
	void TickWanderers(float DeltaTime, AMadPlayerCharacter& Player);

	int32 SpawnWave(AMadPlayerCharacter& Player, int32 Count, bool bHorde, FName Group, int32 MinRing, int32 MaxRing);

	/** Weighted pick from a group, unlocked by game stage and at home in a biome. */
	const FMadZombieDefinition* PickVariant(FName Group, int32 GameStage, FName Biome);

	/** The biome a column belongs to, or None if the world is not ready. */
	FName BiomeAt(const FIntVector& Voxel) const;

	TArray<TWeakObjectPtr<AMadZombie>> Alive;

	bool bHordeActive = false;
	int32 HordeSpawned = 0;
	int32 HordeTarget = 0;
	float WaveTimer = 0.0f;
	int32 HordeNightDay = 0;

	float SleeperTimer = 0.0f;
	float WanderTimer = 0.0f;

	/** Marker position -> game day it last spawned. */
	TMap<FIntVector, int32> SleeperSpawnDay;

	int64 TotalSpawned = 0;
	FRandomStream Random{ 0x40DE };
};

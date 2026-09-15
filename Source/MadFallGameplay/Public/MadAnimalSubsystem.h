// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadAnimalSubsystem.generated.h"

class AMadAnimal;
class AMadPlayerCharacter;
struct FMadAnimalDefinition;

/**
 * Decides where animals exist: the wildlife counterpart of the horde director.
 *
 * Every `mad.animals.SpawnSeconds` it picks a column in a ring around the
 * survivor, out of easy sight, looks up that column's biome, and spawns a herd
 * of a species that lives there and is active at this time of day, weighted by
 * spawn weight. Animals far from the survivor, or out of their active hours and
 * away from them, are removed. Nothing is saved: the population is re-derived
 * around wherever the survivor is, like wandering zombies.
 *
 * Biome membership is the only placement rule, so a mod adds an animal to a
 * biome - or a biome of its own - by listing it, without touching this code.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadAnimalSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Spawns one animal with its feet at a voxel. Null past the cap. */
	AMadAnimal* SpawnAnimal(const FMadAnimalDefinition& Definition, const FIntVector& Feet);

	/** Spawns up to Count of a species on standable ground around a column. Returns how many appeared. */
	int32 SpawnHerd(const FMadAnimalDefinition& Definition, const FIntVector& Near, int32 Count);

	/**
	 * Species that live in a biome and are active now, and a weighted pick among
	 * them. Pure over the definitions, for tests.
	 */
	static void GetCandidates(const TArray<FMadAnimalDefinition>& Animals, FName Biome, bool bNight, TArray<const FMadAnimalDefinition*>& Out);
	static const FMadAnimalDefinition* PickWeighted(const TArray<const FMadAnimalDefinition*>& Candidates, float Roll01);

	int32 NumAlive() const;
	int32 KillAll();
	void GetAlive(TArray<AMadAnimal*>& Out) const;

	FString DescribeStatus() const;

private:
	/** One attempt at a natural herd in the ring around the survivor. */
	int32 TrySpawnHerd(AMadPlayerCharacter& Player);
	void DespawnDistant(AMadPlayerCharacter& Player);

	TArray<TWeakObjectPtr<AMadAnimal>> Alive;
	FRandomStream Random{ 0x0A11A1 };
	float SpawnTimer = 5.0f;
	float DespawnTimer = 0.0f;
	int32 TotalSpawned = 0;
	int32 TotalDespawned = 0;
};

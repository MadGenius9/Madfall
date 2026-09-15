// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MadGameplaySave.h"
#include "MadInventory.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadPickupSubsystem.generated.h"

class AMadPlayerCharacter;
class UStaticMeshComponent;

/**
 * A bag of items lying in the world.
 *
 * One actor per drop event rather than per stack: a zombie's loot, a smashed
 * crate's contents or a dead survivor's backpack is one thing to walk over and
 * pick up, and one actor instead of thirty.
 */
UCLASS()
class MADFALLGAMEPLAY_API AMadItemPickup : public AActor
{
	GENERATED_BODY()

public:
	AMadItemPickup();

	virtual void Tick(float DeltaSeconds) override;

	TArray<FMadItemStack> Stacks;

	/** Seconds until despawn. Backpacks get much longer than loose drops. */
	float Lifetime = 900.0f;

	bool bIsBackpack = false;

	bool IsEmpty() const;

private:
	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UStaticMeshComponent> Mesh;

	float Spin = 0.0f;
	float SettleTimer = 0.0f;
};

/**
 * Spawns pickups, collects them when a player walks over them, expires them.
 *
 * Collection is a distance check from the player each tick over this list,
 * not physics overlaps: pickups are few, a squared-distance loop over them is
 * cheaper than keeping overlap volumes registered, and it cannot miss a pickup
 * that spawned inside the player.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadPickupSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Drops items at a location (cm). Empty stacks are ignored; returns null if nothing was dropped. */
	AMadItemPickup* Drop(const FVector& Location, const TArray<FMadItemStack>& Stacks, bool bBackpack = false);

	/** Moves as much as fits from nearby pickups into the player. Returns items collected. */
	int32 CollectNear(AMadPlayerCharacter& Player, float RadiusCm);

	int32 NumPickups() const;

	void ExportState(TArray<FMadPickupSaveData>& Out) const;
	void ImportState(const TArray<FMadPickupSaveData>& In);

private:
	TArray<TWeakObjectPtr<AMadItemPickup>> Pickups;
};

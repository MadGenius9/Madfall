// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MadDamageable.h"
#include "MadGameplayDefinitions.h"
#include "MadVoxelPathfinder.h"
#include "Math/RandomStream.h"
#include "MadAnimal.generated.h"

class AMadPlayerCharacter;
class UMadQuadrupedRigComponent;

UENUM()
enum class EMadAnimalState : uint8
{
	Idle,
	Graze,
	Flee,
	Chase,
	Attack,
	Dead
};

/** What an animal knows about the survivor this think. */
struct FMadAnimalSenses
{
	bool bPlayerPresent = false;
	float DistanceVoxels = TNumericLimits<float>::Max();
	bool bSeesPlayer = false;
	bool bHearsPlayer = false;

	/** Seconds since the player last hurt it; negative if never. */
	float SecondsSinceHurt = -1.0f;
};

namespace MadFall::Animals
{
	/** Seconds a hurt animal remembers it: a skittish one keeps running, a defensive one keeps fighting. */
	inline constexpr float HurtMemorySeconds = 12.0f;

	/** Where a corpse rests from ground heights under its head, middle and tail; Lowest() for none. */
	MADFALLGAMEPLAY_API float CorpseGroundZ(const TArray<float>& GroundHits);

	/** Metres within which an animal attacks rather than chases. */
	inline constexpr float AttackReachVoxels = 1.4f;

	/**
	 * The behaviour rules, pure so they are testable without a world. Given a
	 * definition and what the animal senses, the state it should be in (never
	 * Dead; Graze covers idling and strolling).
	 */
	MADFALLGAMEPLAY_API EMadAnimalState Decide(const FMadAnimalDefinition& Definition, const FMadAnimalSenses& Senses);

	/**
	 * Where to run: a voxel Distance away from the threat, directly away with a
	 * little sideways jitter (Jitter -1..1) so a herd scatters instead of queueing.
	 */
	MADFALLGAMEPLAY_API FIntVector PickFleeGoal(const FIntVector& Feet, const FIntVector& Threat, int32 Distance, float Jitter);
}

/**
 * A wild animal: deer to hunt, a stag that fights back, wolves that hunt you.
 *
 * One class for every species, driven by madfall.animal/1. Thinks a few times a
 * second in its own tick, like a zombie, and uses the same voxel pathfinder,
 * never digging. Health is a plain float: animals take no Gameplay Effects, so
 * GAS would be ceremony. Killing one drops its loot table as a bag, and animals
 * are not saved - the animal director repopulates around the survivor.
 */
UCLASS()
class MADFALLGAMEPLAY_API AMadAnimal : public ACharacter, public IMadDamageable
{
	GENERATED_BODY()

public:
	AMadAnimal();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	void InitialiseFromDefinition(const FMadAnimalDefinition& InDefinition);

	//~ Begin IMadDamageable
	virtual bool ReceiveHit(float Amount, FName DamageType, AActor* Attacker) override;
	virtual bool IsDead() const override { return State == EMadAnimalState::Dead; }
	//~ End IMadDamageable

	float GetHealth() const { return Health; }
	EMadAnimalState GetState() const { return State; }
	FName GetSpeciesId() const { return Definition.Id; }
	FIntVector GetFeetVoxel() const;

	FString DescribeStatus() const;

	/** Lifetime counters for mad.animals.status and CI. */
	static int32 TotalKills;
	static int32 TotalPlayerHits;

protected:
	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UMadQuadrupedRigComponent> Body;

private:
	void Think();
	FMadAnimalSenses Sense() const;
	void RequestPath(const FIntVector& Goal);
	void FollowPath(float DeltaSeconds);
	void TryAttackPlayer(AMadPlayerCharacter& Player);
	void Die(AActor* Killer);
	/** Puts the body on the ground under it before it goes still; see the .cpp. */
	void SettleCorpse();

	FMadAnimalDefinition Definition;
	float Health = 1.0f;

	EMadAnimalState State = EMadAnimalState::Idle;

	/** World seconds the player last hurt it; negative if never. */
	double LastHurtTime = -1.0;

	FMadVoxelPath Path;
	int32 StepIndex = 0;
	FIntVector PathGoal = FIntVector(MAX_int32);

	float ThinkTimer = 0.0f;
	float RepathTimer = 0.0f;
	float GrazeTimer = 0.0f;
	float AttackCooldown = 0.0f;
	float StuckTimer = 0.0f;
	FVector LastProgressLocation = FVector::ZeroVector;
	float DespawnTimer = -1.0f;
	double NextTrapHitTime = 0.0;

	FRandomStream Random;
};

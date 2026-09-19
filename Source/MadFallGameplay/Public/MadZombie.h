// Copyright MadFall. All Rights Reserved.

#pragma once

#include "AbilitySystemInterface.h"
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MadDamageable.h"
#include "MadGameplayDefinitions.h"
#include "MadVoxelPathfinder.h"
#include "Math/RandomStream.h"
#include "MadZombie.generated.h"

class AMadPlayerCharacter;
class UAbilitySystemComponent;
class UMadSurvivalAttributeSet;
class UMadHumanoidRigComponent;

UENUM()
enum class EMadZombieState : uint8
{
	Idle,
	Wander,
	Chase,
	Dig,
	Attack,
	Dead
};

/**
 * A zombie.
 *
 * One class for every variant: the definition (madfall.zombie/1) supplies
 * health, speed, damage and senses, and GAS holds the live numbers so a
 * modded "armoured" buff is a Gameplay Effect rather than a subclass.
 *
 * BEHAVIOUR, in priority order each think:
 *   Attack  the player is within reach: hit them (damage + infection)
 *   Dig     the next path step has blocks in the way: break them
 *   Chase   a target is known: follow the voxel path toward it
 *   Wander  nothing known: stroll to a nearby standable spot, no digging
 *
 * Senses are cheap and deterministic: sight is a voxel raycast within range,
 * hearing is the player's recent noise (sprinting, swinging, placing) within
 * hearing range, and horde zombies simply know where the player is - they are
 * sent, not stumbled upon.
 *
 * AI runs in the pawn's own tick with a staggered think interval rather than
 * through a Behavior Tree. At horde sizes of tens of zombies the logic is four
 * states; a BT asset would add an editor-only dependency for no expressive
 * gain. If variants grow real tactics (spitters, screamers) a BT or StateTree
 * is the right next step, and the verbs below are what its tasks would call.
 */
UCLASS()
class MADFALLGAMEPLAY_API AMadZombie : public ACharacter, public IAbilitySystemInterface, public IMadDamageable
{
	GENERATED_BODY()

public:
	AMadZombie();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystem; }

	/** Applies a variant. Call right after spawning. */
	void InitialiseFromDefinition(const FMadZombieDefinition& Definition, bool bInHorde);

	//~ Begin IMadDamageable
	virtual bool ReceiveHit(float Amount, FName DamageType, AActor* Attacker) override;
	virtual bool IsDead() const override { return State == EMadZombieState::Dead; }
	virtual float GetDamageMultiplier(FName DamageType) const override;
	//~ End IMadDamageable

	float GetHealth() const;
	bool IsHorde() const { return bHorde; }
	void SetHorde(bool bInHorde) { bHorde = bInHorde; }
	EMadZombieState GetState() const { return State; }
	FName GetVariantId() const { return VariantId; }

	/**
	 * The prefab whose spawn marker woke this zombie, or none if it wandered in
	 * or came with a horde. It is what lets a job be about a place: clearing a
	 * building means killing the zombies that building put there, and a wanderer
	 * that followed you in must not count toward it.
	 */
	FName GetPoiPrefab() const { return PoiPrefabId; }
	void SetPoiPrefab(FName InPrefabId, const TArray<FName>& InTags) { PoiPrefabId = InPrefabId; PoiTags = InTags; }

	FString DescribeStatus() const;

	/** Makes the zombie hunt Player: a scream, or anything else that tells it where they are. */
	void AlertTo(AMadPlayerCharacter& Player);

	/** Lifetime counters for mad.ai.status and CI. */
	static int32 TotalBlocksHit;
	static int32 TotalPlayerHits;
	static int32 TotalPaths;
	static int32 TotalKills;
	static int32 TotalUndermines;
	static int32 TotalBreaches;
	static int32 TotalSpits;
	static int32 TotalScreams;

	/** Voxels climbed, up ladders and walls. */
	static int32 TotalClimbs;

	/** Floors dug through toward a target below. */
	static int32 TotalDigDowns;

protected:
	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UAbilitySystemComponent> AbilitySystem;

	UPROPERTY()
	TObjectPtr<UMadSurvivalAttributeSet> Attributes;

	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UMadHumanoidRigComponent> Body;

private:
	void Think();
	void Sense();
	void RequestPath(const FIntVector& Goal, bool bAllowDigging);

	/** Target out of reach above: replaces the path with one step that breaks what they stand on. */
	bool TryUndermine(const FIntVector& Goal);

	/** Breaks toward a goal no path reaches, from the end of the best partial path (MadFall::Pathfinding::FindBreachTarget). */
	bool TryBreach(const FIntVector& Goal);
	void FollowPath(float DeltaSeconds);
	void TryAttackPlayer();

	/** A ranged attack at the target, if it can see them and is ready. True if it fired. */
	bool TrySpit(AMadPlayerCharacter& Player, float DistanceVoxels);

	/** Hurt and slowed by a trap it stands in. Called each think. */
	void UpdateTrap();
	void Scream(AMadPlayerCharacter& Player);
	void Die(AActor* Killer);
	FIntVector GetFeetVoxel() const;
	FIntVector PickWanderGoal();

	FName VariantId;

	/** Set when a POI's spawn marker woke this one; see GetPoiPrefab. */
	FName PoiPrefabId;
	TArray<FName> PoiTags;
	FMadZombieDefinition Definition;
	bool bHorde = false;

	EMadZombieState State = EMadZombieState::Idle;
	TWeakObjectPtr<AMadPlayerCharacter> Target;
	FIntVector LastKnownTarget = FIntVector::ZeroValue;

	/** The support block last chosen, so the undermine counter counts blocks, not re-plans. */
	FIntVector LastUndermineBlock = FIntVector(MAX_int32);
	FIntVector LastBreachBlock = FIntVector(MAX_int32);
	bool bHasLastKnown = false;

	FMadVoxelPath Path;
	int32 StepIndex = 0;
	FIntVector PathGoal = FIntVector(MAX_int32);

	float GroanTimer = 4.0f;
	float RangedCooldown = 0.0f;
	float ScreamTimer = 0.0f;
	bool bSeesTarget = false;
	float TrapSlow = 1.0f;
	double NextTrapHitTime = 0.0;
	float ThinkTimer = 0.0f;
	float RepathTimer = 0.0f;
	float AttackCooldown = 0.0f;
	float StuckTimer = 0.0f;
	FVector LastProgressLocation = FVector::ZeroVector;
	float DespawnTimer = -1.0f;

	FRandomStream Random;
};

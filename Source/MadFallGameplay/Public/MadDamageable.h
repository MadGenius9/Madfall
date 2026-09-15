// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "MadDamageable.generated.h"

UINTERFACE(meta = (CannotImplementInterfaceInBlueprint))
class MADFALLGAMEPLAY_API UMadDamageable : public UInterface
{
	GENERATED_BODY()
};

/**
 * Anything a survivor's swing can hurt: zombies and animals.
 *
 * An interface rather than a shared base class because the two have nothing
 * else in common worth inheriting - a zombie is GAS-backed and digs, an animal
 * is a plain health float and runs - and the swing only needs these two calls.
 */
class MADFALLGAMEPLAY_API IMadDamageable
{
	GENERATED_BODY()

public:
	/**
	 * Damage from the player or the world, before resistance: the victim applies
	 * its own GetDamageMultiplier, so traps, arrows and debris all respect it.
	 * Returns true if this killed it.
	 */
	virtual bool ReceiveHit(float Amount, FName DamageType, AActor* Attacker) = 0;

	virtual bool IsDead() const = 0;

	/** How much of a damage type gets through: 1 unless the creature resists or is weak to it. */
	virtual float GetDamageMultiplier(FName DamageType) const { return 1.0f; }
};

namespace MadFall::Combat
{
	/**
	 * The damage type a weapon deals best against a victim: the entry whose
	 * amount times the victim's multiplier is largest. Returns the amount before
	 * resistance (ReceiveHit applies it) and the type; 0 for an empty map. A
	 * tie keeps the first entry in the map's order.
	 */
	inline float ChooseDamage(const TMap<FName, float>& Damage, TFunctionRef<float(FName)> Multiplier, FName& OutType)
	{
		float BestDealt = -1.0f;
		float BestAmount = 0.0f;
		for (const TPair<FName, float>& Pair : Damage)
		{
			const float Dealt = Pair.Value * Multiplier(Pair.Key);
			if (Dealt > BestDealt)
			{
				BestDealt = Dealt;
				BestAmount = Pair.Value;
				OutType = Pair.Key;
			}
		}
		return BestAmount;
	}
}

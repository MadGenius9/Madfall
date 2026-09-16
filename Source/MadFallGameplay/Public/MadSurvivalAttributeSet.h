// Copyright MadFall. All Rights Reserved.

#pragma once

#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "CoreMinimal.h"
#include "MadSurvivalModel.h"
#include "MadSurvivalAttributeSet.generated.h"

/**
 * Survival vitals as GAS attributes.
 *
 * GAS owns the numbers so buffs, debuffs, armour and perks are Gameplay
 * Effects - data a mod can ship - rather than special cases in C++. The
 * per-frame survival simulation is NOT a periodic Gameplay Effect: it is
 * MadFall::Survival::Step, a pure function with its own tests, driven by
 * UMadSurvivalComponent reading and writing these attributes. That keeps the
 * balance maths testable without an ability system, and keeps GAS for what it
 * is good at: stacking, tagged, timed modifiers.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadSurvivalAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UMadSurvivalAttributeSet();

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, MaxHealth)

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData Stamina;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, Stamina)

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData MaxStamina;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, MaxStamina)

	/** Satiety, 100 = full. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData Food;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, Food)

	/** Hydration, 100 = full. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData Water;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, Water)

	/** Degrees Celsius. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData CoreTemperature;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, CoreTemperature)

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData Infection;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, Infection)

	/** Air left with the head under water, 0-100. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Survival")
	FGameplayAttributeData Breath;
	ATTRIBUTE_ACCESSORS_BASIC(UMadSurvivalAttributeSet, Breath)

	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;

	/** Copies current values out, for the survival model. */
	FMadSurvivalStats ToStats() const;

	/** Writes base values back through the ability system, so active effects stay applied on top. */
	static void WriteStats(UAbilitySystemComponent& AbilitySystem, const FMadSurvivalStats& Stats);

private:
	void Clamp(const FGameplayAttribute& Attribute, float& NewValue) const;
};

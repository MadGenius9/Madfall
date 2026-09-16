// Copyright MadFall. All Rights Reserved.

#include "MadSurvivalAttributeSet.h"

UMadSurvivalAttributeSet::UMadSurvivalAttributeSet()
{
	const FMadSurvivalStats Defaults;
	InitHealth(Defaults.Health);
	InitMaxHealth(Defaults.MaxHealth);
	InitStamina(Defaults.Stamina);
	InitMaxStamina(Defaults.MaxStamina);
	InitFood(Defaults.Food);
	InitWater(Defaults.Water);
	InitCoreTemperature(Defaults.CoreTemperature);
	InitInfection(Defaults.Infection);
	InitBreath(Defaults.Breath);
}

void UMadSurvivalAttributeSet::Clamp(const FGameplayAttribute& Attribute, float& NewValue) const
{
	if (Attribute == GetHealthAttribute())               { NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth()); }
	else if (Attribute == GetStaminaAttribute())         { NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxStamina()); }
	else if (Attribute == GetMaxHealthAttribute()
		|| Attribute == GetMaxStaminaAttribute())        { NewValue = FMath::Max(1.0f, NewValue); }
	else if (Attribute == GetFoodAttribute()
		|| Attribute == GetWaterAttribute()
		|| Attribute == GetInfectionAttribute()
		|| Attribute == GetBreathAttribute())            { NewValue = FMath::Clamp(NewValue, 0.0f, 100.0f); }
	else if (Attribute == GetCoreTemperatureAttribute()) { NewValue = FMath::Clamp(NewValue, 25.0f, 45.0f); }
}

void UMadSurvivalAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	Clamp(Attribute, NewValue);
}

void UMadSurvivalAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);
	Clamp(Attribute, NewValue);
}

FMadSurvivalStats UMadSurvivalAttributeSet::ToStats() const
{
	FMadSurvivalStats Stats;
	Stats.Health = GetHealth();
	Stats.MaxHealth = GetMaxHealth();
	Stats.Stamina = GetStamina();
	Stats.MaxStamina = GetMaxStamina();
	Stats.Food = GetFood();
	Stats.Water = GetWater();
	Stats.CoreTemperature = GetCoreTemperature();
	Stats.Infection = GetInfection();
	Stats.Breath = GetBreath();
	return Stats;
}

void UMadSurvivalAttributeSet::WriteStats(UAbilitySystemComponent& AbilitySystem, const FMadSurvivalStats& Stats)
{
	AbilitySystem.SetNumericAttributeBase(GetHealthAttribute(), Stats.Health);
	AbilitySystem.SetNumericAttributeBase(GetStaminaAttribute(), Stats.Stamina);
	AbilitySystem.SetNumericAttributeBase(GetFoodAttribute(), Stats.Food);
	AbilitySystem.SetNumericAttributeBase(GetWaterAttribute(), Stats.Water);
	AbilitySystem.SetNumericAttributeBase(GetCoreTemperatureAttribute(), Stats.CoreTemperature);
	AbilitySystem.SetNumericAttributeBase(GetInfectionAttribute(), Stats.Infection);
	AbilitySystem.SetNumericAttributeBase(GetBreathAttribute(), Stats.Breath);
}

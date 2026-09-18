// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A survivor's vital state.
 *
 * Food and Water are SATIETY (100 = full, 0 = starving), not hunger, so every
 * bar in the HUD reads the same way: full is good. Consumable effects use the
 * same names ("food": +35).
 *
 * CoreTemperature is degrees Celsius and moves toward what the environment
 * dictates rather than being set by it - stepping outside in a blizzard is a
 * countdown, not an instant penalty.
 */
struct MADFALLGAMEPLAY_API FMadSurvivalStats
{
	float Health = 100.0f;
	float MaxHealth = 100.0f;
	float Stamina = 100.0f;
	float MaxStamina = 100.0f;
	float Food = 100.0f;
	float Water = 100.0f;
	float CoreTemperature = 37.0f;

	/** 0-100. Grows once contracted, cured only by medicine. */
	float Infection = 0.0f;

	/** Air left with the head under water, 0-100. Refills in seconds at the surface. */
	float Breath = 100.0f;

	bool IsDead() const { return Health <= 0.0f; }
};

/** What the world is doing to the survivor this frame. */
struct MADFALLGAMEPLAY_API FMadSurvivalEnvironment
{
	/** Air temperature at the survivor, Celsius. */
	float AmbientTemperature = 20.0f;

	/** Degrees of cold protection from clothing and shelter. Shifts the comfort band down. */
	float ColdInsulation = 0.0f;

	/** Degrees of heat protection. Shifts the comfort band up. */
	float HeatInsulation = 0.0f;

	bool bSprinting = false;

	/**
	 * How much of the survivor is in water, 0 to 1, and whether their head is
	 * under it. Water pulls heat out of a body far faster than air does and wet
	 * clothing stops insulating, so a swim in a cold lake is dangerous in a way
	 * standing in the same air is not.
	 */
	float SubmergedFraction = 0.0f;
	bool bHeadUnderwater = false;

	/** Swinging a tool or weapon. Stamina does not regenerate while exerting. */
	bool bExerting = false;

	/** Perk multipliers on food and water loss. */
	float FoodDrainMultiplier = 1.0f;
	float WaterDrainMultiplier = 1.0f;
};

/**
 * Balance numbers. Defaults are the shipped tuning; Phase 5 exposes them as a
 * `madfall.survival/1` definition so a hardcore mod is a data file.
 */
struct MADFALLGAMEPLAY_API FMadSurvivalTuning
{
	/** Satiety lost per real-time minute at rest. 0.5 = 200 minutes from full to empty. */
	float FoodPerMinute = 0.5f;
	float WaterPerMinute = 0.8f;

	/** Drain multiplier while sprinting. */
	float SprintDrainMultiplier = 2.5f;

	float StaminaRegenPerSecond = 15.0f;
	float SprintStaminaPerSecond = 10.0f;

	/** Below this satiety, stamina regenerates at half speed. */
	float WeakThreshold = 20.0f;

	float StarvationDamagePerSecond = 0.5f;
	float DehydrationDamagePerSecond = 0.8f;

	/** Natural healing, only while both food and water are above WellFedThreshold. */
	float HealthRegenPerSecond = 0.15f;
	float WellFedThreshold = 60.0f;

	/** Comfortable ambient range, before insulation. */
	float ComfortMin = 5.0f;
	float ComfortMax = 30.0f;

	/** Core temperature drifts this many degrees per second per degree of ambient discomfort. */
	float CoreDriftPerSecondPerDegree = 0.0005f;

	/** Core returns toward 37 at this rate when comfortable. */
	float CoreRecoveryPerSecond = 0.05f;

	float HypothermiaBelow = 35.0f;
	float HyperthermiaAbove = 39.0f;
	float ExposureDamagePerSecondPerDegree = 0.4f;

	/** Heat stroke also dehydrates. */
	float HyperthermiaWaterPerSecond = 0.05f;

	/** Seconds of air: how long a full breath lasts with the head under. */
	float BreathSeconds = 40.0f;

	/** Breath refilled per second once the head is out; a gasp, not a slow recovery. */
	float BreathRecoveryPerSecond = 35.0f;

	/** Damage per second once the air is gone. */
	float DrowningDamagePerSecond = 6.0f;

	/**
	 * How many degrees colder water feels than the air above it, at full
	 * submersion, and how much of the survivor's cold protection wet clothing
	 * loses. Both are why swimming in winter is a bad idea.
	 */
	float WaterChillDegrees = 14.0f;
	float WetInsulationLoss = 0.8f;

	/** Infection growth per second once above zero. */
	float InfectionGrowthPerSecond = 0.02f;

	/** Above this, infection damages health proportionally toward 100. */
	float InfectionDamageAbove = 50.0f;
	float InfectionDamagePerSecondAtFull = 1.0f;
};

/** Damage by cause, for the death screen and the HUD's warning icons. */
struct MADFALLGAMEPLAY_API FMadSurvivalStepResult
{
	float StarvationDamage = 0.0f;
	float DehydrationDamage = 0.0f;
	float ExposureDamage = 0.0f;
	float InfectionDamage = 0.0f;
	float DrowningDamage = 0.0f;

	float TotalDamage() const { return StarvationDamage + DehydrationDamage + ExposureDamage + InfectionDamage + DrowningDamage; }
};

namespace MadFall::Survival
{
	/**
	 * The worst cause in a step's damage, as a word for "You died of ...".
	 *
	 * Pure, and shared by the death message and the death screen, because they
	 * disagreed once already: drowning was added to the model without being
	 * added to the list the message chose from, so a survivor who drowned was
	 * told they died of their wounds.
	 */
	MADFALLGAMEPLAY_API FString WorstCause(const FMadSurvivalStepResult& Causes);

	/**
	 * Advances the vitals by DeltaSeconds. Pure and frame-rate independent for
	 * any dt up to a second (larger steps are subdivided), so the same code
	 * serves the live component and a fast-forward when a save is loaded.
	 */
	MADFALLGAMEPLAY_API FMadSurvivalStepResult Step(FMadSurvivalStats& Stats, const FMadSurvivalEnvironment& Environment,
		const FMadSurvivalTuning& Tuning, float DeltaSeconds);

	/**
	 * Applies instant effects by stat name: health, stamina, food, water,
	 * infection, temperature. Unknown names are ignored and returned in
	 * OutUnknown so content errors are visible. Results are clamped.
	 */
	MADFALLGAMEPLAY_API void ApplyEffects(FMadSurvivalStats& Stats, const TMap<FName, float>& Effects,
		TArray<FName>* OutUnknown = nullptr);
}

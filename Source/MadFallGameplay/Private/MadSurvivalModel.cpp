// Copyright MadFall. All Rights Reserved.

#include "MadSurvivalModel.h"

namespace MadFall::Survival
{
	namespace
	{
		void StepOnce(FMadSurvivalStats& S, const FMadSurvivalEnvironment& Env, const FMadSurvivalTuning& T, float Dt,
			FMadSurvivalStepResult& Result)
		{
			const float Drain = Env.bSprinting ? T.SprintDrainMultiplier : 1.0f;
			S.Food = FMath::Max(0.0f, S.Food - T.FoodPerMinute / 60.0f * Drain * Env.FoodDrainMultiplier * Dt);
			S.Water = FMath::Max(0.0f, S.Water - T.WaterPerMinute / 60.0f * Drain * Env.WaterDrainMultiplier * Dt);

			// --- stamina --------------------------------------------------------
			if (Env.bSprinting)
			{
				S.Stamina -= T.SprintStaminaPerSecond * Dt;
			}
			else if (!Env.bExerting)
			{
				const bool bWeak = S.Food < T.WeakThreshold || S.Water < T.WeakThreshold;
				S.Stamina += T.StaminaRegenPerSecond * (bWeak ? 0.5f : 1.0f) * Dt;
			}
			S.Stamina = FMath::Clamp(S.Stamina, 0.0f, S.MaxStamina);

			// --- breath ---------------------------------------------------------
			// Only the head matters: a survivor up to the chin is breathing.
			float Drowning = 0.0f;
			if (Env.bHeadUnderwater)
			{
				S.Breath = FMath::Max(0.0f, S.Breath - 100.0f / FMath::Max(T.BreathSeconds, 1.0f) * Dt);
				if (S.Breath <= 0.0f)
				{
					Drowning = T.DrowningDamagePerSecond * Dt;
				}
			}
			else
			{
				S.Breath = FMath::Min(100.0f, S.Breath + T.BreathRecoveryPerSecond * Dt);
			}

			// --- temperature ----------------------------------------------------
			// Water is the environment when the survivor is in it: colder than
			// the air above it, and wet clothing has all but stopped insulating.
			const float Wetness = FMath::Clamp(Env.SubmergedFraction, 0.0f, 1.0f);
			const float Ambient = Env.AmbientTemperature - T.WaterChillDegrees * Wetness;
			const float ComfortMin = T.ComfortMin - Env.ColdInsulation * (1.0f - T.WetInsulationLoss * Wetness);
			const float ComfortMax = T.ComfortMax + Env.HeatInsulation;

			if (Ambient < ComfortMin)
			{
				S.CoreTemperature -= (ComfortMin - Ambient) * T.CoreDriftPerSecondPerDegree * Dt;
			}
			else if (Ambient > ComfortMax)
			{
				S.CoreTemperature += (Ambient - ComfortMax) * T.CoreDriftPerSecondPerDegree * Dt;
			}
			else
			{
				S.CoreTemperature = FMath::FInterpConstantTo(S.CoreTemperature, 37.0f, Dt, T.CoreRecoveryPerSecond);
			}
			S.CoreTemperature = FMath::Clamp(S.CoreTemperature, 25.0f, 45.0f);

			float Exposure = 0.0f;
			if (S.CoreTemperature < T.HypothermiaBelow)
			{
				Exposure = (T.HypothermiaBelow - S.CoreTemperature) * T.ExposureDamagePerSecondPerDegree * Dt;
			}
			else if (S.CoreTemperature > T.HyperthermiaAbove)
			{
				Exposure = (S.CoreTemperature - T.HyperthermiaAbove) * T.ExposureDamagePerSecondPerDegree * Dt;
				S.Water = FMath::Max(0.0f, S.Water - T.HyperthermiaWaterPerSecond * Dt);
			}

			// --- infection ------------------------------------------------------
			float InfectionDamage = 0.0f;
			if (S.Infection > 0.0f)
			{
				S.Infection = FMath::Min(100.0f, S.Infection + T.InfectionGrowthPerSecond * Dt);
				if (S.Infection > T.InfectionDamageAbove)
				{
					const float Severity = (S.Infection - T.InfectionDamageAbove) / FMath::Max(1.0f, 100.0f - T.InfectionDamageAbove);
					InfectionDamage = Severity * T.InfectionDamagePerSecondAtFull * Dt;
				}
			}

			// --- health ---------------------------------------------------------
			const float Starvation = S.Food <= 0.0f ? T.StarvationDamagePerSecond * Dt : 0.0f;
			const float Dehydration = S.Water <= 0.0f ? T.DehydrationDamagePerSecond * Dt : 0.0f;

			Result.StarvationDamage += Starvation;
			Result.DehydrationDamage += Dehydration;
			Result.ExposureDamage += Exposure;
			Result.InfectionDamage += InfectionDamage;
			Result.DrowningDamage += Drowning;

			const float Damage = Starvation + Dehydration + Exposure + InfectionDamage + Drowning;
			const bool bWellFed = S.Food > T.WellFedThreshold && S.Water > T.WellFedThreshold;
			const float Healing = (bWellFed && Damage <= 0.0f && S.Infection <= T.InfectionDamageAbove) ? T.HealthRegenPerSecond * Dt : 0.0f;

			S.Health = FMath::Clamp(S.Health - Damage + Healing, 0.0f, S.MaxHealth);
		}
	}

	FMadSurvivalStepResult Step(FMadSurvivalStats& Stats, const FMadSurvivalEnvironment& Environment,
		const FMadSurvivalTuning& Tuning, float DeltaSeconds)
	{
		FMadSurvivalStepResult Result;
		if (DeltaSeconds <= 0.0f || Stats.IsDead())
		{
			return Result;
		}

		// Every rate above is linear within a step except the thresholds; one
		// second is fine enough that crossing one mid-step is not noticeable.
		float Remaining = DeltaSeconds;
		while (Remaining > 0.0f && !Stats.IsDead())
		{
			const float Dt = FMath::Min(Remaining, 1.0f);
			StepOnce(Stats, Environment, Tuning, Dt, Result);
			Remaining -= Dt;
		}
		return Result;
	}

	void ApplyEffects(FMadSurvivalStats& Stats, const TMap<FName, float>& Effects, TArray<FName>* OutUnknown)
	{
		static const FName Health(TEXT("health"));
		static const FName Stamina(TEXT("stamina"));
		static const FName Food(TEXT("food"));
		static const FName Water(TEXT("water"));
		static const FName Infection(TEXT("infection"));
		static const FName Temperature(TEXT("temperature"));

		for (const TPair<FName, float>& Effect : Effects)
		{
			if (Effect.Key == Health)           { Stats.Health = FMath::Clamp(Stats.Health + Effect.Value, 0.0f, Stats.MaxHealth); }
			else if (Effect.Key == Stamina)     { Stats.Stamina = FMath::Clamp(Stats.Stamina + Effect.Value, 0.0f, Stats.MaxStamina); }
			else if (Effect.Key == Food)        { Stats.Food = FMath::Clamp(Stats.Food + Effect.Value, 0.0f, 100.0f); }
			else if (Effect.Key == Water)       { Stats.Water = FMath::Clamp(Stats.Water + Effect.Value, 0.0f, 100.0f); }
			else if (Effect.Key == Infection)   { Stats.Infection = FMath::Clamp(Stats.Infection + Effect.Value, 0.0f, 100.0f); }
			else if (Effect.Key == Temperature) { Stats.CoreTemperature = FMath::Clamp(Stats.CoreTemperature + Effect.Value, 25.0f, 45.0f); }
			else if (OutUnknown != nullptr)     { OutUnknown->Add(Effect.Key); }
		}
	}
}

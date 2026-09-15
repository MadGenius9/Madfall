// Copyright MadFall. All Rights Reserved.

#include "MadProgression.h"

#include "MadFallGameplay.h"

namespace MadFall::Perks
{
	const TSet<FName>& GetKnownStats()
	{
		static const TSet<FName> Stats = {
			FName(TEXT("mining_damage")), FName(TEXT("melee_damage")), FName(TEXT("stamina_cost")), FName(TEXT("craft_time")),
			FName(TEXT("max_health")), FName(TEXT("max_stamina")), FName(TEXT("food_drain")), FName(TEXT("water_drain"))
		};
		return Stats;
	}

	const TCHAR* ToString(EMadPerkResult Result)
	{
		switch (Result)
		{
		case EMadPerkResult::Ok:          return TEXT("ok");
		case EMadPerkResult::UnknownPerk: return TEXT("unknown perk");
		case EMadPerkResult::MaxRank:     return TEXT("already at max rank");
		case EMadPerkResult::LevelTooLow: return TEXT("level too low for the next rank");
		case EMadPerkResult::MissingPrerequisite: return TEXT("another perk is required first");
		case EMadPerkResult::NoPoints:    return TEXT("no perk points");
		default:                          return TEXT("?");
		}
	}

	int32 GetUnspentPoints(int32 Level, const TMap<FName, int32>& Ranks)
	{
		int32 Spent = 0;
		for (const TPair<FName, int32>& Pair : Ranks)
		{
			Spent += FMath::Max(0, Pair.Value);
		}
		return FMath::Max(0, Level - 1 - Spent);
	}

	float GetMultiplier(const TMap<FName, int32>& Ranks, const FMadGameplayDefinitions& Definitions, FName Stat)
	{
		float Multiplier = 1.0f;
		for (const TPair<FName, int32>& Pair : Ranks)
		{
			const FMadPerkDefinition* Perk = Definitions.FindPerk(Pair.Key);
			if (Perk == nullptr || Pair.Value <= 0)
			{
				continue;
			}
			const int32 RankIndex = FMath::Min(Pair.Value, Perk->Ranks.Num()) - 1;
			if (const float* Value = Perk->Ranks[RankIndex].Modifiers.Find(Stat))
			{
				Multiplier *= FMath::Max(0.0f, *Value);
			}
		}
		return Multiplier;
	}

	EMadPerkResult TryBuy(TMap<FName, int32>& Ranks, FName PerkId, int32 Level, const FMadGameplayDefinitions& Definitions)
	{
		const FMadPerkDefinition* Perk = Definitions.FindPerk(PerkId);
		if (Perk == nullptr)
		{
			return EMadPerkResult::UnknownPerk;
		}

		const int32 Owned = Ranks.FindRef(PerkId);
		if (Owned >= Perk->Ranks.Num())
		{
			return EMadPerkResult::MaxRank;
		}
		if (Level < Perk->Ranks[Owned].RequiredLevel)
		{
			return EMadPerkResult::LevelTooLow;
		}
		FName MissingPerk;
		int32 MissingRank = 0;
		if (FindMissingPrerequisite(Ranks, Perk->Ranks[Owned], MissingPerk, MissingRank))
		{
			return EMadPerkResult::MissingPrerequisite;
		}
		if (GetUnspentPoints(Level, Ranks) <= 0)
		{
			return EMadPerkResult::NoPoints;
		}

		Ranks.Add(PerkId, Owned + 1);
		return EMadPerkResult::Ok;
	}

	EMadPerkResult CanBuy(const TMap<FName, int32>& Ranks, FName PerkId, int32 Level, const FMadGameplayDefinitions& Definitions)
	{
		TMap<FName, int32> Copy = Ranks;
		return TryBuy(Copy, PerkId, Level, Definitions);
	}

	bool FindMissingPrerequisite(const TMap<FName, int32>& Ranks, const FMadPerkRank& Rank, FName& OutPerk, int32& OutRank)
	{
		TArray<FName> Required;
		Rank.Requires.GenerateKeyArray(Required);
		Required.Sort(FNameLexicalLess());
		for (const FName& Perk : Required)
		{
			if (Ranks.FindRef(Perk) < Rank.Requires[Perk])
			{
				OutPerk = Perk;
				OutRank = Rank.Requires[Perk];
				return true;
			}
		}
		return false;
	}

	int32 GetTreeDepth(const FMadGameplayDefinitions& Definitions, FName PerkId)
	{
		// Load removed cycles, but a depth cap keeps a bad definition set built
		// by hand (tests, tools) from recursing forever.
		TFunction<int32(FName, int32)> Depth = [&](FName Id, int32 Guard) -> int32
		{
			const FMadPerkDefinition* Perk = Definitions.FindPerk(Id);
			if (Perk == nullptr || Guard > 32)
			{
				return 0;
			}
			const FName Group = Perk->Tags.Num() > 0 ? Perk->Tags[0] : NAME_None;
			int32 Deepest = -1;
			for (const FMadPerkRank& Rank : Perk->Ranks)
			{
				for (const TPair<FName, int32>& Required : Rank.Requires)
				{
					const FMadPerkDefinition* Other = Definitions.FindPerk(Required.Key);
					if (Other != nullptr && (Other->Tags.Num() > 0 ? Other->Tags[0] : NAME_None) == Group)
					{
						Deepest = FMath::Max(Deepest, Depth(Required.Key, Guard + 1));
					}
				}
			}
			return Deepest + 1;
		};
		return Depth(PerkId, 0);
	}

	TArray<int32> GetTreeOrder(const FMadGameplayDefinitions& Definitions)
	{
		const TArray<FMadPerkDefinition>& Perks = Definitions.GetPerks();
		TArray<int32> Order;
		TArray<int32> Depths;
		for (int32 Index = 0; Index < Perks.Num(); ++Index)
		{
			Order.Add(Index);
			Depths.Add(GetTreeDepth(Definitions, Perks[Index].Id));
		}
		Order.StableSort([&Perks, &Depths](int32 A, int32 B)
		{
			const bool bTagA = Perks[A].Tags.Num() > 0;
			const bool bTagB = Perks[B].Tags.Num() > 0;
			if (bTagA != bTagB)
			{
				return bTagA;
			}
			if (bTagA && Perks[A].Tags[0] != Perks[B].Tags[0])
			{
				return Perks[A].Tags[0].LexicalLess(Perks[B].Tags[0]);
			}
			if (Depths[A] != Depths[B])
			{
				return Depths[A] < Depths[B];
			}
			return Perks[A].Id.LexicalLess(Perks[B].Id);
		});
		return Order;
	}

	FString DescribeRank(const FMadPerkRank& Rank)
	{
		// Sorted by stat so the text does not depend on map order.
		TArray<FName> Stats;
		Rank.Modifiers.GetKeys(Stats);
		Stats.Sort(FNameLexicalLess());

		FString Out;
		for (const FName& Stat : Stats)
		{
			const int32 Percent = FMath::RoundToInt32((Rank.Modifiers[Stat] - 1.0f) * 100.0f);
			Out += FString::Printf(TEXT("%s%s %s%d%%"), Out.IsEmpty() ? TEXT("") : TEXT(", "),
				*Stat.ToString().Replace(TEXT("_"), TEXT(" ")), Percent >= 0 ? TEXT("+") : TEXT(""), Percent);
		}
		return Out;
	}
}

namespace MadFall::Survival
{
	void ApplyTuning(const FMadTuningDefinition& Definition, FMadSurvivalTuning& T, TArray<FName>& OutUnknownKeys)
	{
		static const TMap<FName, float FMadSurvivalTuning::*> Fields = {
			{ FName(TEXT("food_per_minute")), &FMadSurvivalTuning::FoodPerMinute },
			{ FName(TEXT("water_per_minute")), &FMadSurvivalTuning::WaterPerMinute },
			{ FName(TEXT("sprint_drain_multiplier")), &FMadSurvivalTuning::SprintDrainMultiplier },
			{ FName(TEXT("stamina_regen_per_second")), &FMadSurvivalTuning::StaminaRegenPerSecond },
			{ FName(TEXT("sprint_stamina_per_second")), &FMadSurvivalTuning::SprintStaminaPerSecond },
			{ FName(TEXT("weak_threshold")), &FMadSurvivalTuning::WeakThreshold },
			{ FName(TEXT("starvation_damage_per_second")), &FMadSurvivalTuning::StarvationDamagePerSecond },
			{ FName(TEXT("dehydration_damage_per_second")), &FMadSurvivalTuning::DehydrationDamagePerSecond },
			{ FName(TEXT("health_regen_per_second")), &FMadSurvivalTuning::HealthRegenPerSecond },
			{ FName(TEXT("well_fed_threshold")), &FMadSurvivalTuning::WellFedThreshold },
			{ FName(TEXT("comfort_min")), &FMadSurvivalTuning::ComfortMin },
			{ FName(TEXT("comfort_max")), &FMadSurvivalTuning::ComfortMax },
			{ FName(TEXT("core_drift_per_second_per_degree")), &FMadSurvivalTuning::CoreDriftPerSecondPerDegree },
			{ FName(TEXT("core_recovery_per_second")), &FMadSurvivalTuning::CoreRecoveryPerSecond },
			{ FName(TEXT("hypothermia_below")), &FMadSurvivalTuning::HypothermiaBelow },
			{ FName(TEXT("hyperthermia_above")), &FMadSurvivalTuning::HyperthermiaAbove },
			{ FName(TEXT("exposure_damage_per_second_per_degree")), &FMadSurvivalTuning::ExposureDamagePerSecondPerDegree },
			{ FName(TEXT("hyperthermia_water_per_second")), &FMadSurvivalTuning::HyperthermiaWaterPerSecond },
			{ FName(TEXT("infection_growth_per_second")), &FMadSurvivalTuning::InfectionGrowthPerSecond },
			{ FName(TEXT("infection_damage_above")), &FMadSurvivalTuning::InfectionDamageAbove },
			{ FName(TEXT("infection_damage_per_second_at_full")), &FMadSurvivalTuning::InfectionDamagePerSecondAtFull }
		};

		for (const TPair<FName, float>& Pair : Definition.Values)
		{
			if (float FMadSurvivalTuning::* const* Field = Fields.Find(Pair.Key))
			{
				T.*(*Field) = Pair.Value;
			}
			else
			{
				OutUnknownKeys.Add(Pair.Key);
			}
		}
	}

	FMadSurvivalTuning GetConfiguredTuning()
	{
		FMadSurvivalTuning Tuning;
		if (const FMadTuningDefinition* Definition = MadFall::GetGameplayDefinitions().FindTuning(FName(TEXT("madfall:survival"))))
		{
			TArray<FName> Unknown;
			ApplyTuning(*Definition, Tuning, Unknown);
			for (const FName& Key : Unknown)
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("%s: survival tuning key '%s' is not recognised and was ignored."),
					*Definition->SourcePath, *Key.ToString());
			}
		}
		return Tuning;
	}
}

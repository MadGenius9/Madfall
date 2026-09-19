// Copyright MadFall. All Rights Reserved.

#include "MadQuests.h"

#include "MadPrefabRegistry.h"
#include "MadVoxelWorldSubsystem.h"

#include "Algo/AllOf.h"
#include "MadLocalization.h"

namespace
{
	bool IsStateObjective(EMadQuestObjectiveType Type)
	{
		return Type == EMadQuestObjectiveType::Have || Type == EMadQuestObjectiveType::ReachDay;
	}

	/** Sizes a progress record to its definition: objectives added or removed by a mod since the save. */
	void FitCounts(FMadQuestProgress& Progress, const FMadQuestDefinition& Quest)
	{
		Progress.Counts.SetNumZeroed(Quest.Objectives.Num());
	}
}

TArray<FName> FMadQuestLog::Refresh(const FMadGameplayDefinitions& Definitions)
{
	TArray<FName> Started;
	for (const FMadQuestDefinition& Quest : Definitions.GetQuests())
	{
		// Being complete stops a quest coming back, unless it is the kind a
		// trader keeps having more of. Its place in Completed is left alone, so
		// anything that requires it still unlocks exactly once.
		if ((Completed.Contains(Quest.Id) && !Quest.bRepeatable) || FindActive(Quest.Id) != nullptr)
		{
			continue;
		}
		const bool bUnlocked = Algo::AllOf(Quest.Requires, [this](const FName& Required) { return Completed.Contains(Required); });
		if (bUnlocked)
		{
			FMadQuestProgress& Progress = Active.AddDefaulted_GetRef();
			Progress.Quest = Quest.Id;
			FitCounts(Progress, Quest);
			Started.Add(Quest.Id);
		}
	}
	return Started;
}

TArray<FName> FMadQuestLog::Notify(EMadQuestObjectiveType Type, FName Id, const TArray<FName>& Tags, int32 Amount,
	const FMadGameplayDefinitions& Definitions)
{
	if (Amount <= 0 || IsStateObjective(Type))
	{
		return {};
	}
	bool bChanged = false;
	for (FMadQuestProgress& Progress : Active)
	{
		const FMadQuestDefinition* Quest = Definitions.FindQuest(Progress.Quest);
		if (Quest == nullptr)
		{
			continue;
		}
		FitCounts(Progress, *Quest);
		for (int32 Index = 0; Index < Quest->Objectives.Num(); ++Index)
		{
			const FMadQuestObjective& Objective = Quest->Objectives[Index];
			if (Objective.Type == Type && Progress.Counts[Index] < Objective.Count && Objective.Matches(Id, Tags))
			{
				Progress.Counts[Index] = FMath::Min(Objective.Count, Progress.Counts[Index] + Amount);
				bChanged = true;
			}
		}
	}
	return bChanged ? CollectCompleted(Definitions) : TArray<FName>();
}

TArray<FName> FMadQuestLog::Evaluate(TFunctionRef<int32(const FMadQuestObjective&)> Current, const FMadGameplayDefinitions& Definitions)
{
	for (FMadQuestProgress& Progress : Active)
	{
		const FMadQuestDefinition* Quest = Definitions.FindQuest(Progress.Quest);
		if (Quest == nullptr)
		{
			continue;
		}
		FitCounts(Progress, *Quest);
		for (int32 Index = 0; Index < Quest->Objectives.Num(); ++Index)
		{
			const FMadQuestObjective& Objective = Quest->Objectives[Index];
			if (IsStateObjective(Objective.Type))
			{
				Progress.Counts[Index] = FMath::Clamp(Current(Objective), 0, Objective.Count);
			}
		}
	}
	return CollectCompleted(Definitions);
}

bool FMadQuestLog::AreObjectivesMet(const FMadQuestProgress& Progress, const FMadQuestDefinition& Quest)
{
	for (int32 Objective = 0; Objective < Quest.Objectives.Num(); ++Objective)
	{
		if (!Progress.Counts.IsValidIndex(Objective) || Progress.Counts[Objective] < Quest.Objectives[Objective].Count)
		{
			return false;
		}
	}
	return true;
}

TArray<FName> FMadQuestLog::CollectCompleted(const FMadGameplayDefinitions& Definitions)
{
	TArray<FName> Done;
	for (int32 Index = Active.Num() - 1; Index >= 0; --Index)
	{
		const FMadQuestDefinition* Quest = Definitions.FindQuest(Active[Index].Quest);
		if (Quest == nullptr || Quest->bHandIn)
		{
			// A hand-in quest that is finished stays in the journal, counts
			// full, until the survivor reaches someone to report to.
			continue;
		}
		if (AreObjectivesMet(Active[Index], *Quest))
		{
			Completed.Add(Quest->Id);
			Done.Insert(Quest->Id, 0);
			Active.RemoveAt(Index);
		}
	}
	return Done;
}

TArray<FName> FMadQuestLog::HandIn(const FMadGameplayDefinitions& Definitions)
{
	TArray<FName> Paid;
	for (int32 Index = Active.Num() - 1; Index >= 0; --Index)
	{
		const FMadQuestDefinition* Quest = Definitions.FindQuest(Active[Index].Quest);
		if (Quest == nullptr || !Quest->bHandIn || !AreObjectivesMet(Active[Index], *Quest))
		{
			continue;
		}
		Completed.Add(Quest->Id);
		Paid.Insert(Quest->Id, 0);
		Active.RemoveAt(Index);
	}
	return Paid;
}

bool FMadQuestLog::IsWaitingToHandIn(FName Quest, const FMadGameplayDefinitions& Definitions) const
{
	const FMadQuestProgress* Progress = FindActive(Quest);
	const FMadQuestDefinition* Definition = Progress ? Definitions.FindQuest(Quest) : nullptr;
	return Definition != nullptr && Definition->bHandIn && AreObjectivesMet(*Progress, *Definition);
}

int32 FMadQuestLog::NumWaitingToHandIn(const FMadGameplayDefinitions& Definitions) const
{
	int32 Waiting = 0;
	for (const FMadQuestProgress& Progress : Active)
	{
		const FMadQuestDefinition* Quest = Definitions.FindQuest(Progress.Quest);
		Waiting += (Quest != nullptr && Quest->bHandIn && AreObjectivesMet(Progress, *Quest)) ? 1 : 0;
	}
	return Waiting;
}

bool FMadQuestLog::ForceComplete(FName Quest, const FMadGameplayDefinitions& Definitions)
{
	if (Definitions.FindQuest(Quest) == nullptr || Completed.Contains(Quest))
	{
		return false;
	}
	Active.RemoveAll([Quest](const FMadQuestProgress& Progress) { return Progress.Quest == Quest; });
	Completed.Add(Quest);
	return true;
}

const FMadQuestProgress* FMadQuestLog::FindActive(FName Quest) const
{
	return Active.FindByPredicate([Quest](const FMadQuestProgress& Progress) { return Progress.Quest == Quest; });
}

void FMadQuestLog::GetJournal(const FMadGameplayDefinitions& Definitions, TArray<TPair<const FMadQuestDefinition*, const FMadQuestProgress*>>& Out) const
{
	Out.Reset();
	for (const FMadQuestProgress& Progress : Active)
	{
		if (const FMadQuestDefinition* Quest = Definitions.FindQuest(Progress.Quest))
		{
			Out.Add({ Quest, &Progress });
		}
	}
	Out.Sort([](const TPair<const FMadQuestDefinition*, const FMadQuestProgress*>& A, const TPair<const FMadQuestDefinition*, const FMadQuestProgress*>& B)
	{
		return A.Key->Order != B.Key->Order ? A.Key->Order < B.Key->Order : A.Key->Id.LexicalLess(B.Key->Id);
	});
}

void FMadQuestLog::Export(TArray<FName>& OutCompleted, TArray<FMadQuestProgress>& OutActive) const
{
	OutCompleted = Completed.Array();
	OutCompleted.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	OutActive = Active;
}

void FMadQuestLog::Import(const TArray<FName>& InCompleted, const TArray<FMadQuestProgress>& InActive, const FMadGameplayDefinitions& Definitions)
{
	Completed = TSet<FName>(InCompleted);
	Active.Reset();
	for (const FMadQuestProgress& Saved : InActive)
	{
		const FMadQuestDefinition* Quest = Definitions.FindQuest(Saved.Quest);

		// A repeatable job on its second run is active AND complete at the same
		// time - complete because it was done once, active because it was taken
		// again - so "complete" cannot be the reason to drop it here, or saving
		// mid-repeat would quietly throw the repeat away.
		const bool bCompletedForGood = Completed.Contains(Saved.Quest) && (Quest == nullptr || !Quest->bRepeatable);
		if (bCompletedForGood || FindActive(Saved.Quest) != nullptr)
		{
			continue;
		}
		FMadQuestProgress& Progress = Active.Add_GetRef(Saved);
		if (Quest != nullptr)
		{
			FitCounts(Progress, *Quest);
		}
	}
}

FString MadFall::Quests::DescribeObjective(const FMadQuestObjective& Objective, const FMadGameplayDefinitions& Definitions)
{
	if (!Objective.Text.IsEmpty())
	{
		return MadFall::Localize(Objective.Text);
	}

	const FString Item = Objective.Target.IsNone() ? Objective.Tag.ToString() : Definitions.GetItemName(Objective.Target);
	const FString Block = Objective.Target.IsNone() ? Objective.Tag.ToString() : FMadGameplayDefinitions::GetBlockName(Objective.Target);
	const FString Thing = Objective.Target.IsNone() ? Objective.Tag.ToString() : Objective.Target.ToString();
	switch (Objective.Type)
	{
	case EMadQuestObjectiveType::Craft:      return FString::Printf(TEXT("Craft %s"), *Item);
	case EMadQuestObjectiveType::Have:       return FString::Printf(TEXT("Carry %s"), *Item);
	case EMadQuestObjectiveType::Place:      return FString::Printf(TEXT("Place %s"), *Block);
	case EMadQuestObjectiveType::Break:      return FString::Printf(TEXT("Break %s"), *Block);
	case EMadQuestObjectiveType::KillZombie: return Objective.Target.IsNone() && Objective.Tag.IsNone() ? FString(TEXT("Kill zombies")) : FString::Printf(TEXT("Kill %s"), *Thing);
	case EMadQuestObjectiveType::KillAnimal: return Objective.Target.IsNone() && Objective.Tag.IsNone() ? FString(TEXT("Hunt animals")) : FString::Printf(TEXT("Hunt %s"), *Thing);
	case EMadQuestObjectiveType::Wear:       return FString::Printf(TEXT("Wear %s"), *Item);
	case EMadQuestObjectiveType::SetSpawn:   return FString(TEXT("Sleep in a bed"));
	case EMadQuestObjectiveType::ReachDay:   return FString::Printf(TEXT("Survive to day %d"), Objective.Count);
	case EMadQuestObjectiveType::Trade:      return FString(TEXT("Trade with a trader"));
	case EMadQuestObjectiveType::ClearPoi:
	{
		// The prefab's display name if it names one, otherwise the tag, so
		// "Clear a Ruined Bunker" and "Clear a military site" both read.
		FString Place = Objective.Tag.ToString();
		if (!Objective.Target.IsNone())
		{
			const FMadPrefabRegistry& Prefabs = UMadVoxelWorldSubsystem::GetPrefabRegistry();
			const int32 Index = Prefabs.FindIndex(Objective.Target);
			Place = Index != INDEX_NONE ? MadFall::Localize(Prefabs.Get(Index).DisplayName) : Objective.Target.ToString();
		}
		return FString::Printf(TEXT("Clear %s"), *Place);
	}
	default:                                 return Thing;
	}
}

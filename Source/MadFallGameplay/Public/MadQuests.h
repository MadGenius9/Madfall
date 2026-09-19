// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadGameplayDefinitions.h"
#include "MadGameplaySave.h"

/**
 * The survivor's journal: which quests are done and how far along the active
 * ones are.
 *
 * Plain C++ with no world, so the rules are tested directly. The player feeds
 * it events as they happen (crafted, placed, broke, killed, wore, slept) and,
 * once a second, state (items held, the day). Event objectives accumulate;
 * state objectives are re-read, so dropping the planks a "have 12 planks"
 * objective counted undoes it until the quest completes.
 *
 * A quest starts as soon as every quest it requires is complete. Most complete
 * the moment their last objective is met; one marked `hand_in` instead waits,
 * with every objective met, until the survivor reaches a trader and HandIn() is
 * called. "Ready" is not stored anywhere - it is simply an active quest whose
 * counts are all full - so a save written before hand-ins existed still loads.
 */
class MADFALLGAMEPLAY_API FMadQuestLog
{
public:
	/** Starts every quest whose requirements are complete. Returns the ids started. */
	TArray<FName> Refresh(const FMadGameplayDefinitions& Definitions);

	/**
	 * Counts an event toward matching objectives of every active quest.
	 * Returns the quests it completed; they are already marked complete.
	 */
	TArray<FName> Notify(EMadQuestObjectiveType Type, FName Id, const TArray<FName>& Tags, int32 Amount,
		const FMadGameplayDefinitions& Definitions);

	/** Re-reads state objectives (have, reach_day) through Current. Returns the quests completed. */
	TArray<FName> Evaluate(TFunctionRef<int32(const FMadQuestObjective&)> Current, const FMadGameplayDefinitions& Definitions);

	/** Completes a quest outright (the console, and tests). */
	bool ForceComplete(FName Quest, const FMadGameplayDefinitions& Definitions);

	/**
	 * Completes every active hand-in quest whose objectives are all met, which
	 * is what reaching a trader does. Returns the quests it completed.
	 */
	TArray<FName> HandIn(const FMadGameplayDefinitions& Definitions);

	/** True if this quest is waiting to be handed in: done, but not paid. */
	bool IsWaitingToHandIn(FName Quest, const FMadGameplayDefinitions& Definitions) const;

	/** How many quests are done but unpaid, for the journal's "see a trader" line. */
	int32 NumWaitingToHandIn(const FMadGameplayDefinitions& Definitions) const;

	bool IsComplete(FName Quest) const { return Completed.Contains(Quest); }
	bool IsActive(FName Quest) const { return FindActive(Quest) != nullptr; }
	const FMadQuestProgress* FindActive(FName Quest) const;
	const TArray<FMadQuestProgress>& GetActive() const { return Active; }
	int32 NumCompleted() const { return Completed.Num(); }

	/** Active quests with definitions, in journal order (Order, then id). */
	void GetJournal(const FMadGameplayDefinitions& Definitions, TArray<TPair<const FMadQuestDefinition*, const FMadQuestProgress*>>& Out) const;

	void Export(TArray<FName>& OutCompleted, TArray<FMadQuestProgress>& OutActive) const;

	/** Unknown quest ids (a removed mod's) are kept, and simply never progress. */
	void Import(const TArray<FName>& InCompleted, const TArray<FMadQuestProgress>& InActive, const FMadGameplayDefinitions& Definitions);

	void Reset() { Completed.Reset(); Active.Reset(); }

private:
	/**
	 * Moves every active quest whose objectives are all met to Completed,
	 * except the hand-in ones, which stay active until HandIn() is called.
	 */
	TArray<FName> CollectCompleted(const FMadGameplayDefinitions& Definitions);

	/** True if every objective of an active quest is met. */
	static bool AreObjectivesMet(const FMadQuestProgress& Progress, const FMadQuestDefinition& Quest);

	TSet<FName> Completed;
	TArray<FMadQuestProgress> Active;
};

namespace MadFall::Quests
{
	/** The journal line for an objective: its "text", or one built from its type and target. */
	MADFALLGAMEPLAY_API FString DescribeObjective(const FMadQuestObjective& Objective, const FMadGameplayDefinitions& Definitions);
}

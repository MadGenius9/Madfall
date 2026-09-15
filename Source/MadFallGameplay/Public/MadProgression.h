// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadGameplayDefinitions.h"
#include "MadSurvivalModel.h"

/** Why a perk could or could not be bought. */
enum class EMadPerkResult : uint8
{
	Ok,
	UnknownPerk,
	MaxRank,
	LevelTooLow,
	MissingPrerequisite,
	NoPoints
};

/**
 * Stats perks can modify. Every modifier is a multiplier; a perk that "adds 20%
 * health" is `"max_health": 1.2`. Keeping one operation means ranks and
 * several perks combine by multiplication, which a player can predict.
 *
 *   mining_damage   damage dealt to blocks
 *   melee_damage    damage dealt to zombies
 *   stamina_cost    stamina spent per swing
 *   craft_time      recipe craft seconds
 *   max_health      maximum health
 *   max_stamina     maximum stamina
 *   food_drain      food lost over time
 *   water_drain     water lost over time
 */
namespace MadFall::Perks
{
	MADFALLGAMEPLAY_API const TSet<FName>& GetKnownStats();

	MADFALLGAMEPLAY_API const TCHAR* ToString(EMadPerkResult Result);

	/** Points available: one per level after the first, minus ranks already bought. */
	MADFALLGAMEPLAY_API int32 GetUnspentPoints(int32 Level, const TMap<FName, int32>& Ranks);

	/**
	 * Product of the owned rank's multiplier for Stat across every perk. Each
	 * perk contributes its highest owned rank only (ranks are cumulative
	 * designs, not additive stacks). Unknown perks contribute nothing.
	 */
	MADFALLGAMEPLAY_API float GetMultiplier(const TMap<FName, int32>& Ranks, const FMadGameplayDefinitions& Definitions, FName Stat);

	/** Buys the next rank of a perk if allowed. Changes Ranks only on Ok. */
	MADFALLGAMEPLAY_API EMadPerkResult TryBuy(TMap<FName, int32>& Ranks, FName Perk, int32 Level, const FMadGameplayDefinitions& Definitions);

	/** What TryBuy would return now, without buying: the skills screen greys out a button with this. */
	MADFALLGAMEPLAY_API EMadPerkResult CanBuy(const TMap<FName, int32>& Ranks, FName Perk, int32 Level, const FMadGameplayDefinitions& Definitions);

	/**
	 * The first requirement of Rank the survivor does not meet, in id order so
	 * the screen names the same one every frame. False when all are met.
	 * Prerequisites gate buying only: ranks already owned keep working if a
	 * patched perk adds a requirement later, since taking a bonus away from a
	 * saved character reads as a bug.
	 */
	MADFALLGAMEPLAY_API bool FindMissingPrerequisite(const TMap<FName, int32>& Ranks, const FMadPerkRank& Rank, FName& OutPerk, int32& OutRank);

	/**
	 * Perk indices in skills-screen order: grouped by first tag (the attribute,
	 * "perk.strength"), then by prerequisite depth so a perk sits below what it
	 * needs, then by id. Untagged perks come last.
	 */
	MADFALLGAMEPLAY_API TArray<int32> GetTreeOrder(const FMadGameplayDefinitions& Definitions);

	/**
	 * How deep a perk sits in its attribute's tree: 0 with no prerequisites in
	 * the same group (first tag), else one more than the deepest it requires
	 * there. A requirement in another attribute does not indent - the perk would
	 * hang off nothing on screen - and the greyed button names it instead.
	 */
	MADFALLGAMEPLAY_API int32 GetTreeDepth(const FMadGameplayDefinitions& Definitions, FName Perk);

	/**
	 * A rank's effects for a player to read, e.g. "max health +20%, food drain -25%".
	 * Multipliers are shown as percentages because "x1.2" reads as maths, not as a bonus.
	 */
	MADFALLGAMEPLAY_API FString DescribeRank(const FMadPerkRank& Rank);
}

namespace MadFall::Survival
{
	/**
	 * Overwrites tuning fields named in a `madfall.tuning/1` definition. Unknown
	 * keys are returned so a typo in a balance mod is reported, not ignored.
	 */
	MADFALLGAMEPLAY_API void ApplyTuning(const FMadTuningDefinition& Definition, FMadSurvivalTuning& InOutTuning, TArray<FName>& OutUnknownKeys);

	/** The shipped survival balance with any `madfall:survival` tuning (and its patches) applied. */
	MADFALLGAMEPLAY_API FMadSurvivalTuning GetConfiguredTuning();
}

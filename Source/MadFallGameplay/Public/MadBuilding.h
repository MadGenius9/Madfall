// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadGameplayDefinitions.h"

/**
 * Working on a block that is already standing: patching the damage out of it,
 * and upgrading it to the next tier where it stands.
 *
 * WHY THIS EXISTS: the game shipped the whole tier ladder - wood frame,
 * reinforced wood, concrete frame, rebar concrete - as four separate blocks you
 * craft and place. So a survivor who wanted a stronger wall knocked their own
 * wall down and built another one, losing the first, and a wall chewed on by a
 * horde stayed chewed for ever. Upgrading and repairing in place is the verb
 * the whole base-building loop is built around, and it was the one verb
 * missing.
 *
 * WHY THE LADDER IS NOT NEW DATA: the recipes already say it. A recipe whose
 * output is a placeable block and whose ingredients include exactly one other
 * placeable block, once, IS an upgrade - "this block, plus these materials,
 * becomes that block". Deriving it means the ladder cannot drift out of step
 * with crafting, costs stay balanced in one place, and a mod that adds a tier
 * recipe gets the upgrade for nothing.
 *
 * The station is deliberately ignored. A recipe may want a workbench; a
 * survivor upgrading a wall is standing at the wall, and nobody carries a
 * workbench to the fight. The level requirement is kept, because that is a
 * progression gate rather than a place.
 */
enum class EMadBlockWork : uint8
{
	/** Nothing to do: undamaged, and either at the top of its ladder or not on one. */
	None,
	/** Damaged, and patched with more of the same block. */
	Repair,
	/** Whole, and there is a better block to become. */
	Upgrade
};

/** What working on a block would do, what it costs and why it cannot. */
struct MADFALLGAMEPLAY_API FMadBlockWorkPlan
{
	EMadBlockWork Action = EMadBlockWork::None;

	/** The block it becomes. Only set for Upgrade. */
	FName ToBlock;

	/** What it costs, all of which must be in the backpack. */
	TArray<FMadItemAmount> Cost;

	/** The survivor's level must be at least this. */
	int32 RequiredLevel = 0;

	/** Set when the action is possible in principle but something is missing. */
	bool bMissingMaterials = false;
	bool bUnderLevelled = false;

	/** True if the work can actually be done now. */
	bool CanDo() const { return Action != EMadBlockWork::None && !bMissingMaterials && !bUnderLevelled; }
};

namespace MadFall::Building
{
	/**
	 * The recipe that upgrades a block, or null. See the header comment: a
	 * recipe qualifies when its output is a placeable block and exactly one of
	 * its ingredients is a placeable block taken one at a time.
	 */
	MADFALLGAMEPLAY_API const FMadRecipeDefinition* FindUpgradeRecipe(
		const FMadGameplayDefinitions& Definitions, FName FromBlock);

	/** The item that places a block, or None. */
	MADFALLGAMEPLAY_API FName FindBlockItem(const FMadGameplayDefinitions& Definitions, FName BlockId);

	/**
	 * What hitting this block with the repair key would do. Damage is the
	 * voxel's own 0-255 wear. Held is asked how many of an item the survivor
	 * has, so the rules stay free of the inventory.
	 */
	MADFALLGAMEPLAY_API FMadBlockWorkPlan PlanBlockWork(
		const FMadGameplayDefinitions& Definitions, FName BlockId, uint8 Damage, int32 Level,
		TFunctionRef<int32(FName)> Held);
}

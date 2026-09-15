// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadInventory.h"

struct FMadBlockDefinitionData;

/** What swinging the held item at a block does. */
struct MADFALLGAMEPLAY_API FMadToolHit
{
	float Amount = 0.0f;
	FName DamageType;

	/** The block's drops are collected when it breaks. */
	bool bHarvests = false;

	/** The tool is wrong for the block (or too low a tier): shown to the player as "wrong tool". */
	bool bPenalised = false;
};

/**
 * Tool-versus-block rules.
 *
 *   Block harvest tags empty                -> any tool: full damage, harvests
 *   Tool shares a harvest tag, tier >= block -> full damage, harvests
 *   Tool shares a tag, tier too low          -> half damage, no drops
 *   Wrong tool (or bare hands on a tagged block) -> quarter damage, no drops
 *
 * The damage type used is whichever of the tool's types does the most damage
 * after the block's resistances, which is what a player swinging it would
 * expect. Bare hands are 8 blunt.
 */
namespace MadFall::Harvest
{
	MADFALLGAMEPLAY_API FMadToolHit ComputeHit(const FMadItemStack* Held, const FMadGameplayDefinitions& Definitions,
		const FMadBlockDefinitionData& Block);

	/**
	 * Product of every installed mod's multiplier for a stat ("damage",
	 * "durability", "use_seconds", "stamina_cost", "range"), for mods that
	 * apply to the item's tags. 1.0 with no mods.
	 */
	MADFALLGAMEPLAY_API float GetModMultiplier(const FMadItemStack& Stack, const FMadGameplayDefinitions& Definitions, FName Stat);

	/**
	 * Drops for a broken block: its drop table if one is registered, otherwise
	 * one of its own block item. Nothing when not harvested, or when the block
	 * requires a tool tag the held item lacks.
	 */
	MADFALLGAMEPLAY_API void RollDrops(const FMadBlockDefinitionData& Block, const FMadItemStack* Held, bool bHarvests,
		const FMadGameplayDefinitions& Definitions, FRandomStream& Random, TArray<FMadItemStack>& OutDrops);

	/**
	 * Uses one point of durability. Returns true if the item broke (and was
	 * cleared). Items without durability never break. Durability mods scale
	 * the chance a use consumes a point, so a 1.5x mod lasts 1.5x as long on
	 * average without changing the stored maximum.
	 */
	MADFALLGAMEPLAY_API bool ConsumeDurability(FMadItemStack& Stack, const FMadGameplayDefinitions& Definitions, FRandomStream& Random);
}

enum class EMadItemActionResult : uint8
{
	Ok,
	NotATool,
	NothingToDo,
	MissingMaterials,
	NoFreeSlot,
	IncompatibleMod
};

namespace MadFall::Items
{
	MADFALLGAMEPLAY_API const TCHAR* ToString(EMadItemActionResult Result);

	/**
	 * Repairs the tool in a slot once: consumes its repair_with cost from the
	 * inventory and restores repair_fraction of its maximum durability, capped at
	 * the maximum. Atomic - missing materials change nothing.
	 */
	MADFALLGAMEPLAY_API EMadItemActionResult Repair(FMadInventory& Inventory, int32 ToolSlot, const FMadGameplayDefinitions& Definitions);

	/**
	 * Installs one of ModItem from the inventory into the tool in ToolSlot. The
	 * mod must apply to the tool's tags and the tool needs a free mod slot.
	 */
	MADFALLGAMEPLAY_API EMadItemActionResult InstallMod(FMadInventory& Inventory, int32 ToolSlot, FName ModItem, const FMadGameplayDefinitions& Definitions);

	/**
	 * Takes one installed mod (ModIndex, or the last installed with INDEX_NONE)
	 * off the tool in ToolSlot and puts it back in the same inventory, whole.
	 * Atomic: with no room for the mod nothing changes (NoFreeSlot). A mod from
	 * an uninstalled mod-pack comes back as its id, like any unknown item.
	 * Removal is only possible in the inventory screen, never mid-swing, which is
	 * what keeps moving mods between tools a decision rather than a per-fight swap.
	 */
	MADFALLGAMEPLAY_API EMadItemActionResult RemoveMod(FMadInventory& Inventory, int32 ToolSlot, int32 ModIndex, const FMadGameplayDefinitions& Definitions);

	/**
	 * True when putting Held down on Target should install Held into Target
	 * instead of swapping the two: an item mod dropped onto a tool or weapon.
	 * A mod that does not fit is then refused with a reason rather than
	 * swapped, so a player never loses track of which slot their mod went to.
	 */
	MADFALLGAMEPLAY_API bool IsModInstallDrop(const FMadItemStack& Held, const FMadItemStack& Target, const FMadGameplayDefinitions& Definitions);

	/**
	 * An item's tooltip: its name first, then one line per thing it does
	 * (damage, durability, effects, protection, mod bonuses, installed mods).
	 * An item from a mod that is not installed says so instead.
	 */
	MADFALLGAMEPLAY_API void DescribeStack(const FMadItemStack& Stack, const FMadGameplayDefinitions& Definitions, TArray<FString>& OutLines);
}

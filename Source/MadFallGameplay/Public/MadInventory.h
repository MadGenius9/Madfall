// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadGameplayDefinitions.h"

class FMadGameplayDefinitions;
struct FRandomStream;

/**
 * One inventory slot's contents.
 *
 * Plain data, no UObject: an inventory is saved, replicated (Phase 6), shown in
 * UI and rolled from loot tables, and every one of those is simpler with a
 * value type. Items are referenced by namespaced id, never by pointer, for the
 * same reason blocks are - a save must survive a mod being removed.
 */
struct MADFALLGAMEPLAY_API FMadItemStack
{
	FName Item;
	int32 Count = 0;

	/** Remaining uses for tools and weapons. -1 for items without durability. */
	int32 Durability = -1;

	/** Installed item mods, by item id. */
	TArray<FName> Mods;

	bool IsEmpty() const { return Item.IsNone() || Count <= 0; }

	/** Two stacks merge only when nothing per-item would be lost by merging. */
	bool CanMergeWith(const FMadItemStack& Other) const
	{
		return Item == Other.Item && Durability < 0 && Other.Durability < 0 && Mods.Num() == 0 && Other.Mods.Num() == 0;
	}

	bool operator==(const FMadItemStack& Other) const
	{
		return Item == Other.Item && Count == Other.Count && Durability == Other.Durability && Mods == Other.Mods;
	}

	/** A fresh stack of an item: full durability for equipment. */
	static FMadItemStack Make(const FMadItemDefinition& Definition, int32 Count);
};

/**
 * A fixed number of slots.
 *
 * Every mutating operation is all-or-nothing where the name says so (Remove,
 * RemoveAll) and reports the remainder where partial success is meaningful
 * (Add). Crafting and looting build on those guarantees instead of checking
 * and then acting in two steps that could disagree.
 */
class MADFALLGAMEPLAY_API FMadInventory
{
public:
	explicit FMadInventory(int32 NumSlots = 0) { Slots.SetNum(NumSlots); }

	int32 NumSlots() const { return Slots.Num(); }
	bool IsValidSlot(int32 Index) const { return Slots.IsValidIndex(Index); }
	const FMadItemStack& GetSlot(int32 Index) const { return Slots[Index]; }
	const TArray<FMadItemStack>& GetSlots() const { return Slots; }

	/** Direct write, for UI moves and loading. Unknown ids are kept verbatim. */
	void SetSlot(int32 Index, const FMadItemStack& Stack) { Slots[Index] = Stack; }

	/**
	 * Adds a stack, topping up compatible stacks first (in slot order), then
	 * filling empty slots. Returns how many did NOT fit.
	 */
	int32 Add(const FMadItemStack& Stack, const FMadGameplayDefinitions& Definitions);

	/** Total count of an item across all slots. */
	int32 CountItem(FName Item) const;

	/** True if every amount is present. Duplicate ids in the list add up. */
	bool HasAll(const TArray<FMadItemAmount>& Amounts) const;

	/** Removes Count of an item, taking from the LAST matching slots first. All or nothing. */
	bool Remove(FName Item, int32 Count);

	/** Removes every amount. All or nothing. */
	bool RemoveAll(const TArray<FMadItemAmount>& Amounts);

	/** True if all stacks would fit, without changing anything. */
	bool CanAddAll(const TArray<FMadItemStack>& Stacks, const FMadGameplayDefinitions& Definitions) const;

	/** Takes up to Count from one slot. Returns what was taken. */
	FMadItemStack TakeFromSlot(int32 Index, int32 Count);

	bool IsEmpty() const;

private:
	TArray<FMadItemStack> Slots;
};

/** What a slot move did. */
enum class EMadSlotMove : uint8
{
	/** Into an empty slot (all or part of the stack). */
	Moved,
	/** Topped up a matching stack; any remainder stayed behind. */
	Merged,
	/** Two different stacks traded places. */
	Swapped,
	/** Nothing to do: empty source, same slot, or a full matching stack. */
	Nothing,
	InvalidSlot
};

/**
 * The moves an inventory screen makes. Both sides may be the same inventory
 * (rearranging the backpack) or different ones (backpack and crate); every
 * move conserves items exactly, which the tests check by counting.
 */
namespace MadFall::InventoryOps
{
	MADFALLGAMEPLAY_API const TCHAR* ToString(EMadSlotMove Result);

	/**
	 * Drag-and-drop. Moves up to Count (<= 0 means the whole stack) from
	 * From[FromIndex] to To[ToIndex]:
	 *   - empty target: the amount moves (capped at the item's max stack);
	 *   - mergeable target: tops it up to the max stack;
	 *   - different item: the stacks swap, but only when moving the whole
	 *     source stack - a partial drag onto a different item does nothing
	 *     rather than silently becoming a full swap.
	 */
	MADFALLGAMEPLAY_API EMadSlotMove MoveSlot(FMadInventory& From, int32 FromIndex, FMadInventory& To, int32 ToIndex,
		int32 Count, const FMadGameplayDefinitions& Definitions);

	/**
	 * Shift-click: as much of the stack as fits goes into slots ToFirst..ToLast
	 * of To (topping up matching stacks first); the rest stays put. The range
	 * is what lets one inventory quick-move between its hotbar and backpack.
	 * Returns how many moved.
	 */
	MADFALLGAMEPLAY_API int32 QuickMove(FMadInventory& From, int32 FromIndex, FMadInventory& To, int32 ToFirst, int32 ToLast,
		const FMadGameplayDefinitions& Definitions);

	/**
	 * Sorts slots First..Last: stacks of the same plain item merge into as few
	 * full stacks as the item allows, and everything is ordered tools, weapons,
	 * mods, food and medicine, blocks, resources - then by name, then id, with
	 * the most worn tool of a kind last - packed from First with the empty slots
	 * at the end. Tools and modded items never merge. Items nobody defines
	 * (their mod was removed) go last, untouched. Every item is kept: if merging
	 * would need more slots than the range has (a stack larger than a patched
	 * max_stack), stacks keep their sizes and are only reordered. Returns true
	 * if any slot changed.
	 */
	MADFALLGAMEPLAY_API bool SortRange(FMadInventory& Inventory, int32 First, int32 Last, const FMadGameplayDefinitions& Definitions);
}

/** The crafting screen's filter tabs. Derived from what a recipe makes, so a mod's recipes sort themselves. */
enum class EMadRecipeCategory : uint8
{
	All,
	Tools,
	Building,
	Food,
	Clothing,
	Materials,
	Num
};

struct FMadRecipeDefinition;

/** One line of the crafting screen. */
struct MADFALLGAMEPLAY_API FMadRecipeRow
{
	const FMadRecipeDefinition* Recipe = nullptr;

	/** Batches the backpack holds ingredients for. */
	int32 Craftable = 0;
	bool bStation = true;
	bool bLevel = true;

	bool CanCraftNow() const { return Recipe != nullptr && Craftable > 0 && bStation && bLevel; }
};

/** Why a craft did or did not happen. */
enum class EMadCraftResult : uint8
{
	Ok,
	UnknownItem,
	MissingIngredients,
	NoSpace,
	WrongStation,
	LevelTooLow
};

namespace MadFall::Crafting
{
	MADFALLGAMEPLAY_API const TCHAR* ToString(EMadCraftResult Result);

	/** How many times the recipe could be crafted from this inventory right now. */
	MADFALLGAMEPLAY_API int32 MaxCraftable(const FMadInventory& Inventory, const FMadRecipeDefinition& Recipe);

	/**
	 * Crafts Times batches: removes ingredients and adds output atomically. If
	 * the output would not fit, nothing changes and NoSpace is returned - a
	 * craft never eats ingredients and drops the result on the floor.
	 *
	 * Station: the station the player is using (NAME_None for the backpack).
	 * A recipe with no station can be crafted anywhere.
	 */
	MADFALLGAMEPLAY_API EMadCraftResult Craft(FMadInventory& Inventory, const FMadRecipeDefinition& Recipe,
		const FMadGameplayDefinitions& Definitions, FName Station, int32 PlayerLevel, int32 Times = 1);

	/**
	 * The first half of a timed craft: checks station, level and ingredients and
	 * removes the ingredients, without adding the output. The caller delivers the
	 * output later (and decides what happens if it no longer fits).
	 */
	MADFALLGAMEPLAY_API EMadCraftResult TakeIngredients(FMadInventory& Inventory, const FMadRecipeDefinition& Recipe,
		const FMadGameplayDefinitions& Definitions, FName Station, int32 PlayerLevel, int32 Times = 1);

	MADFALLGAMEPLAY_API const TCHAR* GetCategoryName(EMadRecipeCategory Category);

	/**
	 * Which tab a recipe is under, from its output: tools, weapons, item mods and
	 * ammunition are Tools; anything that places a block is Building; consumables
	 * Food; clothing Clothing; the rest Materials.
	 */
	MADFALLGAMEPLAY_API EMadRecipeCategory CategoryOf(const FMadRecipeDefinition& Recipe, const FMadGameplayDefinitions& Definitions);

	/**
	 * The crafting screen's list: recipes in Category (every one for All),
	 * optionally only those craftable now, with what can be made right now
	 * first and then by name - so the top of any tab is what a player can act on.
	 */
	MADFALLGAMEPLAY_API void ListRecipes(const FMadGameplayDefinitions& Definitions, const FMadInventory& Inventory, int32 PlayerLevel,
		TFunctionRef<bool(FName Station)> HasStation, EMadRecipeCategory Category, bool bCraftableOnly, TArray<FMadRecipeRow>& Out);
}

/** What loot quality depends on. */
struct MADFALLGAMEPLAY_API FMadLootContext
{
	/** POI tier, 1-5. 1 for loose world drops. */
	int32 Tier = 1;

	/** Days survived plus player level. */
	int32 GameStage = 0;
};

namespace MadFall::Loot
{
	/**
	 * Rolls a loot table. Deterministic for a given stream state, so a
	 * container's contents can be derived from a seed and never stored until
	 * it is opened.
	 */
	MADFALLGAMEPLAY_API void Roll(const FMadLootTableDefinition& Table, const FMadGameplayDefinitions& Definitions,
		const FMadLootContext& Context, FRandomStream& Random, TArray<FMadItemStack>& OutStacks);
}

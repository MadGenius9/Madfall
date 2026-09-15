// Copyright MadFall. All Rights Reserved.

#include "MadInventory.h"

#include "Algo/StableSort.h"

#include "Math/RandomStream.h"

// ===========================================================================
// Stacks
// ===========================================================================

FMadItemStack FMadItemStack::Make(const FMadItemDefinition& Definition, int32 Count)
{
	FMadItemStack Stack;
	Stack.Item = Definition.Id;
	Stack.Count = Count;
	Stack.Durability = (Definition.bHasTool && Definition.Tool.Durability > 0) ? Definition.Tool.Durability : -1;
	return Stack;
}

// ===========================================================================
// Inventory
// ===========================================================================

namespace
{
	int32 MaxStackFor(FName Item, const FMadGameplayDefinitions& Definitions)
	{
		// An item nobody defines (its mod was removed) still occupies space but
		// never accepts more: it is a placeholder, like an unresolved block.
		const FMadItemDefinition* Definition = Definitions.FindItem(Item);
		return Definition ? Definition->MaxStack : 1;
	}
}

int32 FMadInventory::Add(const FMadItemStack& Stack, const FMadGameplayDefinitions& Definitions)
{
	if (Stack.IsEmpty())
	{
		return 0;
	}

	const int32 MaxStack = MaxStackFor(Stack.Item, Definitions);
	int32 Remaining = Stack.Count;

	for (FMadItemStack& Slot : Slots)
	{
		if (Remaining == 0) { break; }
		if (!Slot.IsEmpty() && Slot.CanMergeWith(Stack) && Slot.Count < MaxStack)
		{
			const int32 Moved = FMath::Min(Remaining, MaxStack - Slot.Count);
			Slot.Count += Moved;
			Remaining -= Moved;
		}
	}

	for (FMadItemStack& Slot : Slots)
	{
		if (Remaining == 0) { break; }
		if (Slot.IsEmpty())
		{
			const int32 Moved = FMath::Min(Remaining, MaxStack);
			Slot = Stack;
			Slot.Count = Moved;
			Remaining -= Moved;
		}
	}

	return Remaining;
}

int32 FMadInventory::CountItem(FName Item) const
{
	int32 Total = 0;
	for (const FMadItemStack& Slot : Slots)
	{
		if (!Slot.IsEmpty() && Slot.Item == Item)
		{
			Total += Slot.Count;
		}
	}
	return Total;
}

bool FMadInventory::HasAll(const TArray<FMadItemAmount>& Amounts) const
{
	TMap<FName, int32> Needed;
	for (const FMadItemAmount& Amount : Amounts)
	{
		Needed.FindOrAdd(Amount.Item) += Amount.Count;
	}
	for (const TPair<FName, int32>& Pair : Needed)
	{
		if (CountItem(Pair.Key) < Pair.Value)
		{
			return false;
		}
	}
	return true;
}

bool FMadInventory::Remove(FName Item, int32 Count)
{
	if (Count <= 0)
	{
		return true;
	}
	if (CountItem(Item) < Count)
	{
		return false;
	}

	// Last slots first, so the hotbar (the first slots) is the last thing a
	// craft eats into.
	int32 Remaining = Count;
	for (int32 Index = Slots.Num() - 1; Index >= 0 && Remaining > 0; --Index)
	{
		FMadItemStack& Slot = Slots[Index];
		if (Slot.IsEmpty() || Slot.Item != Item)
		{
			continue;
		}
		const int32 Taken = FMath::Min(Remaining, Slot.Count);
		Slot.Count -= Taken;
		Remaining -= Taken;
		if (Slot.Count == 0)
		{
			Slot = FMadItemStack();
		}
	}
	return true;
}

bool FMadInventory::RemoveAll(const TArray<FMadItemAmount>& Amounts)
{
	if (!HasAll(Amounts))
	{
		return false;
	}
	for (const FMadItemAmount& Amount : Amounts)
	{
		verify(Remove(Amount.Item, Amount.Count));
	}
	return true;
}

bool FMadInventory::CanAddAll(const TArray<FMadItemStack>& Stacks, const FMadGameplayDefinitions& Definitions) const
{
	FMadInventory Copy = *this;
	for (const FMadItemStack& Stack : Stacks)
	{
		if (Copy.Add(Stack, Definitions) > 0)
		{
			return false;
		}
	}
	return true;
}

FMadItemStack FMadInventory::TakeFromSlot(int32 Index, int32 Count)
{
	if (!Slots.IsValidIndex(Index) || Slots[Index].IsEmpty() || Count <= 0)
	{
		return FMadItemStack();
	}

	FMadItemStack& Slot = Slots[Index];
	FMadItemStack Taken = Slot;
	Taken.Count = FMath::Min(Count, Slot.Count);
	Slot.Count -= Taken.Count;
	if (Slot.Count == 0)
	{
		Slot = FMadItemStack();
	}
	return Taken;
}

bool FMadInventory::IsEmpty() const
{
	for (const FMadItemStack& Slot : Slots)
	{
		if (!Slot.IsEmpty())
		{
			return false;
		}
	}
	return true;
}

// ===========================================================================
// Inventory screen moves
// ===========================================================================

namespace MadFall::InventoryOps
{
	const TCHAR* ToString(EMadSlotMove Result)
	{
		switch (Result)
		{
		case EMadSlotMove::Moved:       return TEXT("moved");
		case EMadSlotMove::Merged:      return TEXT("merged");
		case EMadSlotMove::Swapped:     return TEXT("swapped");
		case EMadSlotMove::Nothing:     return TEXT("nothing to move");
		case EMadSlotMove::InvalidSlot: return TEXT("invalid slot");
		default:                        return TEXT("?");
		}
	}

	EMadSlotMove MoveSlot(FMadInventory& From, int32 FromIndex, FMadInventory& To, int32 ToIndex, int32 Count,
		const FMadGameplayDefinitions& Definitions)
	{
		if (FromIndex < 0 || FromIndex >= From.NumSlots() || ToIndex < 0 || ToIndex >= To.NumSlots())
		{
			return EMadSlotMove::InvalidSlot;
		}
		if (&From == &To && FromIndex == ToIndex)
		{
			return EMadSlotMove::Nothing;
		}

		// Copies, not references: From and To may be the same inventory.
		const FMadItemStack Source = From.GetSlot(FromIndex);
		const FMadItemStack Target = To.GetSlot(ToIndex);
		if (Source.IsEmpty())
		{
			return EMadSlotMove::Nothing;
		}

		const int32 Wanted = Count <= 0 ? Source.Count : FMath::Min(Count, Source.Count);
		const FMadItemDefinition* Definition = Definitions.FindItem(Source.Item);
		const int32 MaxStack = Definition ? Definition->MaxStack : 1;

		if (Target.IsEmpty())
		{
			const int32 Moved = FMath::Min(Wanted, FMath::Max(1, MaxStack));
			FMadItemStack Placed = Source;
			Placed.Count = Moved;
			FMadItemStack Left = Source;
			Left.Count -= Moved;
			From.SetSlot(FromIndex, Left.Count > 0 ? Left : FMadItemStack());
			To.SetSlot(ToIndex, Placed);
			return EMadSlotMove::Moved;
		}

		if (Target.CanMergeWith(Source))
		{
			const int32 Moved = FMath::Min(Wanted, MaxStack - Target.Count);
			if (Moved <= 0)
			{
				return EMadSlotMove::Nothing;
			}
			FMadItemStack Topped = Target;
			Topped.Count += Moved;
			FMadItemStack Left = Source;
			Left.Count -= Moved;
			From.SetSlot(FromIndex, Left.Count > 0 ? Left : FMadItemStack());
			To.SetSlot(ToIndex, Topped);
			return EMadSlotMove::Merged;
		}

		if (Wanted < Source.Count)
		{
			return EMadSlotMove::Nothing;
		}
		From.SetSlot(FromIndex, Target);
		To.SetSlot(ToIndex, Source);
		return EMadSlotMove::Swapped;
	}

	bool SortRange(FMadInventory& Inventory, int32 First, int32 Last, const FMadGameplayDefinitions& Definitions)
	{
		First = FMath::Max(0, First);
		Last = FMath::Min(Last, Inventory.NumSlots() - 1);
		if (First > Last)
		{
			return false;
		}

		TArray<FMadItemStack> Stacks;
		for (int32 Index = First; Index <= Last; ++Index)
		{
			if (!Inventory.GetSlot(Index).IsEmpty())
			{
				Stacks.Add(Inventory.GetSlot(Index));
			}
		}

		// Plain stacks of one item pool into totals; anything with state stays whole.
		TArray<FMadItemStack> Merged;
		for (const FMadItemStack& Stack : Stacks)
		{
			const bool bPlain = Stack.Durability < 0 && Stack.Mods.Num() == 0 && Definitions.FindItem(Stack.Item) != nullptr;
			FMadItemStack* Pool = bPlain
				? Merged.FindByPredicate([&Stack](const FMadItemStack& Other) { return Other.CanMergeWith(Stack); })
				: nullptr;
			if (Pool != nullptr)
			{
				Pool->Count += Stack.Count;
			}
			else
			{
				Merged.Add(Stack);
			}
		}
		TArray<FMadItemStack> Laid;
		for (const FMadItemStack& Pool : Merged)
		{
			const bool bPlain = Pool.Durability < 0 && Pool.Mods.Num() == 0 && Definitions.FindItem(Pool.Item) != nullptr;
			if (!bPlain)
			{
				Laid.Add(Pool);
				continue;
			}
			const int32 MaxStack = FMath::Max(1, MaxStackFor(Pool.Item, Definitions));
			for (int32 Remaining = Pool.Count; Remaining > 0; Remaining -= MaxStack)
			{
				FMadItemStack Piece = Pool;
				Piece.Count = FMath::Min(Remaining, MaxStack);
				Laid.Add(Piece);
			}
		}
		if (Laid.Num() > Last - First + 1)
		{
			Laid = Stacks;   // cannot re-split without losing items: reorder only
		}

		auto KindOrder = [](EMadItemKind Kind)
		{
			switch (Kind)
			{
			case EMadItemKind::Tool:       return 0;
			case EMadItemKind::Weapon:     return 1;
			case EMadItemKind::Mod:        return 2;
			case EMadItemKind::Consumable: return 3;
			case EMadItemKind::Block:      return 4;
			default:                       return 5;
			}
		};
		// Names are looked up once: a sort compares each pair several times.
		TMap<FName, FString> Names;
		for (const FMadItemStack& Stack : Laid)
		{
			if (!Names.Contains(Stack.Item))
			{
				Names.Add(Stack.Item, Definitions.GetItemName(Stack.Item));
			}
		}
		Algo::StableSort(Laid, [&](const FMadItemStack& A, const FMadItemStack& B)
		{
			const FMadItemDefinition* DefA = Definitions.FindItem(A.Item);
			const FMadItemDefinition* DefB = Definitions.FindItem(B.Item);
			if ((DefA == nullptr) != (DefB == nullptr))
			{
				return DefA != nullptr;
			}
			if (DefA != nullptr && KindOrder(DefA->Kind) != KindOrder(DefB->Kind))
			{
				return KindOrder(DefA->Kind) < KindOrder(DefB->Kind);
			}
			if (A.Item != B.Item)
			{
				const int32 ByName = Names[A.Item].Compare(Names[B.Item], ESearchCase::IgnoreCase);
				return ByName != 0 ? ByName < 0 : A.Item.LexicalLess(B.Item);
			}
			if (A.Durability != B.Durability)
			{
				return A.Durability > B.Durability;
			}
			return A.Count > B.Count;
		});

		bool bChanged = false;
		for (int32 Index = First; Index <= Last; ++Index)
		{
			const int32 From = Index - First;
			const FMadItemStack Next = From < Laid.Num() ? Laid[From] : FMadItemStack();
			if (!(Inventory.GetSlot(Index) == Next))
			{
				Inventory.SetSlot(Index, Next);
				bChanged = true;
			}
		}
		return bChanged;
	}

	int32 QuickMove(FMadInventory& From, int32 FromIndex, FMadInventory& To, int32 ToFirst, int32 ToLast,
		const FMadGameplayDefinitions& Definitions)
	{
		if (FromIndex < 0 || FromIndex >= From.NumSlots() || From.GetSlot(FromIndex).IsEmpty())
		{
			return 0;
		}
		ToFirst = FMath::Max(0, ToFirst);
		ToLast = FMath::Min(To.NumSlots() - 1, ToLast);

		const bool bSameInventory = &From == &To;
		const FMadItemStack Source = From.GetSlot(FromIndex);
		const FMadItemDefinition* Definition = Definitions.FindItem(Source.Item);
		const int32 MaxStack = FMath::Max(1, Definition ? Definition->MaxStack : 1);
		int32 Remaining = Source.Count;

		// Same order as FMadInventory::Add - top up, then empty slots - but
		// limited to the target range and never back into the source slot.
		for (int32 Pass = 0; Pass < 2 && Remaining > 0; ++Pass)
		{
			for (int32 Index = ToFirst; Index <= ToLast && Remaining > 0; ++Index)
			{
				if (bSameInventory && Index == FromIndex)
				{
					continue;
				}
				FMadItemStack Slot = To.GetSlot(Index);
				if (Pass == 0 && !Slot.IsEmpty() && Slot.CanMergeWith(Source) && Slot.Count < MaxStack)
				{
					const int32 Moved = FMath::Min(Remaining, MaxStack - Slot.Count);
					Slot.Count += Moved;
					Remaining -= Moved;
					To.SetSlot(Index, Slot);
				}
				else if (Pass == 1 && Slot.IsEmpty())
				{
					Slot = Source;
					Slot.Count = FMath::Min(Remaining, MaxStack);
					Remaining -= Slot.Count;
					To.SetSlot(Index, Slot);
				}
			}
		}

		FMadItemStack Left = Source;
		Left.Count = Remaining;
		From.SetSlot(FromIndex, Remaining > 0 ? Left : FMadItemStack());
		return Source.Count - Remaining;
	}
}

// ===========================================================================
// Crafting
// ===========================================================================

namespace MadFall::Crafting
{
	const TCHAR* ToString(EMadCraftResult Result)
	{
		switch (Result)
		{
		case EMadCraftResult::Ok:                 return TEXT("ok");
		case EMadCraftResult::UnknownItem:        return TEXT("unknown item");
		case EMadCraftResult::MissingIngredients: return TEXT("missing ingredients");
		case EMadCraftResult::NoSpace:            return TEXT("no inventory space");
		case EMadCraftResult::WrongStation:       return TEXT("needs a crafting station");
		case EMadCraftResult::LevelTooLow:        return TEXT("level too low");
		default:                                  return TEXT("?");
		}
	}

	const TCHAR* GetCategoryName(EMadRecipeCategory Category)
	{
		switch (Category)
		{
		case EMadRecipeCategory::All:       return TEXT("All");
		case EMadRecipeCategory::Tools:     return TEXT("Tools");
		case EMadRecipeCategory::Building:  return TEXT("Build");
		case EMadRecipeCategory::Food:      return TEXT("Food");
		case EMadRecipeCategory::Clothing:  return TEXT("Wear");
		case EMadRecipeCategory::Materials: return TEXT("Other");
		default:                            return TEXT("?");
		}
	}

	EMadRecipeCategory CategoryOf(const FMadRecipeDefinition& Recipe, const FMadGameplayDefinitions& Definitions)
	{
		const FMadItemDefinition* Output = Definitions.FindItem(Recipe.Output.Item);
		if (Output == nullptr)
		{
			return EMadRecipeCategory::Materials;
		}
		if (Output->bHasTool || Output->bHasMod)
		{
			return EMadRecipeCategory::Tools;
		}
		if (!Output->PlacesBlock.IsNone())
		{
			return EMadRecipeCategory::Building;
		}
		if (Output->bHasConsumable)
		{
			return EMadRecipeCategory::Food;
		}
		if (Output->bHasWear)
		{
			return EMadRecipeCategory::Clothing;
		}
		// Ammunition belongs with what fires it.
		for (const FMadItemDefinition& Item : Definitions.GetItems())
		{
			if (Item.bHasTool && Item.Tool.Ammo == Output->Id)
			{
				return EMadRecipeCategory::Tools;
			}
		}
		return EMadRecipeCategory::Materials;
	}

	void ListRecipes(const FMadGameplayDefinitions& Definitions, const FMadInventory& Inventory, int32 PlayerLevel,
		TFunctionRef<bool(FName Station)> HasStation, EMadRecipeCategory Category, bool bCraftableOnly, TArray<FMadRecipeRow>& Out)
	{
		Out.Reset();
		for (const FMadRecipeDefinition& Recipe : Definitions.GetRecipes())
		{
			if (Category != EMadRecipeCategory::All && CategoryOf(Recipe, Definitions) != Category)
			{
				continue;
			}
			FMadRecipeRow Row;
			Row.Recipe = &Recipe;
			Row.Craftable = MaxCraftable(Inventory, Recipe);
			Row.bStation = HasStation(Recipe.Station);
			Row.bLevel = PlayerLevel >= Recipe.RequiredLevel;
			if (!bCraftableOnly || Row.CanCraftNow())
			{
				Out.Add(Row);
			}
		}
		Out.StableSort([&Definitions](const FMadRecipeRow& A, const FMadRecipeRow& B)
		{
			if (A.CanCraftNow() != B.CanCraftNow())
			{
				return A.CanCraftNow();
			}
			const FString NameA = Definitions.GetItemName(A.Recipe->Output.Item);
			const FString NameB = Definitions.GetItemName(B.Recipe->Output.Item);
			return NameA != NameB ? NameA < NameB : A.Recipe->Id.LexicalLess(B.Recipe->Id);
		});
	}

	int32 MaxCraftable(const FMadInventory& Inventory, const FMadRecipeDefinition& Recipe)
	{
		TMap<FName, int32> Needed;
		for (const FMadItemAmount& Ingredient : Recipe.Ingredients)
		{
			Needed.FindOrAdd(Ingredient.Item) += Ingredient.Count;
		}

		int32 Result = MAX_int32;
		for (const TPair<FName, int32>& Pair : Needed)
		{
			Result = FMath::Min(Result, Inventory.CountItem(Pair.Key) / FMath::Max(1, Pair.Value));
		}
		return Result == MAX_int32 ? 0 : Result;
	}

	EMadCraftResult Craft(FMadInventory& Inventory, const FMadRecipeDefinition& Recipe,
		const FMadGameplayDefinitions& Definitions, FName Station, int32 PlayerLevel, int32 Times)
	{
		Times = FMath::Max(1, Times);

		const FMadItemDefinition* Output = Definitions.FindItem(Recipe.Output.Item);
		if (Output == nullptr)
		{
			return EMadCraftResult::UnknownItem;
		}
		if (!Recipe.Station.IsNone() && Recipe.Station != Station)
		{
			return EMadCraftResult::WrongStation;
		}
		if (PlayerLevel < Recipe.RequiredLevel)
		{
			return EMadCraftResult::LevelTooLow;
		}
		if (MaxCraftable(Inventory, Recipe) < Times)
		{
			return EMadCraftResult::MissingIngredients;
		}

		// Work on a copy and commit only on full success. Removing first matters:
		// the ingredients' slots are often exactly where the output fits.
		FMadInventory Working = Inventory;
		for (const FMadItemAmount& Ingredient : Recipe.Ingredients)
		{
			verify(Working.Remove(Ingredient.Item, Ingredient.Count * Times));
		}

		// Equipment does not stack, so each crafted tool is added on its own.
		const int32 Total = Recipe.Output.Count * Times;
		const int32 PerAdd = Output->MaxStack == 1 ? 1 : Total;
		for (int32 Added = 0; Added < Total; Added += PerAdd)
		{
			if (Working.Add(FMadItemStack::Make(*Output, FMath::Min(PerAdd, Total - Added)), Definitions) > 0)
			{
				return EMadCraftResult::NoSpace;
			}
		}

		Inventory = MoveTemp(Working);
		return EMadCraftResult::Ok;
	}

	EMadCraftResult TakeIngredients(FMadInventory& Inventory, const FMadRecipeDefinition& Recipe,
		const FMadGameplayDefinitions& Definitions, FName Station, int32 PlayerLevel, int32 Times)
	{
		Times = FMath::Max(1, Times);

		if (Definitions.FindItem(Recipe.Output.Item) == nullptr)
		{
			return EMadCraftResult::UnknownItem;
		}
		if (!Recipe.Station.IsNone() && Recipe.Station != Station)
		{
			return EMadCraftResult::WrongStation;
		}
		if (PlayerLevel < Recipe.RequiredLevel)
		{
			return EMadCraftResult::LevelTooLow;
		}
		if (MaxCraftable(Inventory, Recipe) < Times)
		{
			return EMadCraftResult::MissingIngredients;
		}

		for (const FMadItemAmount& Ingredient : Recipe.Ingredients)
		{
			verify(Inventory.Remove(Ingredient.Item, Ingredient.Count * Times));
		}
		return EMadCraftResult::Ok;
	}
}

// ===========================================================================
// Loot
// ===========================================================================

namespace MadFall::Loot
{
	namespace
	{
		/** Nested tables are validated acyclic at load; this only guards content added at runtime. */
		constexpr int32 MaxDepth = 8;

		void AddMerged(TArray<FMadItemStack>& Out, const FMadItemStack& Stack, const FMadItemDefinition& Definition)
		{
			int32 Remaining = Stack.Count;
			if (Stack.Durability < 0)
			{
				for (FMadItemStack& Existing : Out)
				{
					if (Remaining == 0) { break; }
					if (Existing.CanMergeWith(Stack) && Existing.Count < Definition.MaxStack)
					{
						const int32 Moved = FMath::Min(Remaining, Definition.MaxStack - Existing.Count);
						Existing.Count += Moved;
						Remaining -= Moved;
					}
				}
			}
			while (Remaining > 0)
			{
				FMadItemStack Split = Stack;
				Split.Count = FMath::Min(Remaining, Definition.MaxStack);
				Remaining -= Split.Count;
				Out.Add(Split);
			}
		}

		void RollInto(const FMadLootTableDefinition& Table, const FMadGameplayDefinitions& Definitions,
			const FMadLootContext& Context, FRandomStream& Random, TArray<FMadItemStack>& Out, int32 Depth);

		void GiveEntry(const FMadLootEntry& Entry, const FMadGameplayDefinitions& Definitions,
			const FMadLootContext& Context, FRandomStream& Random, TArray<FMadItemStack>& Out, int32 Depth)
		{
			if (!Entry.Table.IsNone())
			{
				if (const FMadLootTableDefinition* Nested = Definitions.FindLootTable(Entry.Table))
				{
					RollInto(*Nested, Definitions, Context, Random, Out, Depth + 1);
				}
				return;
			}

			const FMadItemDefinition* Item = Definitions.FindItem(Entry.Item);
			const int32 Count = Random.RandRange(Entry.CountMin, Entry.CountMax);
			if (Item == nullptr || Count <= 0)
			{
				return;
			}

			if (Item->bHasTool && Item->Tool.Durability > 0)
			{
				// Each piece of equipment gets its own roll for wear.
				for (int32 Piece = 0; Piece < Count; ++Piece)
				{
					FMadItemStack Stack = FMadItemStack::Make(*Item, 1);
					const float Fraction = Random.FRandRange(Entry.DurabilityMin, Entry.DurabilityMax);
					Stack.Durability = FMath::Clamp(FMath::RoundToInt(Item->Tool.Durability * Fraction), 1, Item->Tool.Durability);
					Out.Add(Stack);
				}
			}
			else
			{
				AddMerged(Out, FMadItemStack::Make(*Item, Count), *Item);
			}
		}

		void RollInto(const FMadLootTableDefinition& Table, const FMadGameplayDefinitions& Definitions,
			const FMadLootContext& Context, FRandomStream& Random, TArray<FMadItemStack>& Out, int32 Depth)
		{
			if (Depth > MaxDepth)
			{
				return;
			}

			const float TierRolls = Table.RollsPerTier * static_cast<float>(FMath::Max(0, Context.Tier - 1));
			const int32 Rolls = Random.RandRange(Table.RollsMin, Table.RollsMax) + FMath::FloorToInt(TierRolls);

			TArray<const FMadLootEntry*> Eligible;
			float TotalWeight = Table.EmptyWeight;
			for (const FMadLootEntry& Entry : Table.Entries)
			{
				if (Context.Tier < Entry.MinTier || Context.Tier > Entry.MaxTier || Context.GameStage < Entry.MinGameStage)
				{
					continue;
				}
				if (Entry.bAlways)
				{
					GiveEntry(Entry, Definitions, Context, Random, Out, Depth);
				}
				else if (Entry.Weight > 0.0f)
				{
					Eligible.Add(&Entry);
					TotalWeight += Entry.Weight;
				}
			}

			if (Eligible.Num() == 0 || TotalWeight <= 0.0f)
			{
				return;
			}

			for (int32 RollIndex = 0; RollIndex < Rolls; ++RollIndex)
			{
				float Pick = Random.FRandRange(0.0f, TotalWeight);
				if (Pick < Table.EmptyWeight)
				{
					continue;
				}
				Pick -= Table.EmptyWeight;

				const FMadLootEntry* Chosen = Eligible.Last();
				for (const FMadLootEntry* Entry : Eligible)
				{
					if (Pick < Entry->Weight)
					{
						Chosen = Entry;
						break;
					}
					Pick -= Entry->Weight;
				}

				GiveEntry(*Chosen, Definitions, Context, Random, Out, Depth);
			}
		}
	}

	void Roll(const FMadLootTableDefinition& Table, const FMadGameplayDefinitions& Definitions,
		const FMadLootContext& Context, FRandomStream& Random, TArray<FMadItemStack>& OutStacks)
	{
		OutStacks.Reset();
		RollInto(Table, Definitions, Context, Random, OutStacks, 0);
	}
}

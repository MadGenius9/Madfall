// Copyright MadFall. All Rights Reserved.

#include "MadHarvest.h"

#include "MadBlockDamage.h"
#include "MadBlockDefinition.h"
#include "Math/RandomStream.h"

namespace MadFall::Harvest
{
	namespace
	{
		const FName BluntType(TEXT("madfall:blunt"));
		constexpr float BareHandsDamage = 8.0f;
		constexpr float WrongToolMultiplier = 0.25f;
		constexpr float LowTierMultiplier = 0.5f;

		bool SharesTag(const TArray<FName>& A, const TArray<FName>& B)
		{
			for (const FName& Tag : A)
			{
				if (B.Contains(Tag))
				{
					return true;
				}
			}
			return false;
		}
	}

	float GetModMultiplier(const FMadItemStack& Stack, const FMadGameplayDefinitions& Definitions, FName Stat)
	{
		const FMadItemDefinition* Item = Definitions.FindItem(Stack.Item);
		if (Item == nullptr)
		{
			return 1.0f;
		}

		float Multiplier = 1.0f;
		for (const FName& ModId : Stack.Mods)
		{
			const FMadItemDefinition* Mod = Definitions.FindItem(ModId);
			if (Mod == nullptr || !Mod->bHasMod)
			{
				continue;
			}
			if (Mod->Mod.AppliesToTags.Num() > 0 && !SharesTag(Mod->Mod.AppliesToTags, Item->Tags))
			{
				continue;
			}
			if (const float* Value = Mod->Mod.Multipliers.Find(Stat))
			{
				Multiplier *= FMath::Max(0.0f, *Value);
			}
		}
		return Multiplier;
	}

	FMadToolHit ComputeHit(const FMadItemStack* Held, const FMadGameplayDefinitions& Definitions, const FMadBlockDefinitionData& Block)
	{
		FMadToolHit Hit;

		const FMadItemDefinition* Item = (Held != nullptr && !Held->IsEmpty()) ? Definitions.FindItem(Held->Item) : nullptr;
		const bool bTool = Item != nullptr && Item->bHasTool && Item->Tool.Damage.Num() > 0;

		if (bTool)
		{
			const float ModDamage = GetModMultiplier(*Held, Definitions, FName(TEXT("damage")));
			float BestEffective = -1.0f;
			for (const TPair<FName, float>& Pair : Item->Tool.Damage)
			{
				const float Effective = Pair.Value * MadFall::BlockDamage::GetResistance(Block, Pair.Key);
				// Ties break on the type name, so the choice never depends on map order.
				if (Effective > BestEffective || (Effective == BestEffective && Pair.Key.LexicalLess(Hit.DamageType)))
				{
					BestEffective = Effective;
					Hit.DamageType = Pair.Key;
					Hit.Amount = Pair.Value * ModDamage;
				}
			}
		}
		else
		{
			Hit.DamageType = BluntType;
			Hit.Amount = BareHandsDamage;
		}

		if (Block.HarvestToolTags.Num() == 0)
		{
			Hit.bHarvests = true;
			return Hit;
		}

		if (bTool && SharesTag(Item->Tool.HarvestTags, Block.HarvestToolTags))
		{
			if (Item->Tool.Tier >= Block.HarvestTier)
			{
				Hit.bHarvests = true;
			}
			else
			{
				Hit.Amount *= LowTierMultiplier;
				Hit.bPenalised = true;
			}
			return Hit;
		}

		Hit.Amount *= WrongToolMultiplier;
		Hit.bPenalised = true;
		return Hit;
	}

	void ApplyYieldBonus(TArray<FMadItemStack>& Drops, float Multiplier, FRandomStream& Random)
	{
		if (Multiplier <= 1.0f)
		{
			return;
		}
		for (FMadItemStack& Stack : Drops)
		{
			if (Stack.IsEmpty())
			{
				continue;
			}
			const float Scaled = static_cast<float>(Stack.Count) * Multiplier;
			int32 Whole = FMath::FloorToInt32(Scaled);
			if (Random.FRand() < Scaled - static_cast<float>(Whole))
			{
				++Whole;
			}
			Stack.Count = FMath::Max(Stack.Count, Whole);
		}
	}

	void RollDrops(const FMadBlockDefinitionData& Block, const FMadItemStack* Held, bool bHarvests,
		const FMadGameplayDefinitions& Definitions, FRandomStream& Random, TArray<FMadItemStack>& OutDrops)
	{
		OutDrops.Reset();
		if (!bHarvests)
		{
			return;
		}

		if (Block.RequiresToolTags.Num() > 0)
		{
			const FMadItemDefinition* Item = (Held != nullptr && !Held->IsEmpty()) ? Definitions.FindItem(Held->Item) : nullptr;
			if (Item == nullptr || !Item->bHasTool || !SharesTag(Item->Tool.HarvestTags, Block.RequiresToolTags))
			{
				return;
			}
		}

		if (const FMadLootTableDefinition* Table = Definitions.FindLootTable(Block.DropTable))
		{
			MadFall::Loot::Roll(*Table, Definitions, FMadLootContext(), Random, OutDrops);
			return;
		}

		if (const FMadItemDefinition* BlockItem = Definitions.FindItemForBlock(Block.Id))
		{
			OutDrops.Add(FMadItemStack::Make(*BlockItem, 1));
		}
	}

	bool ConsumeDurability(FMadItemStack& Stack, const FMadGameplayDefinitions& Definitions, FRandomStream& Random)
	{
		if (Stack.IsEmpty() || Stack.Durability < 0)
		{
			return false;
		}

		const float DurabilityMultiplier = FMath::Max(0.01f, GetModMultiplier(Stack, Definitions, FName(TEXT("durability"))));
		if (DurabilityMultiplier > 1.0f && Random.FRand() >= 1.0f / DurabilityMultiplier)
		{
			return false;
		}

		Stack.Durability -= 1;
		if (Stack.Durability <= 0)
		{
			Stack = FMadItemStack();
			return true;
		}
		return false;
	}
}

namespace MadFall::Items
{
	const TCHAR* ToString(EMadItemActionResult Result)
	{
		switch (Result)
		{
		case EMadItemActionResult::Ok:               return TEXT("ok");
		case EMadItemActionResult::NotATool:         return TEXT("not a tool");
		case EMadItemActionResult::NothingToDo:      return TEXT("nothing to do");
		case EMadItemActionResult::MissingMaterials: return TEXT("missing materials");
		case EMadItemActionResult::NoFreeSlot:       return TEXT("no free mod slot");
		case EMadItemActionResult::IncompatibleMod:  return TEXT("that mod does not fit this item");
		default:                                     return TEXT("?");
		}
	}

	EMadItemActionResult Repair(FMadInventory& Inventory, int32 ToolSlot, const FMadGameplayDefinitions& Definitions)
	{
		if (ToolSlot < 0 || ToolSlot >= Inventory.NumSlots())
		{
			return EMadItemActionResult::NotATool;
		}

		FMadItemStack Tool = Inventory.GetSlot(ToolSlot);
		const FMadItemDefinition* Item = Tool.IsEmpty() ? nullptr : Definitions.FindItem(Tool.Item);
		if (Item == nullptr || !Item->bHasTool || Item->Tool.Durability <= 0 || Tool.Durability < 0)
		{
			return EMadItemActionResult::NotATool;
		}
		if (Tool.Durability >= Item->Tool.Durability || Item->Tool.RepairWith.Num() == 0)
		{
			return EMadItemActionResult::NothingToDo;
		}

		// Remove the cost from a copy, so the tool's own slot cannot be consumed
		// as its own repair material and nothing changes on failure.
		FMadInventory Working = Inventory;
		Working.SetSlot(ToolSlot, FMadItemStack());
		if (!Working.RemoveAll(Item->Tool.RepairWith))
		{
			return EMadItemActionResult::MissingMaterials;
		}

		const int32 Restore = FMath::Max(1, FMath::RoundToInt32(Item->Tool.Durability * Item->Tool.RepairFraction));
		Tool.Durability = FMath::Min(Item->Tool.Durability, Tool.Durability + Restore);
		Working.SetSlot(ToolSlot, Tool);
		Inventory = MoveTemp(Working);
		return EMadItemActionResult::Ok;
	}

	EMadItemActionResult InstallMod(FMadInventory& Inventory, int32 ToolSlot, FName ModItem, const FMadGameplayDefinitions& Definitions)
	{
		if (ToolSlot < 0 || ToolSlot >= Inventory.NumSlots())
		{
			return EMadItemActionResult::NotATool;
		}

		FMadItemStack Tool = Inventory.GetSlot(ToolSlot);
		const FMadItemDefinition* Item = Tool.IsEmpty() ? nullptr : Definitions.FindItem(Tool.Item);
		if (Item == nullptr || !Item->bHasTool)
		{
			return EMadItemActionResult::NotATool;
		}

		const FMadItemDefinition* Mod = Definitions.FindItem(ModItem);
		if (Mod == nullptr || !Mod->bHasMod)
		{
			return EMadItemActionResult::IncompatibleMod;
		}
		if (Mod->Mod.AppliesToTags.Num() > 0 && !Harvest::SharesTag(Mod->Mod.AppliesToTags, Item->Tags))
		{
			return EMadItemActionResult::IncompatibleMod;
		}
		if (Tool.Mods.Num() >= Item->Tool.ModSlots)
		{
			return EMadItemActionResult::NoFreeSlot;
		}

		FMadInventory Working = Inventory;
		Working.SetSlot(ToolSlot, FMadItemStack());
		if (!Working.Remove(ModItem, 1))
		{
			return EMadItemActionResult::MissingMaterials;
		}

		Tool.Mods.Add(ModItem);
		Working.SetSlot(ToolSlot, Tool);
		Inventory = MoveTemp(Working);
		return EMadItemActionResult::Ok;
	}

	EMadItemActionResult RemoveMod(FMadInventory& Inventory, int32 ToolSlot, int32 ModIndex, const FMadGameplayDefinitions& Definitions)
	{
		if (ToolSlot < 0 || ToolSlot >= Inventory.NumSlots() || Inventory.GetSlot(ToolSlot).IsEmpty())
		{
			return EMadItemActionResult::NotATool;
		}
		FMadItemStack Tool = Inventory.GetSlot(ToolSlot);
		if (Tool.Mods.Num() == 0)
		{
			return EMadItemActionResult::NothingToDo;
		}
		const int32 Index = ModIndex == INDEX_NONE ? Tool.Mods.Num() - 1 : ModIndex;
		if (!Tool.Mods.IsValidIndex(Index))
		{
			return EMadItemActionResult::NothingToDo;
		}

		FMadItemStack Mod;
		Mod.Item = Tool.Mods[Index];
		Mod.Count = 1;
		Tool.Mods.RemoveAt(Index);

		FMadInventory Working = Inventory;
		Working.SetSlot(ToolSlot, Tool);
		if (Working.Add(Mod, Definitions) > 0)
		{
			return EMadItemActionResult::NoFreeSlot;
		}
		Inventory = MoveTemp(Working);
		return EMadItemActionResult::Ok;
	}

	bool IsModInstallDrop(const FMadItemStack& Held, const FMadItemStack& Target, const FMadGameplayDefinitions& Definitions)
	{
		if (Held.IsEmpty() || Target.IsEmpty())
		{
			return false;
		}
		const FMadItemDefinition* Mod = Definitions.FindItem(Held.Item);
		const FMadItemDefinition* Tool = Definitions.FindItem(Target.Item);
		return Mod != nullptr && Mod->bHasMod && Tool != nullptr && Tool->bHasTool;
	}

	namespace
	{
		/** "madfall:blunt" -> "blunt", "max_health" -> "max health". */
		FString ReadableKey(FName Key)
		{
			FString Text = Key.ToString();
			int32 Colon = INDEX_NONE;
			if (Text.FindLastChar(TEXT(':'), Colon))
			{
				Text.RightChopInline(Colon + 1);
			}
			return Text.Replace(TEXT("_"), TEXT(" "));
		}

		/** Map entries sorted by key, so a tooltip reads the same every time. */
		template <typename ValueType>
		TArray<TPair<FName, ValueType>> Sorted(const TMap<FName, ValueType>& Map)
		{
			TArray<TPair<FName, ValueType>> Out = Map.Array();
			Out.Sort([](const TPair<FName, ValueType>& A, const TPair<FName, ValueType>& B) { return A.Key.LexicalLess(B.Key); });
			return Out;
		}

		FString Signed(float Value)
		{
			return FString::Printf(TEXT("%s%s"), Value >= 0.0f ? TEXT("+") : TEXT(""), *FString::SanitizeFloat(FMath::RoundToFloat(Value * 10.0f) / 10.0f, 0));
		}
	}

	void DescribeStack(const FMadItemStack& Stack, const FMadGameplayDefinitions& Definitions, TArray<FString>& OutLines)
	{
		OutLines.Reset();
		if (Stack.IsEmpty())
		{
			return;
		}

		const FMadItemDefinition* Item = Definitions.FindItem(Stack.Item);
		if (Item == nullptr)
		{
			OutLines.Add(Stack.Item.ToString());
			OutLines.Add(TEXT("From a mod that is not installed. Kept safe until it returns."));
			return;
		}

		OutLines.Add(Stack.Count > 1 ? FString::Printf(TEXT("%s  x%d"), *Definitions.GetItemName(Stack.Item), Stack.Count)
			: Definitions.GetItemName(Stack.Item));

		if (!Item->PlacesBlock.IsNone())
		{
			OutLines.Add(TEXT("Block - right-click to place"));
		}

		if (Item->bHasTool)
		{
			const FMadToolStats& Tool = Item->Tool;
			const float DamageScale = Harvest::GetModMultiplier(Stack, Definitions, FName(TEXT("damage")));
			FString Damage;
			for (const TPair<FName, float>& Pair : Sorted(Tool.Damage))
			{
				Damage += FString::Printf(TEXT("%s%.0f %s"), Damage.IsEmpty() ? TEXT("") : TEXT(", "), Pair.Value * DamageScale, *ReadableKey(Pair.Key));
			}
			if (!Damage.IsEmpty())
			{
				OutLines.Add(FString::Printf(TEXT("Damage %s"), *Damage));
			}
			if (Tool.IsRanged())
			{
				OutLines.Add(FString::Printf(TEXT("Fires %s"), *Definitions.GetItemName(Tool.Ammo)));
			}
			if (Tool.HarvestTags.Num() > 0 || Tool.Tier > 0)
			{
				OutLines.Add(FString::Printf(TEXT("Harvest tier %d"), Tool.Tier));
			}
			if (Tool.Durability > 0 && Stack.Durability >= 0)
			{
				OutLines.Add(FString::Printf(TEXT("Durability %d / %d"), Stack.Durability, Tool.Durability));
			}
			if (Tool.ModSlots > 0)
			{
				FString Installed;
				for (const FName& Mod : Stack.Mods)
				{
					Installed += FString::Printf(TEXT("%s%s"), Installed.IsEmpty() ? TEXT(": ") : TEXT(", "), *Definitions.GetItemName(Mod));
				}
				OutLines.Add(FString::Printf(TEXT("Mods %d / %d%s"), Stack.Mods.Num(), Tool.ModSlots, *Installed));
			}
		}

		if (Item->bHasConsumable)
		{
			FString Effects;
			for (const TPair<FName, float>& Pair : Sorted(Item->Consumable.Effects))
			{
				Effects += FString::Printf(TEXT("%s%s %s"), Effects.IsEmpty() ? TEXT("") : TEXT(", "), *Signed(Pair.Value), *ReadableKey(Pair.Key));
			}
			OutLines.Add(FString::Printf(TEXT("Right-click to use%s%s"), Effects.IsEmpty() ? TEXT("") : TEXT(": "), *Effects));
		}

		if (Item->bHasWear)
		{
			const FMadWearStats& Wear = Item->Wear;
			FString Line = FString::Printf(TEXT("Worn on %s"), *Wear.Slot.ToString());
			if (Wear.Armor > 0.0f) { Line += FString::Printf(TEXT(", armor %.0f%%"), Wear.Armor * 100.0f); }
			if (Wear.Cold > 0.0f)  { Line += FString::Printf(TEXT(", warmth +%.0f C"), Wear.Cold); }
			if (Wear.Heat > 0.0f)  { Line += FString::Printf(TEXT(", cooling +%.0f C"), Wear.Heat); }
			OutLines.Add(Line);
		}

		if (Item->bHasMod)
		{
			FString Bonuses;
			for (const TPair<FName, float>& Pair : Sorted(Item->Mod.Multipliers))
			{
				const int32 Percent = FMath::RoundToInt32((Pair.Value - 1.0f) * 100.0f);
				Bonuses += FString::Printf(TEXT("%s%s %s%d%%"), Bonuses.IsEmpty() ? TEXT("") : TEXT(", "), *ReadableKey(Pair.Key), Percent >= 0 ? TEXT("+") : TEXT(""), Percent);
			}
			OutLines.Add(FString::Printf(TEXT("Item mod: %s"), *Bonuses));
			// "a tool", "a tool or weapon", "a tool, weapon or armor".
			FString Fits;
			const TArray<FName>& Tags = Item->Mod.AppliesToTags;
			for (int32 Index = 0; Index < Tags.Num(); ++Index)
			{
				const TCHAR* Separator = Index == 0 ? TEXT("") : (Index == Tags.Num() - 1 ? TEXT(" or ") : TEXT(", "));
				Fits += FString::Printf(TEXT("%s%s"), Separator, *ReadableKey(Tags[Index]).Replace(TEXT("item."), TEXT("")));
			}
			OutLines.Add(FString::Printf(TEXT("Drop onto a %s to install"), Fits.IsEmpty() ? TEXT("tool or weapon") : *Fits));
		}

		if (Item->MaxStack > 1)
		{
			OutLines.Add(FString::Printf(TEXT("Stacks to %d"), Item->MaxStack));
		}
	}
}

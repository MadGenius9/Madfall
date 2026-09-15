// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadBlockRegistry.h"
#include "MadDefinitionSources.h"
#include "MadGameplayDefinitions.h"
#include "MadHarvest.h"
#include "MadInventory.h"
#include "MadLocalization.h"
#include "MadProgression.h"
#include "MadPrefabRegistry.h"
#include "MadSurvivalModel.h"
#include "MadVoxelWorldSubsystem.h"
#include "Math/RandomStream.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadItemTests
{
	TSharedRef<FJsonObject> Json(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	}

	/**
	 * A small self-contained definition set: planks, a pickaxe family, a
	 * consumable, a mod, two recipes and nested loot. No block registry, so no
	 * generated block items.
	 */
	void BuildDefinitions(FMadGameplayDefinitions& Defs, TArray<FMadDefinitionError>& Errors)
	{
		const FName Mod(TEXT("test"));
		Defs.BeginLoad();

		Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:plank","max_stack":10})")), TEXT("items.json"), Mod, Errors);
		Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:rock","max_stack":50})")), TEXT("items.json"), Mod, Errors);
		Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:pick","tags":["item.tool"],
			"tool":{"damage":{"test:blunt":20},"harvest_tags":["tool.pickaxe"],"tier":1,"durability":10,"mod_slots":1,"repair_with":[{"item":"test:rock","count":2}],"repair_fraction":0.5}})")), TEXT("items.json"), Mod, Errors);
		Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:iron_pick","extends":"test:pick",
			"tool":{"damage":{"test:pierce":50},"tier":3}})")), TEXT("items.json"), Mod, Errors);
		Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:grip","mod":{"applies_to_tags":["item.tool"],"multipliers":{"damage":2.0,"durability":2.0}}})")), TEXT("items.json"), Mod, Errors);
		Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:food","max_stack":5,"consumable":{"effects":{"food":30}}})")), TEXT("items.json"), Mod, Errors);

		Defs.AddRecipeJson(Json(TEXT(R"({"schema":"madfall.recipe/1","id":"test:pick","output":{"item":"test:pick"},
			"ingredients":[{"item":"test:plank","count":2},{"item":"test:rock","count":3}]})")), TEXT("recipes.json"), Mod, Errors);
		Defs.AddRecipeJson(Json(TEXT(R"({"schema":"madfall.recipe/1","id":"test:planks","output":{"item":"test:plank","count":8},
			"ingredients":[{"item":"test:rock","count":1}],"station":"test:bench","required_level":2})")), TEXT("recipes.json"), Mod, Errors);

		Defs.AddLootJson(Json(TEXT(R"({"schema":"madfall.loot/1","id":"test:loot/inner","entries":[{"item":"test:rock","count":[1,5]}]})")), TEXT("loot.json"), Mod, Errors);
		Defs.AddLootJson(Json(TEXT(R"({"schema":"madfall.loot/1","id":"test:loot/outer","rolls":[3,3],"rolls_per_tier":1.0,"entries":[
			{"table":"test:loot/inner","weight":1},
			{"item":"test:pick","weight":1,"durability":[0.5,0.5]},
			{"item":"test:iron_pick","weight":100,"tier":[4,5]}]})")), TEXT("loot.json"), Mod, Errors);

		Defs.FinishLoad(nullptr, Errors);
	}
}

// ===========================================================================
// Definitions
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadItemDefinitionsTest,
	"MadFall.Items.Definitions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadItemDefinitionsTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);

	for (const FMadDefinitionError& Error : Errors)
	{
		AddError(Error.ToString());
	}

	TestEqual(TEXT("6 items"), Defs.GetItems().Num(), 6);
	TestEqual(TEXT("2 recipes"), Defs.GetRecipes().Num(), 2);
	TestEqual(TEXT("2 loot tables"), Defs.GetLootTables().Num(), 2);

	const FMadItemDefinition* Pick = Defs.FindItem(FName(TEXT("test:pick")));
	const FMadItemDefinition* IronPick = Defs.FindItem(FName(TEXT("test:iron_pick")));
	if (TestNotNull(TEXT("pick"), Pick) && TestNotNull(TEXT("iron pick"), IronPick))
	{
		TestTrue(TEXT("a tool section makes the kind Tool"), Pick->Kind == EMadItemKind::Tool);
		TestEqual(TEXT("tools never stack"), Pick->MaxStack, 1);
		TestEqual(TEXT("inherited durability"), IronPick->Tool.Durability, 10);
		TestEqual(TEXT("overridden tier"), IronPick->Tool.Tier, 3);
		TestEqual(TEXT("inherited harvest tags (arrays not mentioned survive)"), IronPick->Tool.HarvestTags.Num(), 1);
		TestEqual(TEXT("damage maps MERGE: blunt inherited plus pierce added"), IronPick->Tool.Damage.Num(), 2);
	}

	// --- validation --------------------------------------------------------
	{
		FMadGameplayDefinitions Bad;
		TArray<FMadDefinitionError> BadErrors;
		const FName Mod(TEXT("test"));
		Bad.BeginLoad();

		// Wrong namespace: rejected at staging.
		TestFalse(TEXT("foreign namespace rejected"), Bad.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"other:thing"})")), TEXT("a.json"), Mod, BadErrors));
		// A stackable tool: loads, forced to 1 with an error.
		Bad.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:stacky","max_stack":5,"tool":{"damage":{"x:y":1}}})")), TEXT("a.json"), Mod, BadErrors);
		// A recipe using an unknown item: disabled.
		Bad.AddRecipeJson(Json(TEXT(R"({"schema":"madfall.recipe/1","id":"test:r","output":{"item":"test:stacky"},"ingredients":[{"item":"test:nope"}]})")), TEXT("r.json"), Mod, BadErrors);
		// A loot cycle: a -> b -> a.
		Bad.AddLootJson(Json(TEXT(R"({"schema":"madfall.loot/1","id":"test:a","entries":[{"table":"test:b"}]})")), TEXT("l.json"), Mod, BadErrors);
		Bad.AddLootJson(Json(TEXT(R"({"schema":"madfall.loot/1","id":"test:b","entries":[{"table":"test:a"}]})")), TEXT("l.json"), Mod, BadErrors);
		// A typo'd field.
		Bad.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:typo","max_stak":5})")), TEXT("a.json"), Mod, BadErrors);

		Bad.FinishLoad(nullptr, BadErrors);

		auto HasError = [&BadErrors](const TCHAR* Needle)
		{
			return BadErrors.ContainsByPredicate([Needle](const FMadDefinitionError& E) { return E.Message.Contains(Needle); });
		};

		TestTrue(TEXT("namespace error reported"), HasError(TEXT("does not match the owning mod id")));
		TestTrue(TEXT("stack forced to 1 reported"), HasError(TEXT("cannot stack")));
		TestEqual(TEXT("stacky is loaded with max_stack 1"), Bad.FindItem(FName(TEXT("test:stacky")))->MaxStack, 1);
		TestNull(TEXT("recipe with an unknown ingredient is disabled"), Bad.FindRecipe(FName(TEXT("test:r"))));
		TestTrue(TEXT("unknown ingredient reported"), HasError(TEXT("unknown item 'test:nope'")));
		TestTrue(TEXT("loot cycle reported"), HasError(TEXT("creates a cycle")));
		TestTrue(TEXT("unknown field reported"), HasError(TEXT("unknown field")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadShippedItemContentTest,
	"MadFall.Items.ShippedContent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadShippedItemContentTest::RunTest(const FString& Parameters)
{
	// Load the real files into a private registry so every validation message
	// is visible to the test (the global one logs them).
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	Defs.BeginLoad();
	MadFall::Definitions::ForEachSource(TEXT("items"), [&](const FString& Dir, FName ModId) { Defs.AddItemsFromDirectory(Dir, ModId, Errors); });
	MadFall::Definitions::ForEachSource(TEXT("recipes"), [&](const FString& Dir, FName ModId) { Defs.AddRecipesFromDirectory(Dir, ModId, Errors); });
	MadFall::Definitions::ForEachSource(TEXT("loot"), [&](const FString& Dir, FName ModId) { Defs.AddLootFromDirectory(Dir, ModId, Errors); });
	MadFall::Definitions::ForEachSource(TEXT("zombies"), [&](const FString& Dir, FName ModId) { Defs.AddZombiesFromDirectory(Dir, ModId, Errors); });
	MadFall::Definitions::ForEachSource(TEXT("tuning"), [&](const FString& Dir, FName ModId) { Defs.AddTuningFromDirectory(Dir, ModId, Errors); });
	MadFall::Definitions::ForEachSource(TEXT("perks"), [&](const FString& Dir, FName ModId) { Defs.AddPerksFromDirectory(Dir, ModId, Errors); });
	Defs.FinishLoad(&Blocks, Errors);

	TestEqual(TEXT("shipped item/recipe/loot content has no validation errors"), Errors.Num(), 0);
	for (const FMadDefinitionError& Error : Errors)
	{
		AddError(Error.ToString());
	}

	TestTrue(TEXT("items loaded"), Defs.GetItems().Num() >= 16);
	TestTrue(TEXT("recipes loaded"), Defs.GetRecipes().Num() >= 10);
	TestTrue(TEXT("zombie variants loaded"), Defs.GetZombies().Num() >= 3);

	// Every loot table content points at must exist - blocks' drop tables and
	// POI loot markers. A missing one would silently fall back or drop nothing.
	for (const FMadBlockEntry& Entry : Blocks.GetEntries())
	{
		for (const FName& Table : { Entry.Definition.DropTable, Entry.Definition.DropTableOnCollapse })
		{
			if (!Table.IsNone())
			{
				TestNotNull(*FString::Printf(TEXT("block %s's table %s exists"), *Entry.Definition.Id.ToString(), *Table.ToString()),
					Defs.FindLootTable(Table));
			}
		}

		// A missing debris block is silent at runtime: the collapse just leaves
		// no rubble. It went unnoticed for wood and steel until debris dropped loot.
		if (!Entry.Definition.DebrisOnCollapse.IsNone())
		{
			TestTrue(*FString::Printf(TEXT("block %s's debris block %s exists"), *Entry.Definition.Id.ToString(), *Entry.Definition.DebrisOnCollapse.ToString()),
				Blocks.IsRegistered(Entry.Definition.DebrisOnCollapse));
		}

		// Every placeable construction block has an item.
		if (Entry.Definition.Tags.Contains(FName(TEXT("block.building"))))
		{
			TestNotNull(*FString::Printf(TEXT("block %s has an item"), *Entry.Definition.Id.ToString()),
				Defs.FindItemForBlock(Entry.Definition.Id));
		}
	}

	for (const FMadPrefab& Prefab : UMadVoxelWorldSubsystem::GetPrefabRegistry().GetAll())
	{
		for (const FMadPoiMarker& Marker : Prefab.Markers)
		{
			if (!Marker.LootTable.IsNone())
			{
				TestNotNull(*FString::Printf(TEXT("%s loot marker table %s exists"), *Prefab.Id.ToString(), *Marker.LootTable.ToString()),
					Defs.FindLootTable(Marker.LootTable));
			}
			if (!Marker.SpawnGroup.IsNone())
			{
				TArray<const FMadZombieDefinition*> Group;
				Defs.GetZombiesInGroup(Marker.SpawnGroup, 1000, Group);
				TestTrue(*FString::Printf(TEXT("%s spawn group %s has zombies"), *Prefab.Id.ToString(), *Marker.SpawnGroup.ToString()), Group.Num() > 0);
			}
		}
	}

	// Tuning keys and perk stats are validated by gameplay code, not Core, so a
	// typo in shipped balance data would otherwise only surface as a log warning.
	if (const FMadTuningDefinition* Tuning = Defs.FindTuning(FName(TEXT("madfall:survival"))); TestNotNull(TEXT("survival tuning shipped"), Tuning))
	{
		FMadSurvivalTuning Applied;
		TArray<FName> Unknown;
		MadFall::Survival::ApplyTuning(*Tuning, Applied, Unknown);
		TestEqual(TEXT("every shipped survival tuning key is recognised"), Unknown.Num(), 0);
		for (const FName& Key : Unknown)
		{
			AddError(FString::Printf(TEXT("unknown tuning key %s"), *Key.ToString()));
		}
	}
	// Localized text: every "@key" an item, perk or zombie shows has English.
	{
		const FMadStringTable& Strings = MadFall::GetStrings();
		auto CheckText = [&](FName Owner, const FString& Text)
		{
			if (Text.StartsWith(TEXT("@")))
			{
				TestNotNull(*FString::Printf(TEXT("%s: %s has English text"), *Owner.ToString(), *Text), Strings.Find(Text.Mid(1), TEXT("en")));
			}
		};
		for (const FMadItemDefinition& Item : Defs.GetItems())       { CheckText(Item.Id, Item.DisplayName); }
		for (const FMadZombieDefinition& Zombie : Defs.GetZombies()) { CheckText(Zombie.Id, Zombie.DisplayName); }
		for (const FMadPerkDefinition& Perk : Defs.GetPerks())       { CheckText(Perk.Id, Perk.DisplayName); CheckText(Perk.Id, Perk.Description); }
	}

	TestTrue(TEXT("perks shipped"), Defs.GetPerks().Num() >= 6);
	for (const FMadPerkDefinition& Perk : Defs.GetPerks())
	{
		int32 PreviousLevel = 0;
		for (const FMadPerkRank& Rank : Perk.Ranks)
		{
			TestTrue(*FString::Printf(TEXT("%s ranks unlock at rising levels"), *Perk.Id.ToString()), Rank.RequiredLevel > PreviousLevel);
			PreviousLevel = Rank.RequiredLevel;
			for (const TPair<FName, float>& Modifier : Rank.Modifiers)
			{
				TestTrue(*FString::Printf(TEXT("%s modifies known stat %s"), *Perk.Id.ToString(), *Modifier.Key.ToString()),
					MadFall::Perks::GetKnownStats().Contains(Modifier.Key));
			}
		}
	}

	TestNull(TEXT("bedrock (indestructible) has no item"), Defs.FindItemForBlock(FName(TEXT("madfall:bedrock"))));
	TestNull(TEXT("water (liquid) has no item"), Defs.FindItemForBlock(FName(TEXT("madfall:water"))));

	// Interactions point at things that exist, and a toggle comes back.
	for (const FMadBlockEntry& Entry : Blocks.GetEntries())
	{
		const FMadBlockDefinitionData& Block = Entry.Definition;
		if (Block.ToggleTo.IsNone())
		{
			continue;
		}
		const FMadBlockDefinitionData* Other = Blocks.FindDefinition(Blocks.ResolveRuntimeId(Block.ToggleTo));
		if (TestNotNull(*FString::Printf(TEXT("%s toggles to a registered block"), *Block.Id.ToString()), Other))
		{
			TestEqual(*FString::Printf(TEXT("%s toggles back"), *Block.Id.ToString()), Other->ToggleTo, Block.Id);
		}
	}
	for (const FMadItemDefinition& Item : Defs.GetItems())
	{
		if (!Item.Consumable.Returns.IsNone())
		{
			TestNotNull(*FString::Printf(TEXT("%s returns an item that exists"), *Item.Id.ToString()), Defs.FindItem(Item.Consumable.Returns));
		}
		if (!Item.FillsInto.IsNone())
		{
			TestNotNull(*FString::Printf(TEXT("%s fills into an item that exists"), *Item.Id.ToString()), Defs.FindItem(Item.FillsInto));
		}
	}
	TestTrue(TEXT("the shipped ladder is climbable"), Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(TEXT("madfall:ladder"))))
		&& Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(TEXT("madfall:ladder"))))->bClimbable);
	TestTrue(TEXT("the shipped bedroll is a spawn point"), Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(TEXT("madfall:bedroll"))))
		&& Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(TEXT("madfall:bedroll"))))->bSpawnPoint);

	AddInfo(Defs.DescribeContents());
	return true;
}

// ===========================================================================
// Inventory and crafting
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadInventoryTest,
	"MadFall.Items.Inventory",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadInventoryTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);

	const FName Plank(TEXT("test:plank"));
	const FName Pick(TEXT("test:pick"));

	FMadInventory Inventory(3);
	FMadItemStack Planks;
	Planks.Item = Plank;
	Planks.Count = 25;

	TestEqual(TEXT("25 planks at stack 10 fit exactly in 3 slots (10 + 10 + 5)"), Inventory.Add(Planks, Defs), 0);
	TestEqual(TEXT("count"), Inventory.CountItem(Plank), 25);

	Planks.Count = 10;
	TestEqual(TEXT("10 more: 5 top up the partial stack, 5 do not fit"), Inventory.Add(Planks, Defs), 5);
	TestEqual(TEXT("count is 30"), Inventory.CountItem(Plank), 30);

	TestFalse(TEXT("cannot remove more than present"), Inventory.Remove(Plank, 31));
	TestEqual(TEXT("failed remove changed nothing"), Inventory.CountItem(Plank), 30);
	TestTrue(TEXT("remove 15"), Inventory.Remove(Plank, 15));
	TestEqual(TEXT("first slot kept (removal takes from the end)"), Inventory.GetSlot(0).Count, 10);
	TestEqual(TEXT("count is 15"), Inventory.CountItem(Plank), 15);

	// Tools never merge even with the same id.
	FMadInventory Tools(2);
	const FMadItemStack PickStack = FMadItemStack::Make(*Defs.FindItem(Pick), 1);
	TestEqual(TEXT("fresh tool has full durability"), PickStack.Durability, 10);
	TestEqual(TEXT("first pick fits"), Tools.Add(PickStack, Defs), 0);
	TestEqual(TEXT("second pick takes its own slot"), Tools.Add(PickStack, Defs), 0);
	TestEqual(TEXT("third pick does not fit"), Tools.Add(PickStack, Defs), 1);

	TArray<FMadItemAmount> Needs = { { Plank, 5 }, { Plank, 5 } };
	TestTrue(TEXT("duplicate amounts add up (10 of 15)"), Inventory.HasAll(Needs));
	Needs.Add({ Plank, 6 });
	TestFalse(TEXT("16 of 15 is not affordable"), Inventory.HasAll(Needs));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadInventoryMovesTest,
	"MadFall.Items.InventoryMoves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadInventoryMovesTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;
	using MadFall::InventoryOps::MoveSlot;
	using MadFall::InventoryOps::QuickMove;

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);

	const FName Plank(TEXT("test:plank"));   // max stack 10
	const FName Rock(TEXT("test:rock"));     // max stack 50
	const FMadItemStack Pick = FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:pick"))), 1);
	auto Stack = [&Defs](FName Item, int32 Count) { return FMadItemStack::Make(*Defs.FindItem(Item), Count); };
	auto Total = [](const FMadInventory& A, const FMadInventory& B, FName Item) { return A.CountItem(Item) + B.CountItem(Item); };

	FMadInventory Bag(6);
	FMadInventory Crate(4);

	// Empty target: partial and whole moves.
	Bag.SetSlot(0, Stack(Plank, 7));
	TestTrue(TEXT("move 3 into an empty crate slot"), MoveSlot(Bag, 0, Crate, 0, 3, Defs) == EMadSlotMove::Moved);
	TestEqual(TEXT("4 left behind"), Bag.GetSlot(0).Count, 4);
	TestEqual(TEXT("3 arrived"), Crate.GetSlot(0).Count, 3);

	// Merge caps at max stack and leaves the remainder.
	Bag.SetSlot(1, Stack(Plank, 9));
	TestTrue(TEXT("merge onto the 3"), MoveSlot(Bag, 1, Crate, 0, 0, Defs) == EMadSlotMove::Merged);
	TestEqual(TEXT("crate stack full at 10"), Crate.GetSlot(0).Count, 10);
	TestEqual(TEXT("2 stay in the bag"), Bag.GetSlot(1).Count, 2);
	TestTrue(TEXT("a full matching stack accepts nothing"), MoveSlot(Bag, 1, Crate, 0, 0, Defs) == EMadSlotMove::Nothing);
	TestEqual(TEXT("planks conserved"), Total(Bag, Crate, Plank), 16);

	// Swap only on a whole-stack move.
	Crate.SetSlot(1, Stack(Rock, 30));
	TestTrue(TEXT("a partial drag onto a different item does nothing"), MoveSlot(Bag, 0, Crate, 1, 2, Defs) == EMadSlotMove::Nothing);
	TestTrue(TEXT("a whole-stack drag swaps"), MoveSlot(Bag, 0, Crate, 1, 0, Defs) == EMadSlotMove::Swapped);
	TestEqual(TEXT("rocks now in the bag"), Bag.GetSlot(0).Item, Rock);
	TestEqual(TEXT("planks now in the crate"), Crate.GetSlot(1).Item, Plank);

	// Tools never merge (durability), even with the same id.
	Bag.SetSlot(2, Pick);
	Crate.SetSlot(2, Pick);
	TestTrue(TEXT("two picks swap rather than merge"), MoveSlot(Bag, 2, Crate, 2, 0, Defs) == EMadSlotMove::Swapped);

	// Within one inventory, and guard rails.
	TestTrue(TEXT("same slot is nothing"), MoveSlot(Bag, 0, Bag, 0, 0, Defs) == EMadSlotMove::Nothing);
	TestTrue(TEXT("rearrange within the bag"), MoveSlot(Bag, 0, Bag, 5, 0, Defs) == EMadSlotMove::Moved);
	TestTrue(TEXT("out of range"), MoveSlot(Bag, 0, Crate, 9, 0, Defs) == EMadSlotMove::InvalidSlot);
	TestTrue(TEXT("empty source"), MoveSlot(Bag, 0, Crate, 3, 0, Defs) == EMadSlotMove::Nothing);

	// Quick move: tops up first, then empty slots, remainder stays.
	{
		FMadInventory Player(6);
		FMadInventory Box(2);
		Box.SetSlot(0, Stack(Plank, 8));
		Box.SetSlot(1, Stack(Rock, 1));
		Player.SetSlot(3, Stack(Plank, 25));

		TestEqual(TEXT("only 2 fit (top-up of the 8)"), QuickMove(Player, 3, Box, 0, 1, Defs), 2);
		TestEqual(TEXT("box planks full"), Box.GetSlot(0).Count, 10);
		TestEqual(TEXT("23 stay in the source slot"), Player.GetSlot(3).Count, 23);

		// Hotbar (0-1) to backpack (2-5) within one inventory.
		TestEqual(TEXT("quick move 23 planks into the backpack range"), QuickMove(Player, 3, Player, 0, 1, Defs), 20);
		TestEqual(TEXT("two full stacks in the hotbar range"), Player.GetSlot(0).Count + Player.GetSlot(1).Count, 20);
		TestEqual(TEXT("3 left in the source"), Player.GetSlot(3).Count, 3);
		TestEqual(TEXT("never quick-moves into its own slot"), QuickMove(Player, 3, Player, 3, 3, Defs), 0);
		TestEqual(TEXT("planks conserved through quick moves"), Total(Player, Box, Plank), 33);
	}

	// Sorting: tools first, plain stacks pooled into full stacks, empties last,
	// the hotbar range left alone, nothing gained or lost.
	{
		using MadFall::InventoryOps::SortRange;
		FMadInventory Pack(8);
		Pack.SetSlot(0, Stack(Rock, 3));      // "hotbar": outside the sorted range
		Pack.SetSlot(2, Stack(Plank, 7));
		Pack.SetSlot(3, Stack(Rock, 20));
		Pack.SetSlot(4, Stack(Plank, 6));
		FMadItemStack Worn = Pick;
		Worn.Durability = 3;
		Pack.SetSlot(5, Worn);
		Pack.SetSlot(7, Pick);
		FMadItemStack Unknown;
		Unknown.Item = FName(TEXT("gonemod:relic"));
		Unknown.Count = 1;
		Pack.SetSlot(6, Unknown);

		TestTrue(TEXT("sorting changes the pack"), SortRange(Pack, 1, 7, Defs));
		TestEqual(TEXT("hotbar untouched"), Pack.GetSlot(0).Count, 3);
		TestEqual(TEXT("the fresh pick first"), Pack.GetSlot(1).Durability, Pick.Durability);
		TestEqual(TEXT("the worn pick after it"), Pack.GetSlot(2).Durability, 3);
		TestTrue(TEXT("then planks, pooled to a full stack of 10"), Pack.GetSlot(3).Item == Plank && Pack.GetSlot(3).Count == 10);
		TestTrue(TEXT("and the remaining 3"), Pack.GetSlot(4).Item == Plank && Pack.GetSlot(4).Count == 3);
		TestTrue(TEXT("then rock (resources sort by name)"), Pack.GetSlot(5).Item == Rock && Pack.GetSlot(5).Count == 20);
		TestTrue(TEXT("unknown items last"), Pack.GetSlot(6).Item == Unknown.Item);
		TestTrue(TEXT("and the rest empty"), Pack.GetSlot(7).IsEmpty());
		TestEqual(TEXT("planks conserved"), Pack.CountItem(Plank), 13);
		TestEqual(TEXT("rock conserved"), Pack.CountItem(Rock), 23);
		TestFalse(TEXT("sorting again changes nothing"), SortRange(Pack, 1, 7, Defs));

		// Stacks over a patched max_stack cannot re-split into the slots there are: reordered only.
		FMadInventory Tight(2);
		FMadItemStack Over = Stack(Plank, 10);
		Over.Count = 25;
		Tight.SetSlot(0, Stack(Rock, 5));
		Tight.SetSlot(1, Over);
		SortRange(Tight, 0, 1, Defs);
		TestEqual(TEXT("an oversized stack keeps every item"), Tight.CountItem(Plank), 25);
		TestEqual(TEXT("and the rock"), Tight.CountItem(Rock), 5);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadCraftingTest,
	"MadFall.Items.Crafting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadCraftingTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);

	const FName Plank(TEXT("test:plank"));
	const FName Rock(TEXT("test:rock"));
	const FMadRecipeDefinition& PickRecipe = *Defs.FindRecipe(FName(TEXT("test:pick")));
	const FMadRecipeDefinition& PlankRecipe = *Defs.FindRecipe(FName(TEXT("test:planks")));

	// Two full slots and ingredients that leave both partly used: nowhere to put a pick.
	{
		FMadInventory Inventory(2);
		Inventory.Add(FMadItemStack::Make(*Defs.FindItem(Plank), 4), Defs);
		Inventory.Add(FMadItemStack::Make(*Defs.FindItem(Rock), 7), Defs);

		TestEqual(TEXT("max craftable is limited by rocks (7/3)"), MadFall::Crafting::MaxCraftable(Inventory, PickRecipe), 2);

		const FMadInventory Before = Inventory;
		TestTrue(TEXT("no space for the output"),
			MadFall::Crafting::Craft(Inventory, PickRecipe, Defs, NAME_None, 0) == EMadCraftResult::NoSpace);
		TestTrue(TEXT("a failed craft consumes nothing"), Inventory.GetSlots() == Before.GetSlots());
	}

	// Ingredients that empty a slot make room for the output.
	{
		FMadInventory Inventory(2);
		Inventory.Add(FMadItemStack::Make(*Defs.FindItem(Plank), 2), Defs);
		Inventory.Add(FMadItemStack::Make(*Defs.FindItem(Rock), 7), Defs);

		TestTrue(TEXT("craft into the slot the planks vacated"),
			MadFall::Crafting::Craft(Inventory, PickRecipe, Defs, NAME_None, 0) == EMadCraftResult::Ok);
		TestEqual(TEXT("planks consumed"), Inventory.CountItem(Plank), 0);
		TestEqual(TEXT("rocks consumed"), Inventory.CountItem(Rock), 4);
		TestEqual(TEXT("pick crafted"), Inventory.CountItem(FName(TEXT("test:pick"))), 1);
		TestTrue(TEXT("second pick: out of planks"),
			MadFall::Crafting::Craft(Inventory, PickRecipe, Defs, NAME_None, 0) == EMadCraftResult::MissingIngredients);
	}

	FMadInventory Inventory(4);
	Inventory.Add(FMadItemStack::Make(*Defs.FindItem(Rock), 7), Defs);

	TestTrue(TEXT("station required"),
		MadFall::Crafting::Craft(Inventory, PlankRecipe, Defs, NAME_None, 5) == EMadCraftResult::WrongStation);
	TestTrue(TEXT("level required"),
		MadFall::Crafting::Craft(Inventory, PlankRecipe, Defs, FName(TEXT("test:bench")), 1) == EMadCraftResult::LevelTooLow);
	return true;
}

// ===========================================================================
// Loot
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadLootTest,
	"MadFall.Items.Loot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadLootTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);
	const FMadLootTableDefinition& Outer = *Defs.FindLootTable(FName(TEXT("test:loot/outer")));

	// Determinism.
	TArray<FMadItemStack> A, B;
	FRandomStream StreamA(1234), StreamB(1234);
	MadFall::Loot::Roll(Outer, Defs, FMadLootContext(), StreamA, A);
	MadFall::Loot::Roll(Outer, Defs, FMadLootContext(), StreamB, B);
	TestTrue(TEXT("same seed, same loot"), A == B);

	// Tier gating and tier rolls, over many seeds.
	int32 IronAtTier1 = 0, IronAtTier4 = 0, StacksAtTier1 = 0, StacksAtTier4 = 0;
	bool bDurabilityRight = true;
	for (int32 Seed = 0; Seed < 200; ++Seed)
	{
		TArray<FMadItemStack> Low, High;
		FRandomStream R1(Seed), R4(Seed);
		MadFall::Loot::Roll(Outer, Defs, FMadLootContext{ 1, 0 }, R1, Low);
		MadFall::Loot::Roll(Outer, Defs, FMadLootContext{ 4, 0 }, R4, High);

		for (const FMadItemStack& S : Low)
		{
			IronAtTier1 += S.Item == FName(TEXT("test:iron_pick")) ? 1 : 0;
			if (S.Item == FName(TEXT("test:pick"))) { bDurabilityRight &= (S.Durability == 5); }
			StacksAtTier1 += 1;
		}
		for (const FMadItemStack& S : High)
		{
			IronAtTier4 += S.Item == FName(TEXT("test:iron_pick")) ? 1 : 0;
			StacksAtTier4 += 1;
		}
	}

	TestEqual(TEXT("tier-gated entry never appears at tier 1"), IronAtTier1, 0);
	TestTrue(TEXT("tier-gated entry dominates at tier 4"), IronAtTier4 > 200);
	TestTrue(TEXT("loot durability range applied (0.5 of 10)"), bDurabilityRight);
	TestTrue(TEXT("rolls_per_tier gives more items at tier 4"), StacksAtTier4 > StacksAtTier1);
	return true;
}

// ===========================================================================
// Harvest
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadHarvestTest,
	"MadFall.Items.Harvest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadHarvestTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);

	FMadBlockDefinitionData Ore;
	Ore.Id = FName(TEXT("test:ore"));
	Ore.HarvestToolTags = { FName(TEXT("tool.pickaxe")) };
	Ore.HarvestTier = 2;
	Ore.Resistances.Add(FName(TEXT("test:blunt")), 1.0f);
	Ore.Resistances.Add(FName(TEXT("test:pierce")), 0.1f);

	FMadBlockDefinitionData Dirt;
	Dirt.Id = FName(TEXT("test:dirt"));

	FMadItemStack Pick = FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:pick"))), 1);
	FMadItemStack IronPick = FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:iron_pick"))), 1);
	FMadItemStack Plank = FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:plank"))), 1);

	FMadToolHit Hit = MadFall::Harvest::ComputeHit(nullptr, Defs, Dirt);
	TestTrue(TEXT("bare hands harvest untagged blocks"), Hit.bHarvests && !Hit.bPenalised);
	TestEqual(TEXT("bare hands damage"), Hit.Amount, 8.0f);

	Hit = MadFall::Harvest::ComputeHit(&Plank, Defs, Ore);
	TestTrue(TEXT("a plank is the wrong tool for ore"), Hit.bPenalised && !Hit.bHarvests);
	TestEqual(TEXT("wrong tool deals a quarter"), Hit.Amount, 2.0f);

	Hit = MadFall::Harvest::ComputeHit(&Pick, Defs, Ore);
	TestTrue(TEXT("tier-1 pick on tier-2 ore: penalised, no drops"), Hit.bPenalised && !Hit.bHarvests);
	TestEqual(TEXT("low tier deals half"), Hit.Amount, 10.0f);

	Hit = MadFall::Harvest::ComputeHit(&IronPick, Defs, Ore);
	TestTrue(TEXT("tier-3 pick harvests"), Hit.bHarvests && !Hit.bPenalised);
	TestEqual(TEXT("best type after resistance is blunt (20) over pierce (50 x 0.1)"), Hit.DamageType, FName(TEXT("test:blunt")));

	IronPick.Mods.Add(FName(TEXT("test:grip")));
	Hit = MadFall::Harvest::ComputeHit(&IronPick, Defs, Ore);
	TestEqual(TEXT("damage mod doubles"), Hit.Amount, 40.0f);

	// Durability: 10 uses without mods; a 2x mod roughly doubles it on average.
	FRandomStream Random(7);
	FMadItemStack Wearing = Pick;
	int32 Uses = 0;
	while (!Wearing.IsEmpty() && Uses < 100)
	{
		MadFall::Harvest::ConsumeDurability(Wearing, Defs, Random);
		++Uses;
	}
	TestEqual(TEXT("a 10-durability pick breaks on the 10th use"), Uses, 10);

	int32 ModdedUses = 0;
	for (int32 Trial = 0; Trial < 50; ++Trial)
	{
		FMadItemStack Modded = Pick;
		Modded.Mods.Add(FName(TEXT("test:grip")));
		while (!Modded.IsEmpty() && ModdedUses < 100000)
		{
			MadFall::Harvest::ConsumeDurability(Modded, Defs, Random);
			++ModdedUses;
		}
	}
	const float Average = ModdedUses / 50.0f;
	TestTrue(FString::Printf(TEXT("durability mod ~doubles life (average %.1f uses)"), Average), Average > 16.0f && Average < 24.0f);
	return true;
}

// ===========================================================================
// Survival
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSurvivalModelTest,
	"MadFall.Survival.Model",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSurvivalModelTest::RunTest(const FString& Parameters)
{
	const FMadSurvivalTuning Tuning;

	// Resting in comfort for 10 minutes: food and water drain at their rates, nothing hurts.
	{
		FMadSurvivalStats S;
		S.Health = 5.0f;
		const FMadSurvivalStepResult R = MadFall::Survival::Step(S, FMadSurvivalEnvironment(), Tuning, 600.0f);
		TestEqual(TEXT("food after 10 min"), S.Food, 95.0f, 0.01f);
		TestEqual(TEXT("water after 10 min"), S.Water, 92.0f, 0.01f);
		TestEqual(TEXT("no damage"), R.TotalDamage(), 0.0f);
		TestEqual(TEXT("well fed heals 0.15/s"), S.Health, 5.0f + 90.0f, 0.5f);
	}

	// Frame-rate independence: one 60 s step equals 3600 steps of 1/60 s.
	{
		FMadSurvivalStats Coarse, Fine;
		Coarse.Food = Fine.Food = 30.0f;
		Coarse.Infection = Fine.Infection = 70.0f;
		FMadSurvivalEnvironment Cold;
		Cold.AmbientTemperature = -20.0f;

		MadFall::Survival::Step(Coarse, Cold, Tuning, 60.0f);
		for (int32 I = 0; I < 3600; ++I)
		{
			MadFall::Survival::Step(Fine, Cold, Tuning, 1.0f / 60.0f);
		}
		TestEqual(TEXT("core temperature agrees"), Coarse.CoreTemperature, Fine.CoreTemperature, 0.05f);
		TestEqual(TEXT("health agrees"), Coarse.Health, Fine.Health, 0.5f);
		TestEqual(TEXT("infection agrees"), Coarse.Infection, Fine.Infection, 0.05f);
	}

	// Starvation kills, and reports why.
	{
		FMadSurvivalStats S;
		S.Food = 0.0f;
		S.Health = 10.0f;
		const FMadSurvivalStepResult R = MadFall::Survival::Step(S, FMadSurvivalEnvironment(), Tuning, 60.0f);
		TestTrue(TEXT("starved to death"), S.IsDead());
		TestTrue(TEXT("damage attributed to starvation"), R.StarvationDamage >= 10.0f - 0.01f && R.DehydrationDamage == 0.0f);
	}

	// Cold is a countdown, insulation pushes it back.
	{
		FMadSurvivalEnvironment Blizzard;
		Blizzard.AmbientTemperature = -30.0f;
		FMadSurvivalStats Bare, Dressed;
		MadFall::Survival::Step(Bare, Blizzard, Tuning, 600.0f);
		Blizzard.ColdInsulation = 45.0f;
		MadFall::Survival::Step(Dressed, Blizzard, Tuning, 600.0f);
		TestTrue(TEXT("bare survivor's core drops"), Bare.CoreTemperature < 34.0f);
		TestEqual(TEXT("insulated survivor stays at 37"), Dressed.CoreTemperature, 37.0f, 0.01f);
		TestTrue(TEXT("hypothermia hurts"), Bare.Health < 100.0f);
	}

	// Sprinting drains stamina; exertion blocks regeneration.
	{
		FMadSurvivalEnvironment Sprint;
		Sprint.bSprinting = true;
		FMadSurvivalStats S;
		MadFall::Survival::Step(S, Sprint, Tuning, 5.0f);
		TestEqual(TEXT("5 s sprint costs 50 stamina"), S.Stamina, 50.0f, 0.01f);

		FMadSurvivalEnvironment Swing;
		Swing.bExerting = true;
		MadFall::Survival::Step(S, Swing, Tuning, 5.0f);
		TestEqual(TEXT("no regen while exerting"), S.Stamina, 50.0f, 0.01f);
	}

	// Effects.
	{
		FMadSurvivalStats S;
		S.Food = 90.0f;
		S.Infection = 30.0f;
		TArray<FName> Unknown;
		MadFall::Survival::ApplyEffects(S, { { FName(TEXT("food")), 35.0f }, { FName(TEXT("infection")), -40.0f }, { FName(TEXT("mana")), 5.0f } }, &Unknown);
		TestEqual(TEXT("food clamps at 100"), S.Food, 100.0f);
		TestEqual(TEXT("infection clamps at 0"), S.Infection, 0.0f);
		TestEqual(TEXT("unknown effect reported"), Unknown.Num(), 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadRepairAndModsTest,
	"MadFall.Items.RepairAndMods",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadRepairAndModsTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);

	const FName Rock(TEXT("test:rock"));
	const FName Grip(TEXT("test:grip"));

	FMadInventory Inventory(4);
	FMadItemStack Pick = FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:pick"))), 1);
	Pick.Durability = 2;
	Inventory.SetSlot(0, Pick);
	Inventory.Add(FMadItemStack::Make(*Defs.FindItem(Rock), 3), Defs);

	// Repair: 50% of 10 = 5, costs 2 rocks.
	TestTrue(TEXT("repair ok"), MadFall::Items::Repair(Inventory, 0, Defs) == EMadItemActionResult::Ok);
	TestEqual(TEXT("durability restored by half of max"), Inventory.GetSlot(0).Durability, 7);
	TestEqual(TEXT("two rocks spent"), Inventory.CountItem(Rock), 1);

	const FMadInventory BeforeFail = Inventory;
	TestTrue(TEXT("second repair lacks rocks"), MadFall::Items::Repair(Inventory, 0, Defs) == EMadItemActionResult::MissingMaterials);
	TestTrue(TEXT("failed repair changes nothing"), Inventory.GetSlots() == BeforeFail.GetSlots());

	Inventory.Add(FMadItemStack::Make(*Defs.FindItem(Rock), 10), Defs);
	TestTrue(TEXT("repair again"), MadFall::Items::Repair(Inventory, 0, Defs) == EMadItemActionResult::Ok);
	TestEqual(TEXT("capped at max"), Inventory.GetSlot(0).Durability, 10);
	TestTrue(TEXT("nothing to repair at full"), MadFall::Items::Repair(Inventory, 0, Defs) == EMadItemActionResult::NothingToDo);
	TestTrue(TEXT("rocks are not a tool"), MadFall::Items::Repair(Inventory, 1, Defs) == EMadItemActionResult::NotATool);

	// Mods: one slot on the pick.
	TestTrue(TEXT("no grip in the backpack yet"), MadFall::Items::InstallMod(Inventory, 0, Grip, Defs) == EMadItemActionResult::MissingMaterials);
	Inventory.Add(FMadItemStack::Make(*Defs.FindItem(Grip), 2), Defs);
	TestTrue(TEXT("install ok"), MadFall::Items::InstallMod(Inventory, 0, Grip, Defs) == EMadItemActionResult::Ok);
	TestEqual(TEXT("mod recorded on the stack"), Inventory.GetSlot(0).Mods.Num(), 1);
	TestEqual(TEXT("one grip consumed"), Inventory.CountItem(Grip), 1);
	TestTrue(TEXT("slot is full"), MadFall::Items::InstallMod(Inventory, 0, Grip, Defs) == EMadItemActionResult::NoFreeSlot);
	TestTrue(TEXT("a plank is not a mod"), MadFall::Items::InstallMod(Inventory, 0, FName(TEXT("test:plank")), Defs) == EMadItemActionResult::IncompatibleMod);

	// Removing it puts the grip back whole; a full backpack refuses without change.
	TestTrue(TEXT("remove ok"), MadFall::Items::RemoveMod(Inventory, 0, INDEX_NONE, Defs) == EMadItemActionResult::Ok);
	TestEqual(TEXT("the tool has no mods"), Inventory.GetSlot(0).Mods.Num(), 0);
	TestEqual(TEXT("both grips back in the backpack"), Inventory.CountItem(Grip), 2);
	TestTrue(TEXT("nothing left to remove"), MadFall::Items::RemoveMod(Inventory, 0, INDEX_NONE, Defs) == EMadItemActionResult::NothingToDo);
	{
		FMadInventory Full(1);
		FMadItemStack Modded = Inventory.GetSlot(0);
		Modded.Mods.Add(Grip);
		Full.SetSlot(0, Modded);
		TestTrue(TEXT("no room: refused"), MadFall::Items::RemoveMod(Full, 0, INDEX_NONE, Defs) == EMadItemActionResult::NoFreeSlot);
		TestEqual(TEXT("and the mod stays installed"), Full.GetSlot(0).Mods.Num(), 1);
		TestTrue(TEXT("a plank slot is not a tool"), MadFall::Items::RemoveMod(Inventory, 5, INDEX_NONE, Defs) == EMadItemActionResult::NotATool);
	}
	TestTrue(TEXT("re-install for what follows"), MadFall::Items::InstallMod(Inventory, 0, Grip, Defs) == EMadItemActionResult::Ok);

	// Timed crafting takes ingredients without adding output.
	FMadInventory Crafter(4);
	Crafter.Add(FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:plank"))), 2), Defs);
	Crafter.Add(FMadItemStack::Make(*Defs.FindItem(Rock), 3), Defs);
	const FMadRecipeDefinition& PickRecipe = *Defs.FindRecipe(FName(TEXT("test:pick")));
	TestTrue(TEXT("ingredients taken"), MadFall::Crafting::TakeIngredients(Crafter, PickRecipe, Defs, NAME_None, 0) == EMadCraftResult::Ok);
	TestTrue(TEXT("backpack is now empty, no output yet"), Crafter.IsEmpty());
	TestTrue(TEXT("second take fails"), MadFall::Crafting::TakeIngredients(Crafter, PickRecipe, Defs, NAME_None, 0) == EMadCraftResult::MissingIngredients);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadRecipeListTest,
	"MadFall.Items.RecipeList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadRecipeListTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;
	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);

	const FMadRecipeDefinition& PickRecipe = *Defs.FindRecipe(FName(TEXT("test:pick")));
	const FMadRecipeDefinition& PlanksRecipe = *Defs.FindRecipe(FName(TEXT("test:planks")));
	TestTrue(TEXT("a tool is under Tools"), MadFall::Crafting::CategoryOf(PickRecipe, Defs) == EMadRecipeCategory::Tools);
	TestTrue(TEXT("a plain item is under Other"), MadFall::Crafting::CategoryOf(PlanksRecipe, Defs) == EMadRecipeCategory::Materials);

	FMadInventory Backpack(8);
	Backpack.Add(FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:plank"))), 4), Defs);
	Backpack.Add(FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:rock"))), 7), Defs);

	bool bBench = false;
	auto HasStation = [&bBench](FName Station) { return Station.IsNone() || bBench; };

	TArray<FMadRecipeRow> Rows;
	MadFall::Crafting::ListRecipes(Defs, Backpack, 1, HasStation, EMadRecipeCategory::All, false, Rows);
	TestEqual(TEXT("both recipes listed"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		TestTrue(TEXT("what can be made now comes first"), Rows[0].Recipe == &PickRecipe && Rows[0].CanCraftNow());
		TestEqual(TEXT("two picks' worth of planks"), Rows[0].Craftable, 2);
		TestFalse(TEXT("planks need the bench"), Rows[1].bStation);
		TestFalse(TEXT("and level 2"), Rows[1].bLevel);
		TestEqual(TEXT("ingredients alone would make seven"), Rows[1].Craftable, 7);
	}

	MadFall::Crafting::ListRecipes(Defs, Backpack, 1, HasStation, EMadRecipeCategory::Tools, false, Rows);
	TestTrue(TEXT("the Tools tab has only the pick"), Rows.Num() == 1 && Rows[0].Recipe == &PickRecipe);

	MadFall::Crafting::ListRecipes(Defs, Backpack, 1, HasStation, EMadRecipeCategory::All, true, Rows);
	TestTrue(TEXT("craftable only hides the planks"), Rows.Num() == 1 && Rows[0].Recipe == &PickRecipe);

	// At the bench, at level 2, both can be made: then they sort by name.
	bBench = true;
	MadFall::Crafting::ListRecipes(Defs, Backpack, 2, HasStation, EMadRecipeCategory::All, true, Rows);
	TestEqual(TEXT("both craftable"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		TestTrue(TEXT("sorted by output name"), Defs.GetItemName(Rows[0].Recipe->Output.Item) <= Defs.GetItemName(Rows[1].Recipe->Output.Item));
	}

	FMadInventory Empty(4);
	MadFall::Crafting::ListRecipes(Defs, Empty, 5, HasStation, EMadRecipeCategory::Food, false, Rows);
	TestEqual(TEXT("an empty tab is empty"), Rows.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadItemTooltipTest,
	"MadFall.Items.Tooltips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadItemTooltipTest::RunTest(const FString& Parameters)
{
	using namespace MadItemTests;
	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	BuildDefinitions(Defs, Errors);

	const FMadItemDefinition& PickDef = *Defs.FindItem(FName(TEXT("test:pick")));
	const FMadItemDefinition& GripDef = *Defs.FindItem(FName(TEXT("test:grip")));
	const FMadItemDefinition& FoodDef = *Defs.FindItem(FName(TEXT("test:food")));
	const FMadItemDefinition& PlankDef = *Defs.FindItem(FName(TEXT("test:plank")));

	auto Has = [](const TArray<FString>& Lines, const FString& Text)
	{
		return Lines.ContainsByPredicate([&Text](const FString& Line) { return Line.StartsWith(Text); });
	};

	TArray<FString> Lines;
	FMadItemStack Pick = FMadItemStack::Make(PickDef, 1);
	MadFall::Items::DescribeStack(Pick, Defs, Lines);
	TestEqual(TEXT("the name comes first"), Lines.Num() > 0 ? Lines[0] : FString(), Defs.GetItemName(PickDef.Id));
	TestTrue(TEXT("damage, without the damage type's namespace"), Has(Lines, TEXT("Damage 20 blunt")));
	TestTrue(TEXT("durability out of maximum"), Has(Lines, TEXT("Durability 10 / 10")));
	TestTrue(TEXT("mod slots"), Has(Lines, TEXT("Mods 0 / 1")));
	TestTrue(TEXT("harvest tier"), Has(Lines, TEXT("Harvest tier 1")));

	Pick.Mods.Add(GripDef.Id);
	MadFall::Items::DescribeStack(Pick, Defs, Lines);
	TestTrue(TEXT("installed mods change the damage shown"), Has(Lines, TEXT("Damage 40 blunt")));
	TestTrue(TEXT("and are listed by name"), Has(Lines, FString::Printf(TEXT("Mods 1 / 1: %s"), *Defs.GetItemName(GripDef.Id))));

	MadFall::Items::DescribeStack(FMadItemStack::Make(FoodDef, 3), Defs, Lines);
	TestTrue(TEXT("stack count with the name"), Lines.Num() > 0 && Lines[0].EndsWith(TEXT("x3")));
	TestTrue(TEXT("consumable effects"), Has(Lines, TEXT("Right-click to use: +30 food")));
	TestTrue(TEXT("stack size"), Has(Lines, TEXT("Stacks to 5")));

	MadFall::Items::DescribeStack(FMadItemStack::Make(GripDef, 1), Defs, Lines);
	TestTrue(TEXT("mod bonuses as percentages"), Has(Lines, TEXT("Item mod: damage +100%, durability +100%")));
	TestTrue(TEXT("what the mod fits and how to install it"), Has(Lines, TEXT("Drop onto a tool to install")));

	MadFall::Items::DescribeStack(FMadItemStack{ FName(TEXT("gonemod:laser")), 1, -1, {} }, Defs, Lines);
	TestEqual(TEXT("an item from a missing mod: id and a note"), Lines.Num(), 2);
	TestTrue(TEXT("saying it is kept"), Lines.Num() == 2 && Lines[1].Contains(TEXT("not installed")));

	MadFall::Items::DescribeStack(FMadItemStack(), Defs, Lines);
	TestEqual(TEXT("an empty slot has no tooltip"), Lines.Num(), 0);

	// Which drops install a mod rather than swap.
	const FMadItemStack Grip = FMadItemStack::Make(GripDef, 1);
	TestTrue(TEXT("a mod onto a tool installs"), MadFall::Items::IsModInstallDrop(Grip, FMadItemStack::Make(PickDef, 1), Defs));
	TestFalse(TEXT("a tool onto a mod swaps"), MadFall::Items::IsModInstallDrop(FMadItemStack::Make(PickDef, 1), Grip, Defs));
	TestFalse(TEXT("a mod onto a plank swaps"), MadFall::Items::IsModInstallDrop(Grip, FMadItemStack::Make(PlankDef, 1), Defs));
	TestFalse(TEXT("a mod onto an empty slot moves"), MadFall::Items::IsModInstallDrop(Grip, FMadItemStack(), Defs));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadDefinitionPatches.h"
#include "MadDefinitionSources.h"
#include "MadGameplayDefinitions.h"
#include "MadLocalization.h"
#include "MadModManager.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadInstalledModsTests
{
	/** Messages prefixed "warning:" are advisory (unknown fields and the like); anything else is a real error. */
	int32 ReportErrors(FAutomationTestBase& Test, const TCHAR* Kind, const TArray<FMadDefinitionError>& Errors)
	{
		int32 Real = 0;
		for (const FMadDefinitionError& Error : Errors)
		{
			if (Error.Message.StartsWith(TEXT("warning:")))
			{
				Test.AddInfo(FString::Printf(TEXT("%s: %s"), Kind, *Error.ToString()));
				continue;
			}
			Test.AddError(FString::Printf(TEXT("%s: %s"), Kind, *Error.ToString()));
			++Real;
		}
		return Real;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadInstalledModsLoadCleanlyTest,
	"MadFall.Mods.InstalledModsLoadCleanly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadInstalledModsLoadCleanlyTest::RunTest(const FString& Parameters)
{
	using namespace MadInstalledModsTests;

	// Every mod in Mods/ - the examples and the test mods - through every
	// definition kind, into private registries so each message is visible here.
	const FMadModManager& Manager = MadFall::GetModManager();
	const FMadModResolution& Resolution = Manager.GetResolution();
	for (const TPair<FName, EMadModStatus>& Pair : Resolution.Status)
	{
		TestTrue(*FString::Printf(TEXT("mod %s is enabled"), *Pair.Key.ToString()), Pair.Value == EMadModStatus::Enabled);
	}
	for (const FMadModIssue& Issue : Resolution.Issues)
	{
		if (Issue.bFatal)
		{
			AddError(FString::Printf(TEXT("mod issue: %s"), *Issue.ToString()));
		}
		else
		{
			AddInfo(FString::Printf(TEXT("mod warning: %s"), *Issue.ToString()));
		}
	}
	TestTrue(TEXT("at least the eight shipped mods are installed"), Resolution.LoadOrder.Num() >= 8);

	TArray<FMadDefinitionError> PatchErrors;
	FMadPatchSet Patches;
	Patches.LoadFromSources(PatchErrors);
	TestEqual(TEXT("patch files parse"), ReportErrors(*this, TEXT("patches"), PatchErrors), 0);

	TArray<FMadDefinitionError> BlockErrors;
	FMadBlockRegistry Blocks;
	Blocks.BeginLoad();
	MadFall::Definitions::ForEachSource(TEXT("blocks"), [&](const FString& Dir, FName ModId) { Blocks.AddFromDirectory(Dir, ModId, BlockErrors); });
	Blocks.ApplyPatches(Patches, BlockErrors);
	Blocks.FinishLoad(BlockErrors);
	TestEqual(TEXT("blocks load cleanly"), ReportErrors(*this, TEXT("blocks"), BlockErrors), 0);

	TArray<FMadDefinitionError> BiomeErrors;
	FMadBiomeRegistry Biomes;
	Biomes.BeginLoad();
	MadFall::Definitions::ForEachSource(TEXT("biomes"), [&](const FString& Dir, FName ModId) { Biomes.AddFromDirectory(Dir, ModId, BiomeErrors); });
	Biomes.ApplyPatches(Patches, BiomeErrors);
	Biomes.FinishLoad(BiomeErrors);
	TestEqual(TEXT("biomes load cleanly"), ReportErrors(*this, TEXT("biomes"), BiomeErrors), 0);

	TArray<FMadDefinitionError> DefErrors;
	FMadGameplayDefinitions Defs;
	Defs.BeginLoad();
	MadFall::Definitions::ForEachSource(TEXT("items"), [&](const FString& Dir, FName ModId) { Defs.AddItemsFromDirectory(Dir, ModId, DefErrors); });
	MadFall::Definitions::ForEachSource(TEXT("recipes"), [&](const FString& Dir, FName ModId) { Defs.AddRecipesFromDirectory(Dir, ModId, DefErrors); });
	MadFall::Definitions::ForEachSource(TEXT("loot"), [&](const FString& Dir, FName ModId) { Defs.AddLootFromDirectory(Dir, ModId, DefErrors); });
	MadFall::Definitions::ForEachSource(TEXT("zombies"), [&](const FString& Dir, FName ModId) { Defs.AddZombiesFromDirectory(Dir, ModId, DefErrors); });
	MadFall::Definitions::ForEachSource(TEXT("tuning"), [&](const FString& Dir, FName ModId) { Defs.AddTuningFromDirectory(Dir, ModId, DefErrors); });
	MadFall::Definitions::ForEachSource(TEXT("perks"), [&](const FString& Dir, FName ModId) { Defs.AddPerksFromDirectory(Dir, ModId, DefErrors); });
	Defs.ApplyPatches(Patches, DefErrors);
	Defs.FinishLoad(&Blocks, DefErrors);
	TestEqual(TEXT("items, recipes, loot, zombies, tuning and perks load cleanly"), ReportErrors(*this, TEXT("gameplay"), DefErrors), 0);

	TArray<FMadDefinitionError> SurfaceErrors;
	FMadSurfaceRegistry Surfaces;
	Surfaces.BeginLoad();
	MadFall::Definitions::ForEachSource(TEXT("surfaces"), [&](const FString& Dir, FName ModId) { Surfaces.AddFromDirectory(Dir, ModId, SurfaceErrors); });
	Surfaces.FinishLoad(&Patches, SurfaceErrors);
	TestEqual(TEXT("surfaces load cleanly"), ReportErrors(*this, TEXT("surfaces"), SurfaceErrors), 0);

	TArray<FMadDefinitionError> StringErrors;
	FMadStringTable Strings;
	MadFall::Definitions::ForEachSource(TEXT("strings"), [&](const FString& Dir, FName ModId) { Strings.AddFromDirectory(Dir, ModId, StringErrors); });
	TestEqual(TEXT("strings load cleanly"), ReportErrors(*this, TEXT("strings"), StringErrors), 0);

	// Cross-checks a player would hit as a visible bug.
	for (const FMadBlockEntry& Entry : Blocks.GetEntries())
	{
		const FMadBlockDefinitionData& Block = Entry.Definition;
		if (Entry.bUnresolved)
		{
			continue;
		}
		if (!Block.MaterialClass.IsNone())
		{
			TestNotNull(*FString::Printf(TEXT("%s: material class %s has a surface"), *Block.Id.ToString(), *Block.MaterialClass.ToString()),
				Surfaces.Find(Block.MaterialClass));
		}
		if (!Block.DebrisOnCollapse.IsNone())
		{
			TestTrue(*FString::Printf(TEXT("%s: debris block %s exists"), *Block.Id.ToString(), *Block.DebrisOnCollapse.ToString()),
				Blocks.IsRegistered(Block.DebrisOnCollapse));
		}
		if (Block.DisplayName.StartsWith(TEXT("@")))
		{
			TestNotNull(*FString::Printf(TEXT("%s: %s has English text"), *Block.Id.ToString(), *Block.DisplayName), Strings.Find(Block.DisplayName.Mid(1), TEXT("en")));
		}
	}
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
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadTestModsBehaveTest,
	"MadFall.Mods.TestModsBehave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadTestModsBehaveTest::RunTest(const FString& Parameters)
{
	// Each test mod exists to exercise one part of the mod system; this checks
	// that part actually took effect in the live, global registries.
	const FMadModResolution& Resolution = MadFall::GetModManager().GetResolution();
	for (const TCHAR* Id : { TEXT("survival_tweaks"), TEXT("zombie_variety"), TEXT("builders_pack"), TEXT("field_medic"), TEXT("fortifications") })
	{
		if (!TestTrue(*FString::Printf(TEXT("%s is enabled"), Id), Resolution.IsEnabled(FName(Id))))
		{
			AddInfo(MadFall::GetModManager().Describe());
			return false;
		}
	}

	const FMadGameplayDefinitions& Defs = MadFall::GetGameplayDefinitions();
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();

	// survival_tweaks: tuning and perk patches, a new perk.
	if (const FMadTuningDefinition* Tuning = Defs.FindTuning(FName(TEXT("madfall:survival"))); TestNotNull(TEXT("survival tuning"), Tuning))
	{
		TestEqual(TEXT("survival_tweaks patched food drain"), Tuning->Values.FindRef(FName(TEXT("food_per_minute"))), 0.375f);
	}
	if (const FMadPerkDefinition* Miner = Defs.FindPerk(FName(TEXT("madfall:miner"))); TestNotNull(TEXT("miner perk"), Miner))
	{
		TestEqual(TEXT("survival_tweaks patched miner rank 1"), Miner->Ranks[0].Modifiers.FindRef(FName(TEXT("mining_damage"))), 1.25f);
	}
	TestNotNull(TEXT("new perk"), Defs.FindPerk(FName(TEXT("survival_tweaks:iron_stomach"))));

	// zombie_variety: new zombies in existing spawn groups, with their own loot.
	TArray<const FMadZombieDefinition*> Horde;
	Defs.GetZombiesInGroup(FName(TEXT("madfall:zombies/horde")), 100, Horde);
	for (const TCHAR* Id : { TEXT("zombie_variety:crawler"), TEXT("zombie_variety:bloater"), TEXT("zombie_variety:night_stalker") })
	{
		TestTrue(*FString::Printf(TEXT("%s joins horde nights"), Id), Horde.ContainsByPredicate([Id](const FMadZombieDefinition* Z) { return Z->Id == FName(Id); }));
	}
	TestNotNull(TEXT("crawler loot table"), Defs.FindLootTable(FName(TEXT("zombie_variety:loot/crawler"))));

	// builders_pack: blocks with their own material classes and surfaces, items, patched loot.
	const FName Brick(TEXT("builders_pack:brick_wall"));
	if (TestTrue(TEXT("brick wall registered"), Blocks.IsRegistered(Brick)))
	{
		const FMadBlockDefinitionData* Def = Blocks.FindDefinition(Blocks.ResolveRuntimeId(Brick));
		TestEqual(TEXT("brick has its own material class"), Def->MaterialClass, FName(TEXT("builders_pack:brick")));
		TestNotNull(TEXT("brick surface"), MadFall::GetSurfaces().Find(Def->MaterialClass));
		TestNotNull(TEXT("brick wall has a placeable item"), Defs.FindItemForBlock(Brick));
	}
	const FName FencePost(TEXT("builders_pack:fence_post"));
	if (TestTrue(TEXT("fence post registered"), Blocks.IsRegistered(FencePost)))
	{
		const FMadBlockDefinitionData* Def = Blocks.FindDefinition(Blocks.ResolveRuntimeId(FencePost));
		TestEqual(TEXT("fence post is a model block"), Def->ShapeKind, EMadBlockShapeKind::Model);
		TestTrue(TEXT("fence post is thin"), Def->MeshScale.Equals(FVector(0.3, 0.3, 1.0)));
		TestNotNull(TEXT("fence post has a placeable item"), Defs.FindItemForBlock(FencePost));
	}
	if (const FMadLootTableDefinition* Cabin = Defs.FindLootTable(FName(TEXT("madfall:loot/cabin_supplies"))); TestNotNull(TEXT("cabin loot"), Cabin))
	{
		TestTrue(TEXT("builders_pack added clay to cabin loot"), Cabin->Entries.ContainsByPredicate([](const FMadLootEntry& E) { return E.Item == FName(TEXT("builders_pack:clay")); }));
	}

	// field_medic: consumables, a weapon with a mod, loot patches, German text.
	if (const FMadItemDefinition* Kit = Defs.FindItem(FName(TEXT("field_medic:first_aid_kit"))); TestNotNull(TEXT("first aid kit"), Kit))
	{
		TestEqual(TEXT("first aid kit heals"), Kit->Consumable.Effects.FindRef(FName(TEXT("health"))), 60.0f);
	}
	TestTrue(TEXT("machete is a weapon with mod slots"), Defs.FindItem(FName(TEXT("field_medic:steel_machete"))) != nullptr
		&& Defs.FindItem(FName(TEXT("field_medic:steel_machete")))->Tool.ModSlots == 2);
	if (const FMadLootTableDefinition* Cabinet = Defs.FindLootTable(FName(TEXT("madfall:loot/medical_cabinet"))); TestNotNull(TEXT("medical cabinet loot"), Cabinet))
	{
		TestTrue(TEXT("field_medic nested its kit table into medical cabinets"), Cabinet->Entries.ContainsByPredicate([](const FMadLootEntry& E) { return E.Table == FName(TEXT("field_medic:loot/field_kit")); }));
	}
	TestNotNull(TEXT("field_medic ships German"), MadFall::GetStrings().Find(TEXT("field_medic.items.first_aid_kit"), TEXT("de")));

	// fortifications: dependency ordering, cross-mod inheritance and a cross-mod patch.
	int32 BuildersIndex = INDEX_NONE;
	int32 FortIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Resolution.LoadOrder.Num(); ++Index)
	{
		if (Resolution.LoadOrder[Index].Id == FName(TEXT("builders_pack"))) { BuildersIndex = Index; }
		if (Resolution.LoadOrder[Index].Id == FName(TEXT("fortifications"))) { FortIndex = Index; }
	}
	TestTrue(TEXT("builders_pack loads before the mod that depends on it"), BuildersIndex != INDEX_NONE && BuildersIndex < FortIndex);

	const FName Reinforced(TEXT("fortifications:reinforced_brick"));
	if (TestTrue(TEXT("reinforced brick registered"), Blocks.IsRegistered(Reinforced)))
	{
		const FMadBlockDefinitionData* Def = Blocks.FindDefinition(Blocks.ResolveRuntimeId(Reinforced));
		TestEqual(TEXT("inherits its material class from another mod's block"), Def->MaterialClass, FName(TEXT("builders_pack:brick")));
		TestEqual(TEXT("and overrides hardness"), Def->Hardness, 900.0f);
	}
	if (const FMadRecipeDefinition* Sheet = Defs.FindRecipe(FName(TEXT("builders_pack:metal_sheet"))); TestNotNull(TEXT("metal sheet recipe"), Sheet))
	{
		TestEqual(TEXT("fortifications patched builders_pack's recipe cost"), Sheet->Ingredients[0].Count, 3);
		TestEqual(TEXT("and its output"), Sheet->Output.Count, 3);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

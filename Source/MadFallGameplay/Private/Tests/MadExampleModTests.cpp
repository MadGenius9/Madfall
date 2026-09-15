// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBlockRegistry.h"
#include "MadGameplayDefinitions.h"
#include "MadModManager.h"
#include "MadVoxelWorldSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadExampleDataModTest,
	"MadFall.Mods.ExampleDataMod",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadExampleDataModTest::RunTest(const FString& Parameters)
{
	// The shipped Tier-1 example goes through the real pipeline: discovery at
	// startup, resolution, the global registries, patches. If this breaks, the
	// modding documentation is lying.
	const FMadModResolution& Mods = MadFall::GetModManager().GetResolution();
	const FName ModId(TEXT("example_scavenger"));

	if (!TestTrue(TEXT("example_scavenger is discovered and enabled"), Mods.IsEnabled(ModId)))
	{
		AddInfo(MadFall::GetModManager().Describe());
		return false;
	}

	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const FMadGameplayDefinitions& Defs = MadFall::GetGameplayDefinitions();

	const FName WallId(TEXT("example_scavenger:scrap_wall"));
	TestTrue(TEXT("mod block registered"), Blocks.IsRegistered(WallId));
	if (const FMadBlockDefinitionData* Wall = Blocks.FindDefinition(Blocks.ResolveRuntimeId(WallId)))
	{
		TestEqual(TEXT("mod block overrides its first-party parent"), Wall->Hardness, 320.0f);
		TestTrue(TEXT("and inherits the rest (damage stages)"), Wall->DamageStages.Num() > 0);
	}
	TestNotNull(TEXT("mod block gets a generated item"), Defs.FindItemForBlock(WallId));

	const FMadItemDefinition* Knife = Defs.FindItem(FName(TEXT("example_scavenger:scrap_knife")));
	if (TestNotNull(TEXT("mod item"), Knife))
	{
		TestTrue(TEXT("inherits first-party base_tool"), Knife->bHasTool && Knife->MaxStack == 1);
		TestEqual(TEXT("own durability"), Knife->Tool.Durability, 120);
	}
	TestNotNull(TEXT("mod recipe"), Defs.FindRecipe(FName(TEXT("example_scavenger:scrap_knife"))));
	TestNotNull(TEXT("mod zombie"), Defs.FindZombie(FName(TEXT("example_scavenger:zombie_runner"))));

	TArray<const FMadZombieDefinition*> Horde;
	Defs.GetZombiesInGroup(FName(TEXT("madfall:zombies/horde")), 100, Horde);
	TestTrue(TEXT("mod zombie joins the first-party horde group"),
		Horde.ContainsByPredicate([](const FMadZombieDefinition* Z) { return Z->Id == FName(TEXT("example_scavenger:zombie_runner")); }));

	auto TableHasKnife = [&Defs](const TCHAR* Table)
	{
		const FMadLootTableDefinition* Loot = Defs.FindLootTable(FName(Table));
		return Loot != nullptr && Loot->Entries.ContainsByPredicate([](const FMadLootEntry& E) { return E.Item == FName(TEXT("example_scavenger:scrap_knife")); });
	};
	TestTrue(TEXT("patch appended the knife to a first-party loot table"), TableHasKnife(TEXT("madfall:loot/weapons_locker")));
	TestTrue(TEXT("second patch in the same file too"), TableHasKnife(TEXT("madfall:loot/tools")));

	if (const FMadLootTableDefinition* Locker = Defs.FindLootTable(FName(TEXT("madfall:loot/weapons_locker"))))
	{
		TestTrue(TEXT("patching kept the original entries"), Locker->Entries.Num() >= 4);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

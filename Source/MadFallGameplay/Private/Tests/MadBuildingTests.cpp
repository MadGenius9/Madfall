// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBlockRegistry.h"
#include "MadBuilding.h"
#include "MadVoxelWorldSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadBlockWorkTest,
	"MadFall.World.BlockWork",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadBlockWorkTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Building;
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();

	auto Nothing = [](FName) { return 0; };
	auto Everything = [](FName) { return 999; };

	// --- the ladder the recipes describe ------------------------------------
	//
	// Not a list written here: what the recipes actually say, so this fails if
	// someone changes a tier recipe in a way that breaks upgrading rather than
	// only breaking crafting.
	const FName Frame(TEXT("madfall:wood_frame"));
	const FMadBlockWorkPlan Step = PlanBlockWork(Definitions, Frame, 0, 99, Everything);
	TestEqual(TEXT("a whole wood frame offers an upgrade"), Step.Action, EMadBlockWork::Upgrade);
	TestEqual(TEXT("and it becomes reinforced wood"), Step.ToBlock, FName(TEXT("madfall:wood_reinforced")));
	TestTrue(TEXT("which costs something"), Step.Cost.Num() > 0);
	TestTrue(TEXT("and can be done with a full backpack"), Step.CanDo());

	// The base block is not part of the price: it is already in the wall.
	for (const FMadItemAmount& Amount : Step.Cost)
	{
		const FMadItemDefinition* Item = Definitions.FindItem(Amount.Item);
		TestTrue(*FString::Printf(TEXT("the cost does not include the block itself (%s)"), *Amount.Item.ToString()),
			Item == nullptr || Item->PlacesBlock != Frame);
	}

	// --- the ladder goes somewhere ------------------------------------------
	//
	// Walking it from the bottom proves the steps chain rather than each one
	// existing in isolation, and that it terminates.
	int32 Rungs = 0;
	FName At = Frame;
	TSet<FName> Seen;
	while (Rungs < 16)
	{
		bool bAlready = false;
		Seen.Add(At, &bAlready);
		TestFalse(TEXT("the upgrade ladder does not loop"), bAlready);
		if (bAlready)
		{
			break;
		}
		const FMadBlockWorkPlan Next = PlanBlockWork(Definitions, At, 0, 99, Everything);
		if (Next.Action != EMadBlockWork::Upgrade)
		{
			break;
		}
		AddInfo(FString::Printf(TEXT("%s -> %s"), *At.ToString(), *Next.ToBlock.ToString()));
		At = Next.ToBlock;
		++Rungs;
	}
	TestTrue(*FString::Printf(TEXT("the ladder has more than one rung (%d)"), Rungs), Rungs >= 2);

	// --- what stops you ------------------------------------------------------
	const FMadBlockWorkPlan Broke = PlanBlockWork(Definitions, Frame, 0, 99, Nothing);
	TestEqual(TEXT("an empty backpack still plans the upgrade"), Broke.Action, EMadBlockWork::Upgrade);
	TestTrue(TEXT("but says the materials are missing"), Broke.bMissingMaterials);
	TestFalse(TEXT("so it cannot be done"), Broke.CanDo());

	if (Step.RequiredLevel > 0)
	{
		const FMadBlockWorkPlan Early = PlanBlockWork(Definitions, Frame, 0, 0, Everything);
		TestTrue(TEXT("a survivor below the recipe's level is told so"), Early.bUnderLevelled);
		TestFalse(TEXT("and cannot do it"), Early.CanDo());
	}

	// --- damage comes first --------------------------------------------------
	//
	// WHY: upgrading a chewed wall would otherwise throw its damage away, which
	// makes upgrading a cheaper repair than repairing.
	const FMadBlockWorkPlan Chewed = PlanBlockWork(Definitions, Frame, 200, 99, Everything);
	TestEqual(TEXT("a damaged block is repaired, not upgraded"), Chewed.Action, EMadBlockWork::Repair);
	TestEqual(TEXT("patched with one of itself"), Chewed.Cost.Num(), 1);
	TestTrue(TEXT("and that is the block's own item"), !Chewed.Cost.IsEmpty()
		&& Definitions.FindItem(Chewed.Cost[0].Item) != nullptr
		&& Definitions.FindItem(Chewed.Cost[0].Item)->PlacesBlock == Frame);

	const FMadBlockWorkPlan CannotPatch = PlanBlockWork(Definitions, Frame, 200, 99, Nothing);
	TestTrue(TEXT("with none of it in the backpack, the repair is blocked"), CannotPatch.bMissingMaterials);

	// --- every placeable block can at least be patched ------------------------
	//
	// A block a survivor can put up and a horde can chew, with no way to mend
	// it, is a trap rather than a choice.
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	int32 Placeable = 0;
	for (const FMadItemDefinition& Item : Definitions.GetItems())
	{
		if (Item.PlacesBlock.IsNone() || !Blocks.IsRegistered(Item.PlacesBlock))
		{
			continue;
		}
		++Placeable;
		const FMadBlockWorkPlan Patch = PlanBlockWork(Definitions, Item.PlacesBlock, 128, 99, Everything);
		TestEqual(*FString::Printf(TEXT("%s can be repaired"), *Item.PlacesBlock.ToString()),
			Patch.Action, EMadBlockWork::Repair);
	}
	TestTrue(*FString::Printf(TEXT("there are blocks to place (%d)"), Placeable), Placeable > 5);

	// --- terrain is not a building --------------------------------------------
	const FMadBlockWorkPlan Rock = PlanBlockWork(Definitions, FName(TEXT("madfall:bedrock")), 128, 99, Everything);
	TestEqual(TEXT("bedrock is not something to mend"), Rock.Action, EMadBlockWork::None);

	return true;
}

#endif

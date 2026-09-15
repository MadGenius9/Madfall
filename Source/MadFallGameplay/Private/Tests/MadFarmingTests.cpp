// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadBlockDefinitionJson.h"
#include "MadBlockRegistry.h"
#include "MadFarming.h"
#include "MadGameplayDefinitions.h"
#include "MadGameplaySave.h"
#include "MadInventory.h"
#include "MadVoxelWorldSubsystem.h"
#include "Math/RandomStream.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadFarmingTests
{
	/**
	 * Three stages, 6 then 10 hours, mature at the end. No "extends": the
	 * registry's asset path used here resolves it only for directory loads.
	 */
	const TCHAR* StagesJson = TEXT(R"JSON([
		{ "schema": "madfall.block/1", "id": "test:ripe", "shape": { "kind": "model", "collision": "none" },
		  "placement": { "on_tag": "block.farmland" } },
		{ "schema": "madfall.block/1", "id": "test:young", "shape": { "kind": "model", "collision": "none" },
		  "placement": { "on_tag": "block.farmland" }, "grow": { "into": "test:ripe", "hours": 10 } },
		{ "schema": "madfall.block/1", "id": "test:seed", "shape": { "kind": "model", "collision": "none" },
		  "placement": { "on_tag": "block.farmland" }, "grow": { "into": "test:young", "hours": 6 } }
	])JSON");

	void BuildStages(FMadBlockRegistry& Blocks, TArray<FMadDefinitionError>& Errors)
	{
		TArray<FMadBlockDefinitionData> Parsed;
		MadFall::BlockDefinitionJson::ParseText(StagesJson, TEXT("test"), FName(TEXT("test")), Parsed, Errors);
		Blocks.BeginLoad();
		for (const FMadBlockDefinitionData& Data : Parsed)
		{
			Blocks.AddFromAsset(Data, Errors);
		}
		Blocks.FinishLoad(Errors);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadFarmingDefinitionsTest,
	"MadFall.Farming.Definitions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadFarmingDefinitionsTest::RunTest(const FString& Parameters)
{
	TArray<FMadDefinitionError> Errors;
	FMadBlockRegistry Blocks;
	MadFarmingTests::BuildStages(Blocks, Errors);
	TestEqual(TEXT("stage definitions parse cleanly"), Errors.Num(), 0);
	for (const FMadDefinitionError& Error : Errors)
	{
		AddError(Error.ToString());
	}

	const FMadBlockDefinitionData* Seed = Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(TEXT("test:seed"))));
	const FMadBlockDefinitionData* Ripe = Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(TEXT("test:ripe"))));
	if (!TestNotNull(TEXT("seed stage"), Seed) || !TestNotNull(TEXT("ripe stage"), Ripe))
	{
		return false;
	}
	TestEqual(TEXT("grow.into"), Seed->GrowInto, FName(TEXT("test:young")));
	TestEqual(TEXT("grow.hours"), Seed->GrowHours, 6.0f);
	TestEqual(TEXT("placement.on_tag"), Seed->PlaceOnTag, FName(TEXT("block.farmland")));
	TestTrue(TEXT("the last stage does not grow"), Ripe->GrowInto.IsNone());

	// A grow section without hours is an error, and the block does not grow.
	TArray<FMadBlockDefinitionData> Bad;
	TArray<FMadDefinitionError> BadErrors;
	MadFall::BlockDefinitionJson::ParseText(
		TEXT(R"({ "schema": "madfall.block/1", "id": "test:bad", "grow": { "into": "test:ripe" } })"),
		TEXT("bad"), FName(TEXT("test")), Bad, BadErrors);
	TestTrue(TEXT("grow without hours is reported"), BadErrors.ContainsByPredicate(
		[](const FMadDefinitionError& E) { return E.ToString().Contains(TEXT("/grow")); }));
	TestTrue(TEXT("and does not grow"), Bad.Num() == 1 && Bad[0].GrowInto.IsNone());

	// Loot entries marked "always" come with every roll, outside the weights.
	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> DefErrors;
	Defs.BeginLoad();
	const auto Json = [](const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	};
	Defs.AddItemJson(Json(TEXT(R"({ "schema": "madfall.item/1", "id": "test:grain", "max_stack": 99 })")), TEXT("t"), FName(TEXT("test")), DefErrors);
	Defs.AddItemJson(Json(TEXT(R"({ "schema": "madfall.item/1", "id": "test:seed_item", "max_stack": 99, "places_block": "test:seed" })")), TEXT("t"), FName(TEXT("test")), DefErrors);
	Defs.AddItemJson(Json(TEXT(R"({ "schema": "madfall.item/1", "id": "test:straw", "max_stack": 99 })")), TEXT("t"), FName(TEXT("test")), DefErrors);
	Defs.AddLootJson(Json(TEXT(R"({ "schema": "madfall.loot/1", "id": "test:loot/harvest", "rolls": [1, 1], "empty_weight": 3,
		"entries": [
			{ "item": "test:grain", "count": [2, 3], "always": true },
			{ "item": "test:seed_item", "always": true },
			{ "item": "test:straw", "weight": 1 }
		] })")), TEXT("t"), FName(TEXT("test")), DefErrors);
	Defs.FinishLoad(&Blocks, DefErrors);
	TestEqual(TEXT("loot with always entries parses cleanly"), DefErrors.Num(), 0);
	for (const FMadDefinitionError& Error : DefErrors)
	{
		AddError(Error.ToString());
	}

	const FMadLootTableDefinition* Harvest = Defs.FindLootTable(FName(TEXT("test:loot/harvest")));
	if (TestNotNull(TEXT("harvest table"), Harvest))
	{
		FRandomStream Random(7);
		int32 Straw = 0;
		for (int32 Trial = 0; Trial < 200; ++Trial)
		{
			TArray<FMadItemStack> Stacks;
			MadFall::Loot::Roll(*Harvest, Defs, FMadLootContext(), Random, Stacks);
			int32 Grain = 0;
			int32 Seeds = 0;
			for (const FMadItemStack& Stack : Stacks)
			{
				Grain += Stack.Item == FName(TEXT("test:grain")) ? Stack.Count : 0;
				Seeds += Stack.Item == FName(TEXT("test:seed_item")) ? Stack.Count : 0;
				Straw += Stack.Item == FName(TEXT("test:straw")) ? Stack.Count : 0;
			}
			if (Grain < 2 || Grain > 3 || Seeds != 1)
			{
				AddError(FString::Printf(TEXT("trial %d: %d grain and %d seed(s); always entries must always drop"), Trial, Grain, Seeds));
				break;
			}
		}
		// Straw is a 1-in-4 weighted pick against the empty weight: some, not all.
		TestTrue(TEXT("weighted entries still roll alongside"), Straw > 20 && Straw < 100);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadFarmingGrowthTest,
	"MadFall.Farming.Growth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadFarmingGrowthTest::RunTest(const FString& Parameters)
{
	TArray<FMadDefinitionError> Errors;
	FMadBlockRegistry Blocks;
	MadFarmingTests::BuildStages(Blocks, Errors);
	const auto Def = [&Blocks](const TCHAR* Id) { return Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(Id))); };

	FMadPlantTracker Tracker;
	const FIntVector Field(4, -2, 20);
	const FIntVector Other(5, -2, 20);

	// Planting records the stage start; a non-growing block forgets the voxel.
	Tracker.NoteBlock(Field, Def(TEXT("test:seed")), 100.0);
	Tracker.NoteBlock(Other, Def(TEXT("test:seed")), 101.0);
	TestEqual(TEXT("two plants"), Tracker.Num(), 2);
	Tracker.NoteBlock(Other, Def(TEXT("test:ripe")), 102.0);
	TestEqual(TEXT("replacing with a non-growing block forgets it"), Tracker.Num(), 1);
	Tracker.NoteBlock(Other, nullptr, 102.0);
	TestEqual(TEXT("air forgets nothing further"), Tracker.Num(), 1);

	TArray<FMadPlantTracker::FDue> Due;
	Tracker.CollectDue(105.9, Due);
	TestEqual(TEXT("not due before its hours are up"), Due.Num(), 0);
	TestEqual(TEXT("hours until the next stage"), Tracker.HoursUntilNext(105.0), 1.0, 1e-9);

	// Unloaded for two days: at hour 150 the seed is due, and its next stage
	// is timed from when it was due (106), not from when it was noticed.
	Tracker.CollectDue(150.0, Due);
	if (TestEqual(TEXT("seed due after its chunk returns"), Due.Num(), 1))
	{
		TestEqual(TEXT("due plant position"), Due[0].Position, Field);
		TestEqual(TEXT("grows into the young stage"), Due[0].Into, FName(TEXT("test:young")));
		TestEqual(TEXT("next stage starts at the old due hour"), Due[0].NextStageStartHour, 106.0, 1e-9);
		Tracker.NoteBlock(Field, Def(TEXT("test:young")), Due[0].NextStageStartHour);
	}

	Due.Reset();
	Tracker.CollectDue(150.0, Due);
	if (TestEqual(TEXT("the young stage catches up in the same visit"), Due.Num(), 1))
	{
		TestEqual(TEXT("young stage started at 106 and lasted 10 h"), Due[0].NextStageStartHour, 116.0, 1e-9);
		Tracker.NoteBlock(Field, Def(TEXT("test:ripe")), Due[0].NextStageStartHour);
	}
	TestEqual(TEXT("ripe crops are not tracked"), Tracker.Num(), 0);

	// Fast-forward, ordering and save round trip.
	Tracker.NoteBlock(FIntVector(1, 0, 0), Def(TEXT("test:seed")), 200.0);
	Tracker.NoteBlock(FIntVector(0, 0, 0), Def(TEXT("test:seed")), 200.0);
	Tracker.NoteBlock(FIntVector(2, 0, 0), Def(TEXT("test:young")), 195.0);
	Due.Reset();
	Tracker.CollectDue(201.0, Due);
	TestEqual(TEXT("nothing due at 201"), Due.Num(), 0);
	Tracker.Advance(10.0);
	Due.Reset();
	Tracker.CollectDue(201.0, Due);
	if (TestEqual(TEXT("advancing 10 h makes all three due"), Due.Num(), 3))
	{
		TestEqual(TEXT("earliest due first"), Due[0].Position, FIntVector(2, 0, 0));
		TestEqual(TEXT("ties by position"), Due[1].Position, FIntVector(0, 0, 0));
	}

	TArray<FMadPlantSaveData> Saved;
	Tracker.Export(Saved);
	Saved.Add({ FIntVector(9, 9, 9), FName(TEXT("removedmod:crop")), 12.0 });

	FMadGameplaySave Save;
	Save.Plants = Saved;
	FMadGameplaySave Loaded;
	TArray<FString> Warnings;
	TestTrue(TEXT("save with plants parses"), MadFall::GameplaySave::FromJson(MadFall::GameplaySave::ToJson(Save), Loaded, Warnings));
	TestEqual(TEXT("plants survive the save"), Loaded.Plants.Num(), 4);

	FMadPlantTracker Restored;
	Restored.Import(Loaded.Plants, Blocks);
	TestEqual(TEXT("every plant restored, unknown ones included"), Restored.Num(), 4);
	const FMadPlantTracker::FPlant* Young = Restored.Find(FIntVector(2, 0, 0));
	if (TestNotNull(TEXT("young plant restored"), Young))
	{
		TestEqual(TEXT("stage start survives, advance included"), Young->StageStartHour, 185.0, 1e-6);
		TestEqual(TEXT("due hour recomputed from the definition"), Young->DueHour, 195.0, 1e-6);
	}
	Due.Reset();
	Restored.CollectDue(1.0e9, Due);
	TestEqual(TEXT("a crop from a removed mod never comes due"), Due.Num(), 3);

	TArray<FMadPlantSaveData> Resaved;
	Restored.Export(Resaved);
	TestTrue(TEXT("and is written back verbatim"), Resaved.ContainsByPredicate(
		[](const FMadPlantSaveData& P) { return P.Block == FName(TEXT("removedmod:crop")) && P.StageStartHour == 12.0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadFarmingShippedContentTest,
	"MadFall.Farming.ShippedContent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadFarmingShippedContentTest::RunTest(const FString& Parameters)
{
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const FMadGameplayDefinitions& Defs = MadFall::GetGameplayDefinitions();
	const FName Farmland(TEXT("block.farmland"));

	int32 Chains = 0;
	for (const FMadBlockEntry& Entry : Blocks.GetEntries())
	{
		const FMadBlockDefinitionData& Block = Entry.Definition;
		if (Block.GrowInto.IsNone())
		{
			continue;
		}

		// Every growth chain ends, within a sane number of stages, at a block that exists.
		++Chains;
		const FMadBlockDefinitionData* Stage = &Block;
		int32 Steps = 0;
		while (Stage != nullptr && !Stage->GrowInto.IsNone() && Steps < 16)
		{
			const FMadBlockDefinitionData* Next = Blocks.FindDefinition(Blocks.ResolveRuntimeId(Stage->GrowInto));
			if (Next == nullptr || Blocks.IsUnresolvedId(Blocks.ResolveRuntimeId(Stage->GrowInto)))
			{
				AddError(FString::Printf(TEXT("%s grows into unknown block %s"), *Stage->Id.ToString(), *Stage->GrowInto.ToString()));
				break;
			}
			Stage = Next;
			++Steps;
		}
		TestTrue(*FString::Printf(TEXT("%s's growth chain ends"), *Block.Id.ToString()), Steps < 16);
		TestNotEqual(*FString::Printf(TEXT("%s takes time"), *Block.Id.ToString()), Block.GrowHours, 0.0f);
		TestEqual(*FString::Printf(TEXT("%s grows on farmland"), *Block.Id.ToString()), Block.PlaceOnTag, Farmland);
		TestTrue(*FString::Printf(TEXT("%s has a drop table"), *Block.Id.ToString()), !Block.DropTable.IsNone());
	}
	TestTrue(TEXT("corn and potato stages"), Chains >= 4);

	// The loop: a hoe tills dirt and grass into farmland; seeds plant a stage that grows.
	const FMadItemDefinition* Hoe = Defs.FindItem(FName(TEXT("madfall:stone_hoe")));
	if (TestNotNull(TEXT("stone hoe"), Hoe))
	{
		const FMadBlockDefinitionData* Tilled = Blocks.FindDefinition(Blocks.ResolveRuntimeId(Hoe->TillsInto));
		TestTrue(TEXT("the hoe tills into farmland"), Tilled != nullptr && Tilled->Tags.Contains(Farmland));
		TestTrue(TEXT("farmland is cubic"), Tilled != nullptr && Tilled->ShapeKind == EMadBlockShapeKind::Cubic);
	}
	for (const TCHAR* Ground : { TEXT("madfall:dirt"), TEXT("madfall:grass") })
	{
		const FMadBlockDefinitionData* Block = Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(Ground)));
		TestTrue(*FString::Printf(TEXT("%s is tillable"), Ground), Block != nullptr && Block->Tags.Contains(FName(TEXT("block.tillable"))));
	}
	for (const TCHAR* SeedId : { TEXT("madfall:corn_seed"), TEXT("madfall:potato_seed") })
	{
		const FMadItemDefinition* Seed = Defs.FindItem(FName(SeedId));
		const FMadBlockDefinitionData* Planted = Seed ? Blocks.FindDefinition(Blocks.ResolveRuntimeId(Seed->PlacesBlock)) : nullptr;
		TestTrue(*FString::Printf(TEXT("%s plants a growing stage"), SeedId), Planted != nullptr && !Planted->GrowInto.IsNone());
	}

	// Ripe corn always yields corn and seeds back, so a farm sustains itself.
	const FMadLootTableDefinition* CornHarvest = Defs.FindLootTable(FName(TEXT("madfall:loot/corn_harvest")));
	if (TestNotNull(TEXT("corn harvest table"), CornHarvest))
	{
		FRandomStream Random(3);
		for (int32 Trial = 0; Trial < 20; ++Trial)
		{
			TArray<FMadItemStack> Stacks;
			MadFall::Loot::Roll(*CornHarvest, Defs, FMadLootContext(), Random, Stacks);
			const bool bCorn = Stacks.ContainsByPredicate([](const FMadItemStack& S) { return S.Item == FName(TEXT("madfall:corn")); });
			const bool bSeed = Stacks.ContainsByPredicate([](const FMadItemStack& S) { return S.Item == FName(TEXT("madfall:corn_seed")); });
			if (!bCorn || !bSeed)
			{
				AddError(TEXT("a corn harvest dropped no corn or no seeds"));
				break;
			}
		}
	}
	return true;
}

#endif

// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadBlockRegistry.h"
#include "MadDefinitionSources.h"
#include "MadGameplayDefinitions.h"
#include "MadGameplaySave.h"
#include "MadPrefabRegistry.h"
#include "MadQuests.h"
#include "MadVoxelWorldSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadQuestTests
{
	TSharedRef<FJsonObject> Json(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadQuestLogTest,
	"MadFall.Progression.Quests",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadQuestLogTest::RunTest(const FString& Parameters)
{
	using namespace MadQuestTests;
	const FName Mod(TEXT("test"));

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	Defs.BeginLoad();
	Defs.AddItemJson(Json(TEXT(R"({ "schema": "madfall.item/1", "id": "test:plank", "tags": ["item.wood"] })")), TEXT("t"), Mod, Errors);
	Defs.AddItemJson(Json(TEXT(R"({ "schema": "madfall.item/1", "id": "test:axe", "max_stack": 1 })")), TEXT("t"), Mod, Errors);
	Defs.AddQuestJson(Json(TEXT(R"({ "schema": "madfall.quest/1", "id": "test:quest/tools", "order": 1,
		"objectives": [ { "type": "craft", "target": "test:axe" } ],
		"rewards": { "experience": 10, "items": [ { "item": "test:plank", "count": 3 } ] } })")), TEXT("t"), Mod, Errors);
	Defs.AddQuestJson(Json(TEXT(R"({ "schema": "madfall.quest/1", "id": "test:quest/wood", "requires": ["test:quest/tools"], "order": 2,
		"objectives": [ { "type": "break", "tag": "block.wood", "count": 3 }, { "type": "have", "tag": "item.wood", "count": 10 } ] })")), TEXT("t"), Mod, Errors);
	Defs.AddQuestJson(Json(TEXT(R"({ "schema": "madfall.quest/1", "id": "test:quest/night", "requires": ["test:quest/tools"], "order": 3,
		"objectives": [ { "type": "kill_zombie", "count": 2 }, { "type": "reach_day", "count": 8 } ] })")), TEXT("t"), Mod, Errors);
	Defs.AddQuestJson(Json(TEXT(R"({ "schema": "madfall.quest/1", "id": "test:quest/orphan", "requires": ["test:quest/nowhere"],
		"objectives": [ { "type": "set_spawn" } ] })")), TEXT("t"), Mod, Errors);
	Defs.AddQuestJson(Json(TEXT(R"({ "schema": "madfall.quest/1", "id": "test:quest/broken", "objectives": [ { "type": "dance" } ] })")), TEXT("t"), Mod, Errors);
	Defs.FinishLoad(nullptr, Errors);

	TestTrue(TEXT("an unknown objective type is reported"), Errors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.ToString().Contains(TEXT("dance")); }));
	TestTrue(TEXT("an unknown requirement is reported"), Errors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.ToString().Contains(TEXT("nowhere")); }));
	TestNull(TEXT("a quest with no valid objectives does not load"), Defs.FindQuest(FName(TEXT("test:quest/broken"))));

	FMadQuestLog Log;
	TArray<FName> Started = Log.Refresh(Defs);
	TestTrue(TEXT("the first quest starts"), Started.Contains(FName(TEXT("test:quest/tools"))));
	TestTrue(TEXT("a quest whose missing requirement was dropped starts too"), Started.Contains(FName(TEXT("test:quest/orphan"))));
	TestFalse(TEXT("a locked quest does not"), Log.IsActive(FName(TEXT("test:quest/wood"))));

	// Events that do not match count for nothing.
	TestEqual(TEXT("crafting planks completes nothing"), Log.Notify(EMadQuestObjectiveType::Craft, FName(TEXT("test:plank")), {}, 5, Defs).Num(), 0);
	TArray<FName> Done = Log.Notify(EMadQuestObjectiveType::Craft, FName(TEXT("test:axe")), {}, 1, Defs);
	TestTrue(TEXT("crafting the axe completes the tools quest"), Done.Num() == 1 && Done[0] == FName(TEXT("test:quest/tools")));
	TestTrue(TEXT("marked complete"), Log.IsComplete(FName(TEXT("test:quest/tools"))));
	Started = Log.Refresh(Defs);
	TestEqual(TEXT("completing it unlocks two quests"), Started.Num(), 2);

	// Tag-matched events accumulate and clamp.
	Log.Notify(EMadQuestObjectiveType::Break, FName(TEXT("test:log")), { FName(TEXT("block.wood")) }, 2, Defs);
	Log.Notify(EMadQuestObjectiveType::Break, FName(TEXT("test:stone")), { FName(TEXT("block.stone")) }, 5, Defs);
	Log.Notify(EMadQuestObjectiveType::Break, FName(TEXT("test:log")), { FName(TEXT("block.wood")) }, 9, Defs);
	const FMadQuestProgress* Wood = Log.FindActive(FName(TEXT("test:quest/wood")));
	if (TestNotNull(TEXT("wood quest active"), Wood))
	{
		TestEqual(TEXT("breaks counted and clamped at the goal"), Wood->Counts[0], 3);
	}

	// State objectives follow the state, both ways.
	int32 Planks = 12;
	auto Current = [&Planks](const FMadQuestObjective& Objective)
	{
		return Objective.Type == EMadQuestObjectiveType::ReachDay ? 3 : Planks;
	};
	Planks = 4;
	TestEqual(TEXT("too few planks: not done"), Log.Evaluate(Current, Defs).Num(), 0);
	Planks = 12;
	Done = Log.Evaluate(Current, Defs);
	TestTrue(TEXT("enough planks: the wood quest completes"), Done.Contains(FName(TEXT("test:quest/wood"))));
	TestTrue(TEXT("the day objective tracks the clock"), Log.FindActive(FName(TEXT("test:quest/night"))) != nullptr
		&& Log.FindActive(FName(TEXT("test:quest/night")))->Counts[1] == 3);

	// Save and restore, including a quest from a removed mod.
	TArray<FName> Completed;
	TArray<FMadQuestProgress> Active;
	Log.Notify(EMadQuestObjectiveType::KillZombie, FName(TEXT("test:zombie")), {}, 1, Defs);
	Log.Export(Completed, Active);
	Active.Add({ FName(TEXT("gonemod:quest/x")), { 4 } });

	FMadGameplaySave Save;
	Save.bHasPlayer = true;
	Save.Player.CompletedQuests = Completed;
	Save.Player.ActiveQuests = Active;
	FMadGameplaySave Loaded;
	TArray<FString> Warnings;
	TestTrue(TEXT("save parses"), MadFall::GameplaySave::FromJson(MadFall::GameplaySave::ToJson(Save), Loaded, Warnings));

	FMadQuestLog Restored;
	Restored.Import(Loaded.Player.CompletedQuests, Loaded.Player.ActiveQuests, Defs);
	TestTrue(TEXT("completed quests survive"), Restored.IsComplete(FName(TEXT("test:quest/tools"))) && Restored.IsComplete(FName(TEXT("test:quest/wood"))));
	const FMadQuestProgress* Night = Restored.FindActive(FName(TEXT("test:quest/night")));
	TestTrue(TEXT("progress survives"), Night != nullptr && Night->Counts.Num() == 2 && Night->Counts[0] == 1);
	TestTrue(TEXT("an unknown quest is kept"), Restored.IsActive(FName(TEXT("gonemod:quest/x"))));
	TestEqual(TEXT("nothing restarts that was done"), Restored.Refresh(Defs).Num(), 0);

	TArray<TPair<const FMadQuestDefinition*, const FMadQuestProgress*>> Journal;
	Restored.GetJournal(Defs, Journal);
	TestTrue(TEXT("the journal lists known quests in order"), Journal.Num() == 2 && Journal[0].Key->Order <= Journal[1].Key->Order);
	return true;
}

// ===========================================================================
// Handing a job in
// ===========================================================================
//
// A hand-in quest is done but not paid: every objective met, still in the
// journal, waiting for the survivor to reach someone to report to. It is
// stored as nothing at all - just an active quest whose counts are full - so
// a save written before hand-ins existed loads unchanged, and that is the
// property most worth pinning down, because the alternative is a save field
// nobody notices is missing until a player's journal empties itself.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadQuestHandInTest,
	"MadFall.Progression.QuestHandIn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadQuestHandInTest::RunTest(const FString& Parameters)
{
	using namespace MadQuestTests;
	const FName Mod(TEXT("test"));
	const FName Job(TEXT("test:quest/job"));
	const FName Chore(TEXT("test:quest/chore"));

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	Defs.BeginLoad();
	Defs.AddItemJson(Json(TEXT(R"({ "schema": "madfall.item/1", "id": "test:axe", "max_stack": 1 })")), TEXT("t"), Mod, Errors);
	Defs.AddQuestJson(Json(TEXT(R"({ "schema": "madfall.quest/1", "id": "test:quest/job", "order": 1, "hand_in": true,
		"objectives": [ { "type": "craft", "target": "test:axe", "count": 2 } ] })")), TEXT("t"), Mod, Errors);
	Defs.AddQuestJson(Json(TEXT(R"({ "schema": "madfall.quest/1", "id": "test:quest/chore", "order": 2,
		"objectives": [ { "type": "craft", "target": "test:axe" } ] })")), TEXT("t"), Mod, Errors);
	Defs.FinishLoad(nullptr, Errors);
	TestEqual(TEXT("both quests load"), Defs.GetQuests().Num(), 2);

	FMadQuestLog Log;
	Log.Refresh(Defs);
	TestTrue(TEXT("the job starts"), Log.IsActive(Job));

	// One axe: neither quest's objective is met for the job, but the chore's is.
	TArray<FName> Done = Log.Notify(EMadQuestObjectiveType::Craft, FName(TEXT("test:axe")), {}, 1, Defs);
	TestTrue(TEXT("an ordinary quest still completes where it stands"), Done.Contains(Chore));
	TestFalse(TEXT("and the job is not waiting yet"), Log.IsWaitingToHandIn(Job, Defs));

	// The second axe meets the job's objective. It must NOT complete.
	Done = Log.Notify(EMadQuestObjectiveType::Craft, FName(TEXT("test:axe")), {}, 1, Defs);
	TestFalse(TEXT("meeting a hand-in quest's objectives does not complete it"), Done.Contains(Job));
	TestFalse(TEXT("it is not marked complete"), Log.IsComplete(Job));
	TestTrue(TEXT("it is still in the journal"), Log.IsActive(Job));
	TestTrue(TEXT("and it reports itself as waiting to be handed in"), Log.IsWaitingToHandIn(Job, Defs));
	TestEqual(TEXT("one job is waiting"), Log.NumWaitingToHandIn(Defs), 1);

	// Further progress does not lose the "waiting" state or double-count.
	Log.Notify(EMadQuestObjectiveType::Craft, FName(TEXT("test:axe")), {}, 5, Defs);
	TestTrue(TEXT("overshooting the objective keeps it waiting"), Log.IsWaitingToHandIn(Job, Defs));

	// A save round trip through the same representation an old save uses.
	{
		TArray<FName> Completed;
		TArray<FMadQuestProgress> Active;
		Log.Export(Completed, Active);

		FMadQuestLog Reloaded;
		Reloaded.Import(Completed, Active, Defs);
		TestTrue(TEXT("a reloaded save still knows the job is waiting"), Reloaded.IsWaitingToHandIn(Job, Defs));
		TestFalse(TEXT("and has not paid it"), Reloaded.IsComplete(Job));
	}

	// Reaching the trader.
	const TArray<FName> Paid = Log.HandIn(Defs);
	TestTrue(TEXT("handing in completes the job"), Paid.Contains(Job));
	TestTrue(TEXT("it is marked complete"), Log.IsComplete(Job));
	TestFalse(TEXT("and gone from the journal"), Log.IsActive(Job));
	TestEqual(TEXT("nothing is left waiting"), Log.NumWaitingToHandIn(Defs), 0);

	// A second visit pays nothing twice.
	TestEqual(TEXT("handing in again pays nothing"), Log.HandIn(Defs).Num(), 0);

	// A hand-in quest whose objectives are not met is not paid by a visit.
	{
		FMadQuestLog Fresh;
		Fresh.Refresh(Defs);
		Fresh.Notify(EMadQuestObjectiveType::Craft, FName(TEXT("test:axe")), {}, 1, Defs);
		TestEqual(TEXT("an unfinished job is not paid by walking past the trader"), Fresh.HandIn(Defs).Num(), 0);
		TestTrue(TEXT("and stays active"), Fresh.IsActive(Job));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadShippedQuestsTest,
	"MadFall.Progression.ShippedQuests",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadShippedQuestsTest::RunTest(const FString& Parameters)
{
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	Defs.BeginLoad();
	for (const TCHAR* Kind : { TEXT("items"), TEXT("recipes"), TEXT("loot"), TEXT("quests") })
	{
		MadFall::Definitions::ForEachSource(Kind, [&](const FString& Dir, FName ModId)
		{
			if (FCString::Strcmp(Kind, TEXT("items")) == 0) { Defs.AddItemsFromDirectory(Dir, ModId, Errors); }
			else if (FCString::Strcmp(Kind, TEXT("recipes")) == 0) { Defs.AddRecipesFromDirectory(Dir, ModId, Errors); }
			else if (FCString::Strcmp(Kind, TEXT("loot")) == 0) { Defs.AddLootFromDirectory(Dir, ModId, Errors); }
			else { Defs.AddQuestsFromDirectory(Dir, ModId, Errors); }
		});
	}
	Defs.FinishLoad(&Blocks, Errors);
	for (const FMadDefinitionError& Error : Errors)
	{
		if (Error.SourcePath.Contains(TEXT("quests")))
		{
			AddError(Error.ToString());
		}
	}
	TestTrue(TEXT("the tutorial ships"), Defs.GetQuests().Num() >= 8);

	// Completing every quest in unlock order reaches all of them: no quest is stranded behind a cycle.
	FMadQuestLog Log;
	int32 Guard = 0;
	for (TArray<FName> Started = Log.Refresh(Defs); Started.Num() > 0 && Guard < 64; Started = Log.Refresh(Defs), ++Guard)
	{
		for (const FName& Id : Started)
		{
			Log.ForceComplete(Id, Defs);
		}
	}
	TestEqual(TEXT("every shipped quest is reachable"), Log.NumCompleted(), Defs.GetQuests().Num());

	// Exactly one quest to start with, so a new survivor's journal is not a wall of text.
	FMadQuestLog Fresh;
	TestEqual(TEXT("one quest at the start"), Fresh.Refresh(Defs).Num(), 1);

	// Every objective names something that exists or a tag something carries.
	for (const FMadQuestDefinition& Quest : Defs.GetQuests())
	{
		for (const FMadQuestObjective& Objective : Quest.Objectives)
		{
			if (Objective.Type == EMadQuestObjectiveType::Place && !Objective.Target.IsNone())
			{
				TestTrue(*FString::Printf(TEXT("%s: block %s exists"), *Quest.Id.ToString(), *Objective.Target.ToString()), Blocks.IsRegistered(Objective.Target));
			}
			const FString Text = MadFall::Quests::DescribeObjective(Objective, Defs);
			TestFalse(*FString::Printf(TEXT("%s: objective text is not a raw key (%s)"), *Quest.Id.ToString(), *Text), Text.StartsWith(TEXT("@")) || Text.StartsWith(TEXT("quests.")));
		}
	}
	// --- a clearing job is one building's worth of work -----------------------
	//
	// WHY this is checked rather than trusted: a clear_poi objective's count is
	// a number in one file and the zombies that satisfy it are spawn markers in
	// another. Raise a prefab's sleepers and the job finishes early; lower them
	// and the survivor clears the building, finds it empty, and has to walk to a
	// second one with no idea why. Neither shows up as an error anywhere.
	{
		const FMadPrefabRegistry& Prefabs = UMadVoxelWorldSubsystem::GetPrefabRegistry();

		auto SleeperCount = [](const FMadPrefab& Prefab)
		{
			int32 Total = 0;
			for (const FMadPoiMarker& Marker : Prefab.Markers)
			{
				if (!Marker.SpawnGroup.IsNone())
				{
					Total += FMath::Max(1, Marker.Count);
				}
			}
			return Total;
		};

		int32 JobsChecked = 0;
		for (const FMadQuestDefinition& Quest : Defs.GetQuests())
		{
			for (const FMadQuestObjective& Objective : Quest.Objectives)
			{
				if (Objective.Type != EMadQuestObjectiveType::ClearPoi)
				{
					continue;
				}
				++JobsChecked;

				int32 Matches = 0;
				int32 Exact = 0;
				FString Counts;
				for (const FMadPrefab& Prefab : Prefabs.GetAll())
				{
					if (!Objective.Matches(Prefab.Id, Prefab.Tags))
					{
						continue;
					}
					const int32 Sleepers = SleeperCount(Prefab);
					if (Sleepers == 0)
					{
						continue;   // nothing lives there; it can never be cleared
					}
					++Matches;
					Exact += Sleepers == Objective.Count ? 1 : 0;
					Counts += FString::Printf(TEXT(" %s=%d"), *Prefab.Id.ToString(), Sleepers);
				}

				TestTrue(*FString::Printf(TEXT("%s: a building with sleepers matches the job"), *Quest.Id.ToString()), Matches > 0);
				TestTrue(*FString::Printf(TEXT("%s: asks for exactly one building's sleepers (wants %d, buildings:%s)"),
					*Quest.Id.ToString(), Objective.Count, *Counts), Exact > 0);
			}
		}
		TestTrue(TEXT("clearing jobs ship at all"), JobsChecked >= 3);
	}

	const TArray<FName> RipeTag = { FName(TEXT("block.ripe")) };
	TestTrue(TEXT("ripe corn is tagged ripe"), Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(TEXT("madfall:corn_plant"))))->Tags.Contains(RipeTag[0]));
	TestFalse(TEXT("a sprout is not"), Blocks.FindDefinition(Blocks.ResolveRuntimeId(FName(TEXT("madfall:corn_sprout"))))->Tags.Contains(RipeTag[0]));
	return true;
}

#endif

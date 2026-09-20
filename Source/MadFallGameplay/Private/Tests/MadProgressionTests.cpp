// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadGameplayDefinitions.h"
#include "MadHarvest.h"
#include "MadProgression.h"
#include "MadSurvivalModel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadProgressionTests
{
	TSharedRef<FJsonObject> Json(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadPerkRulesTest,
	"MadFall.Progression.Perks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadPerkRulesTest::RunTest(const FString& Parameters)
{
	using namespace MadProgressionTests;

	const FName Mod(TEXT("test"));
	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	Defs.BeginLoad();
	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:miner","ranks":[
		{"level":2,"modifiers":{"mining_damage":1.5}},
		{"level":4,"modifiers":{"mining_damage":2.0,"stamina_cost":0.5}}]})")), TEXT("perks.json"), Mod, Errors);
	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:athlete","ranks":[{"level":1,"modifiers":{"stamina_cost":0.8}}]})")),
		TEXT("perks.json"), Mod, Errors);
	Defs.FinishLoad(nullptr, Errors);
	for (const FMadDefinitionError& Error : Errors)
	{
		AddError(Error.ToString());
	}

	const FName Miner(TEXT("test:miner"));
	const FName Athlete(TEXT("test:athlete"));
	const FName Mining(TEXT("mining_damage"));
	const FName Stamina(TEXT("stamina_cost"));
	TMap<FName, int32> Ranks;

	// Points: one per level after the first.
	TestEqual(TEXT("level 1 has no points"), MadFall::Perks::GetUnspentPoints(1, Ranks), 0);
	TestEqual(TEXT("level 3 has two points"), MadFall::Perks::GetUnspentPoints(3, Ranks), 2);

	TestTrue(TEXT("no points at level 1"), MadFall::Perks::TryBuy(Ranks, Athlete, 1, Defs) == EMadPerkResult::NoPoints);
	TestTrue(TEXT("unknown perk"), MadFall::Perks::TryBuy(Ranks, FName(TEXT("test:nope")), 5, Defs) == EMadPerkResult::UnknownPerk);
	TestTrue(TEXT("rank 1 needs level 2"), MadFall::Perks::TryBuy(Ranks, Miner, 1, Defs) == EMadPerkResult::LevelTooLow);
	TestEqual(TEXT("failed buys spend nothing"), Ranks.Num(), 0);

	TestTrue(TEXT("buy miner 1 at level 3"), MadFall::Perks::TryBuy(Ranks, Miner, 3, Defs) == EMadPerkResult::Ok);
	TestTrue(TEXT("miner 2 needs level 4"), MadFall::Perks::TryBuy(Ranks, Miner, 3, Defs) == EMadPerkResult::LevelTooLow);
	TestTrue(TEXT("buy athlete with the second point"), MadFall::Perks::TryBuy(Ranks, Athlete, 3, Defs) == EMadPerkResult::Ok);
	TestEqual(TEXT("points used up"), MadFall::Perks::GetUnspentPoints(3, Ranks), 0);
	TestTrue(TEXT("athlete maxed"), MadFall::Perks::TryBuy(Ranks, Athlete, 10, Defs) == EMadPerkResult::MaxRank);

	// Multipliers: a rank REPLACES the previous rank's modifiers; different perks multiply.
	TestEqual(TEXT("miner 1 mining"), MadFall::Perks::GetMultiplier(Ranks, Defs, Mining), 1.5f);
	TestEqual(TEXT("athlete stamina"), MadFall::Perks::GetMultiplier(Ranks, Defs, Stamina), 0.8f);
	TestEqual(TEXT("unmodified stat is 1"), MadFall::Perks::GetMultiplier(Ranks, Defs, FName(TEXT("craft_time"))), 1.0f);

	TestTrue(TEXT("buy miner 2 at level 4"), MadFall::Perks::TryBuy(Ranks, Miner, 4, Defs) == EMadPerkResult::Ok);
	TestEqual(TEXT("miner 2 replaces rank 1"), MadFall::Perks::GetMultiplier(Ranks, Defs, Mining), 2.0f);
	TestTrue(TEXT("miner 2 and athlete stack multiplicatively"), FMath::IsNearlyEqual(MadFall::Perks::GetMultiplier(Ranks, Defs, Stamina), 0.4f));

	// A save from a game with a mod perk that is no longer installed, or ranks
	// beyond what a patched perk now has, must not crash or count as a bonus.
	TMap<FName, int32> Stale;
	Stale.Add(FName(TEXT("gonemod:telekinesis")), 3);
	Stale.Add(Athlete, 7);
	TestEqual(TEXT("missing perks are ignored, over-ranked perks clamp"), MadFall::Perks::GetMultiplier(Stale, Defs, Stamina), 0.8f);
	TestEqual(TEXT("stale ranks still count as spent"), MadFall::Perks::GetUnspentPoints(5, Stale), 0);

	// The skills screen asks without buying.
	TMap<FName, int32> Fresh;
	TestTrue(TEXT("can buy reports ok"), MadFall::Perks::CanBuy(Fresh, Athlete, 2, Defs) == EMadPerkResult::Ok);
	TestTrue(TEXT("can buy reports the reason"), MadFall::Perks::CanBuy(Fresh, Miner, 1, Defs) == EMadPerkResult::LevelTooLow);
	TestEqual(TEXT("asking buys nothing"), Fresh.Num(), 0);

	TestEqual(TEXT("rank effects as percentages, sorted by stat"),
		MadFall::Perks::DescribeRank(Defs.FindPerk(Miner)->Ranks[1]), FString(TEXT("mining damage +100%, stamina cost -50%")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadPerkPrerequisitesTest,
	"MadFall.Progression.PerkPrerequisites",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadPerkPrerequisitesTest::RunTest(const FString& Parameters)
{
	using namespace MadProgressionTests;

	const FName Mod(TEXT("test"));
	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	Defs.BeginLoad();
	// athlete opens freely; sprinter needs athlete 1, and its second rank athlete 2;
	// marathon needs sprinter; the loop pair and the bad references must be caught at load.
	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:athlete","tags":["perk.agility"],"ranks":[
		{"level":1,"modifiers":{"max_stamina":1.1}},{"level":1,"modifiers":{"max_stamina":1.2}}]})")), TEXT("perks.json"), Mod, Errors);
	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:sprinter","tags":["perk.agility"],"ranks":[
		{"level":1,"modifiers":{"stamina_cost":0.9},"requires":{"test:athlete":1}},
		{"level":1,"modifiers":{"stamina_cost":0.8},"requires":{"test:athlete":2}}]})")), TEXT("perks.json"), Mod, Errors);
	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:marathon","tags":["perk.agility"],"ranks":[
		{"level":1,"modifiers":{"food_drain":0.9},"requires":{"test:sprinter":1}}]})")), TEXT("perks.json"), Mod, Errors);
	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:brawler","tags":["perk.strength"],"ranks":[
		{"level":1,"modifiers":{"melee_damage":1.1}}]})")), TEXT("perks.json"), Mod, Errors);
	TestEqual(TEXT("good perks parse cleanly"), Errors.Num(), 0);

	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:loop_a","ranks":[
		{"level":1,"modifiers":{},"requires":{"test:loop_b":1}}]})")), TEXT("loop.json"), Mod, Errors);
	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:loop_b","ranks":[
		{"level":1,"modifiers":{},"requires":{"test:loop_a":1,"gone:perk":1,"test:brawler":5}}]})")), TEXT("loop.json"), Mod, Errors);
	Defs.AddPerkJson(Json(TEXT(R"({"schema":"madfall.perk/1","id":"test:selfish","ranks":[
		{"level":1,"modifiers":{},"requires":{"test:selfish":1,"test:athlete":0.5}}]})")), TEXT("self.json"), Mod, Errors);
	TestEqual(TEXT("self and fractional requirements are parse errors"), Errors.Num(), 2);
	Errors.Reset();
	Defs.FinishLoad(nullptr, Errors);

	auto HasError = [&Errors](const TCHAR* Fragment)
	{
		return Errors.ContainsByPredicate([Fragment](const FMadDefinitionError& E) { return E.ToString().Contains(Fragment); });
	};
	TestTrue(TEXT("unknown perk reported"), HasError(TEXT("unknown perk 'gone:perk'")));
	TestTrue(TEXT("over-high rank reported"), HasError(TEXT("'test:brawler' has only 1 rank(s)")));
	TestTrue(TEXT("cycle reported"), HasError(TEXT("creates a cycle")));
	TestEqual(TEXT("exactly those three at load"), Errors.Num(), 3);

	const FMadPerkDefinition* LoopA = Defs.FindPerk(FName(TEXT("test:loop_a")));
	const FMadPerkDefinition* LoopB = Defs.FindPerk(FName(TEXT("test:loop_b")));
	if (!TestNotNull(TEXT("loop a"), LoopA) || !TestNotNull(TEXT("loop b"), LoopB))
	{
		return false;
	}
	TestEqual(TEXT("one side of the cycle is broken"), LoopA->Ranks[0].Requires.Num() + (LoopB->Ranks[0].Requires.Contains(FName(TEXT("test:loop_a"))) ? 1 : 0), 1);
	TestEqual(TEXT("unknown dropped, over-high clamped"), LoopB->Ranks[0].Requires.FindRef(FName(TEXT("test:brawler"))), 1);
	TestFalse(TEXT("unknown requirement removed"), LoopB->Ranks[0].Requires.Contains(FName(TEXT("gone:perk"))));

	const FName Athlete(TEXT("test:athlete"));
	const FName Sprinter(TEXT("test:sprinter"));
	const FName Marathon(TEXT("test:marathon"));
	TMap<FName, int32> Ranks;
	TestTrue(TEXT("sprinter needs athlete first"), MadFall::Perks::TryBuy(Ranks, Sprinter, 10, Defs) == EMadPerkResult::MissingPrerequisite);
	TestEqual(TEXT("nothing spent"), Ranks.Num(), 0);

	FName Missing;
	int32 MissingRank = 0;
	TestTrue(TEXT("the missing perk is named"), MadFall::Perks::FindMissingPrerequisite(Ranks, Defs.FindPerk(Sprinter)->Ranks[0], Missing, MissingRank));
	TestTrue(TEXT("named athlete"), Missing == Athlete);
	TestEqual(TEXT("rank 1"), MissingRank, 1);

	TestTrue(TEXT("buy athlete 1"), MadFall::Perks::TryBuy(Ranks, Athlete, 10, Defs) == EMadPerkResult::Ok);
	TestTrue(TEXT("now sprinter 1"), MadFall::Perks::TryBuy(Ranks, Sprinter, 10, Defs) == EMadPerkResult::Ok);
	TestTrue(TEXT("sprinter 2 needs athlete 2"), MadFall::Perks::CanBuy(Ranks, Sprinter, 10, Defs) == EMadPerkResult::MissingPrerequisite);
	TestTrue(TEXT("marathon chains off sprinter"), MadFall::Perks::TryBuy(Ranks, Marathon, 10, Defs) == EMadPerkResult::Ok);
	TestTrue(TEXT("level is checked before prerequisites"), MadFall::Perks::CanBuy(TMap<FName, int32>(), Sprinter, 0, Defs) == EMadPerkResult::LevelTooLow);

	// Tree: agility (athlete, sprinter, marathon by depth) before strength, untagged last.
	TestEqual(TEXT("depth of a root"), MadFall::Perks::GetTreeDepth(Defs, Athlete), 0);
	TestEqual(TEXT("depth of a chain"), MadFall::Perks::GetTreeDepth(Defs, Marathon), 2);
	const TArray<int32> Order = MadFall::Perks::GetTreeOrder(Defs);
	TArray<FName> Ids;
	for (int32 Index : Order)
	{
		Ids.Add(Defs.GetPerks()[Index].Id);
	}
	TestEqual(TEXT("a requirement in another attribute does not indent"), MadFall::Perks::GetTreeDepth(Defs, FName(TEXT("test:loop_b"))), 0);
	TestEqual(TEXT("one in the same group does"), MadFall::Perks::GetTreeDepth(Defs, FName(TEXT("test:loop_a"))), 1);
	TestTrue(TEXT("every perk listed once"), Ids.Num() == Defs.GetPerks().Num());
	TestTrue(TEXT("agility tree in depth order, then strength"), Ids.Num() >= 4 && Ids[0] == Athlete && Ids[1] == Sprinter
		&& Ids[2] == Marathon && Ids[3] == FName(TEXT("test:brawler")));
	return true;
}

// ===========================================================================
// What a perk multiplier does to a pile of drops
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadYieldBonusTest,
	"MadFall.Progression.YieldBonus",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadYieldBonusTest::RunTest(const FString& Parameters)
{
	FRandomStream Random(12345);

	auto Pile = [](int32 Count)
	{
		FMadItemStack Stack;
		Stack.Item = FName(TEXT("madfall:rock"));
		Stack.Count = Count;
		return TArray<FMadItemStack>{ Stack };
	};

	// No perk, no change - and nothing rolled, so the stream is untouched.
	{
		TArray<FMadItemStack> Drops = Pile(3);
		MadFall::Harvest::ApplyYieldBonus(Drops, 1.0f, Random);
		TestEqual(TEXT("without the perk the drops are exactly what the block gave"), Drops[0].Count, 3);
	}

	// A whole multiplier is exact, with no luck involved.
	{
		TArray<FMadItemStack> Drops = Pile(3);
		MadFall::Harvest::ApplyYieldBonus(Drops, 2.0f, Random);
		TestEqual(TEXT("doubling doubles"), Drops[0].Count, 6);
	}

	// WHY the fraction is odds and not rounding: most blocks drop one thing, and
	// 1 * 1.25 rounds to 1 every single time. Over many swings the perk has to
	// actually pay out, and it must never pay less than the block gave.
	{
		int32 Total = 0;
		int32 Worse = 0;
		constexpr int32 Swings = 4000;
		for (int32 Swing = 0; Swing < Swings; ++Swing)
		{
			TArray<FMadItemStack> Drops = Pile(1);
			MadFall::Harvest::ApplyYieldBonus(Drops, 1.25f, Random);
			Total += Drops[0].Count;
			Worse += Drops[0].Count < 1 ? 1 : 0;
		}
		const float Average = static_cast<float>(Total) / Swings;
		TestEqual(TEXT("a quarter more rocks, on average"), Average, 1.25f, 0.05f);
		TestEqual(TEXT("and never fewer than the block dropped"), Worse, 0);
	}

	// An empty stack is left alone rather than conjured into existence.
	{
		TArray<FMadItemStack> Drops;
		Drops.Add(FMadItemStack());
		MadFall::Harvest::ApplyYieldBonus(Drops, 3.0f, Random);
		TestTrue(TEXT("nothing multiplied is still nothing"), Drops[0].IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSurvivalTuningTest,
	"MadFall.Progression.SurvivalTuning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSurvivalTuningTest::RunTest(const FString& Parameters)
{
	using namespace MadProgressionTests;

	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	Defs.BeginLoad();
	Defs.AddTuningJson(Json(TEXT(R"({"schema":"madfall.tuning/1","id":"test:survival","values":{
		"food_per_minute":2.0,"hypothermia_below":33.5,"food_per_minut":9.0}})")), TEXT("tuning.json"), FName(TEXT("test")), Errors);
	Defs.FinishLoad(nullptr, Errors);
	TestEqual(TEXT("tuning loads"), Errors.Num(), 0);

	const FMadTuningDefinition* Definition = Defs.FindTuning(FName(TEXT("test:survival")));
	if (!TestNotNull(TEXT("tuning found"), Definition))
	{
		return false;
	}

	FMadSurvivalTuning Tuning;
	const float DefaultWater = Tuning.WaterPerMinute;
	TArray<FName> Unknown;
	MadFall::Survival::ApplyTuning(*Definition, Tuning, Unknown);

	TestEqual(TEXT("food rate applied"), Tuning.FoodPerMinute, 2.0f);
	TestEqual(TEXT("hypothermia threshold applied"), Tuning.HypothermiaBelow, 33.5f);
	TestEqual(TEXT("keys not mentioned keep their defaults"), Tuning.WaterPerMinute, DefaultWater);
	TestEqual(TEXT("the typo is reported"), Unknown.Num(), 1);
	TestTrue(TEXT("by name"), Unknown.Contains(FName(TEXT("food_per_minut"))));

	// Drain multipliers from perks scale the tuned rate.
	FMadSurvivalStats Stats;
	FMadSurvivalEnvironment Environment;
	Environment.FoodDrainMultiplier = 0.5f;
	MadFall::Survival::Step(Stats, Environment, Tuning, 60.0f);
	TestTrue(TEXT("one minute at 2/min with a 0.5 perk drains 1 food"), FMath::IsNearlyEqual(Stats.Food, 99.0f, 0.01f));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

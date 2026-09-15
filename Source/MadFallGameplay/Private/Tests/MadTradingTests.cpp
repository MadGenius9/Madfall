// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadGameplayDefinitions.h"
#include "MadInventory.h"
#include "MadTrading.h"
#include "Math/RandomStream.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadTradingTests
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
	FMadTradingRulesTest,
	"MadFall.Items.Trading",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadTradingRulesTest::RunTest(const FString& Parameters)
{
	using namespace MadTradingTests;
	const FName Mod(TEXT("test"));
	FMadGameplayDefinitions Defs;
	TArray<FMadDefinitionError> Errors;
	Defs.BeginLoad();
	Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:coin","max_stack":100,"value":1})")), TEXT("items.json"), Mod, Errors);
	Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:plank","max_stack":20,"value":2,"tags":["item.wood"]})")), TEXT("items.json"), Mod, Errors);
	Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:junk","max_stack":20})")), TEXT("items.json"), Mod, Errors);
	Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:pick","value":40,"tool":{"durability":10}})")), TEXT("items.json"), Mod, Errors);
	TestTrue(TEXT("trader parses"), Defs.AddTraderJson(Json(TEXT(R"({"schema":"madfall.trader/1","id":"test:rosa","currency":"test:coin",
		"buy_factor":0.5,"restock_days":2,
		"stock":[{"item":"test:plank","count":5},{"item":"test:pick","price":50},{"item":"test:nothing"},{"item":"test:junk"}]})")), TEXT("traders.json"), Mod, Errors));
	TestFalse(TEXT("a buy factor above 1 is refused"), Defs.AddTraderJson(Json(TEXT(R"({"schema":"madfall.trader/1","id":"test:greedy","buy_factor":2})")), TEXT("t.json"), Mod, Errors)
		&& Errors.Num() == 0);
	Defs.FinishLoad(nullptr, Errors);

	auto HasError = [&Errors](const TCHAR* Text)
	{
		return Errors.ContainsByPredicate([Text](const FMadDefinitionError& Error) { return Error.ToString().Contains(Text); });
	};
	TestTrue(TEXT("an unknown stock item is reported"), HasError(TEXT("unknown item 'test:nothing'")));
	TestTrue(TEXT("a line with no value and no price is reported"), HasError(TEXT("has no value and the line names no price")));

	const FMadTraderDefinition* Rosa = Defs.FindTrader(FName(TEXT("test:rosa")));
	if (!TestNotNull(TEXT("trader registered"), Rosa))
	{
		return false;
	}
	TestEqual(TEXT("bad lines dropped"), Rosa->Stock.Num(), 2);
	const FName Coin(TEXT("test:coin"));
	const FName Plank(TEXT("test:plank"));
	const FName Pick(TEXT("test:pick"));

	TestEqual(TEXT("price is the item value"), Defs.GetTraderPrice(*Rosa, Plank), 2);
	TestEqual(TEXT("or the line's price"), Defs.GetTraderPrice(*Rosa, Pick), 50);
	TestEqual(TEXT("offer is value x buy factor"), Defs.GetTraderOffer(*Rosa, Pick), 20);
	TestEqual(TEXT("never less than 1 for something with value"), Defs.GetTraderOffer(*Rosa, Plank), 1);
	TestEqual(TEXT("the currency is not bought"), Defs.GetTraderOffer(*Rosa, Coin), 0);
	TestEqual(TEXT("valueless items are not bought"), Defs.GetTraderOffer(*Rosa, FName(TEXT("test:junk"))), 0);

	// --- restock ---
	FMadTraderState State;
	FRandomStream RandomA(42);
	MadFall::Trade::Restock(State, *Rosa, Defs, RandomA, 3);
	TestEqual(TEXT("restocks again after restock_days"), State.RestockDay, 5);
	TestEqual(TEXT("five planks on the shelf"), State.Stock.CountItem(Plank), 5);
	TestEqual(TEXT("and the pick"), State.Stock.CountItem(Pick), 1);
	FMadTraderState Again;
	FRandomStream RandomB(42);
	MadFall::Trade::Restock(Again, *Rosa, Defs, RandomB, 3);
	TestTrue(TEXT("the same seed stocks the same shelves"), State.Stock.GetSlots() == Again.Stock.GetSlots());

	int32 PlankSlot = INDEX_NONE;
	for (int32 Slot = 0; Slot < State.Stock.NumSlots(); ++Slot)
	{
		if (State.Stock.GetSlot(Slot).Item == Plank)
		{
			PlankSlot = Slot;
		}
	}

	// --- buying ---
	FMadInventory Backpack(4);
	Backpack.Add(FMadItemStack::Make(*Defs.FindItem(Coin), 5), Defs);
	int32 Total = 0;
	TestTrue(TEXT("cannot afford four planks with five coins"), MadFall::Trade::Buy(Backpack, State, PlankSlot, 4, *Rosa, Defs, Total) == EMadTradeResult::CannotAfford);
	TestEqual(TEXT("nothing paid"), Backpack.CountItem(Coin), 5);
	TestTrue(TEXT("buy two"), MadFall::Trade::Buy(Backpack, State, PlankSlot, 2, *Rosa, Defs, Total) == EMadTradeResult::Ok);
	TestEqual(TEXT("paid four"), Total, 4);
	TestEqual(TEXT("one coin left"), Backpack.CountItem(Coin), 1);
	TestEqual(TEXT("two planks carried"), Backpack.CountItem(Plank), 2);
	TestEqual(TEXT("three left on the shelf"), State.Stock.CountItem(Plank), 3);
	TestTrue(TEXT("an empty shelf slot is not for sale"), MadFall::Trade::Buy(Backpack, State, 23, 1, *Rosa, Defs, Total) == EMadTradeResult::NotForSale);

	// A full backpack: nothing changes, not even the coins.
	FMadInventory Full(2);
	Full.Add(FMadItemStack::Make(*Defs.FindItem(Coin), 100), Defs);
	Full.SetSlot(1, FMadItemStack::Make(*Defs.FindItem(FName(TEXT("test:junk"))), 20));
	const TArray<FMadItemStack> Before = Full.GetSlots();
	TestTrue(TEXT("no room"), MadFall::Trade::Buy(Full, State, PlankSlot, 1, *Rosa, Defs, Total) == EMadTradeResult::NoSpace);
	TestTrue(TEXT("a failed buy changes nothing"), Full.GetSlots() == Before);
	TestEqual(TEXT("and leaves the shelf"), State.Stock.CountItem(Plank), 3);

	// --- selling ---
	FMadInventory Seller(4);
	FMadItemStack Worn = FMadItemStack::Make(*Defs.FindItem(Pick), 1);
	Worn.Durability = 5;
	Seller.SetSlot(0, Worn);
	Seller.SetSlot(1, FMadItemStack::Make(*Defs.FindItem(Coin), 3));
	TestEqual(TEXT("a half-worn pick fetches half"), MadFall::Trade::GetUnitOffer(Worn, *Rosa, Defs), 10);
	TestTrue(TEXT("sell the pick"), MadFall::Trade::Sell(Seller, 0, 1, State, *Rosa, Defs, Total) == EMadTradeResult::Ok);
	TestEqual(TEXT("earned ten"), Total, 10);
	TestEqual(TEXT("coins added"), Seller.CountItem(Coin), 13);
	TestEqual(TEXT("the pick is gone from the backpack"), Seller.CountItem(Pick), 0);
	TestEqual(TEXT("and on the shelf"), State.Stock.CountItem(Pick), 2);
	TestTrue(TEXT("the currency cannot be sold"), MadFall::Trade::Sell(Seller, 1, 1, State, *Rosa, Defs, Total) == EMadTradeResult::NotBought);
	return true;
}

#endif

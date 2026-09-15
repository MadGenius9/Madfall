// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadGameplayDefinitions.h"
#include "MadGameplaySave.h"
#include "MadSurvivalModel.h"
#include "MadSurvivorComponents.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWearTest,
	"MadFall.Items.Wear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWearTest::RunTest(const FString& Parameters)
{
	auto Json = [](const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	};

	// Parsing, and the mistakes a modder makes.
	{
		FMadItemDefinition Coat;
		TArray<FMadDefinitionError> Errors;
		MadFall::GameplayDefinitionsJson::ParseItem(Json(TEXT(R"({ "schema": "madfall.item/1", "id": "test:coat",
			"wear": { "slot": "body", "cold": 10, "heat": 1, "armor": 0.2 } })")), TEXT("coat"), FName(TEXT("test")), Coat, Errors);
		TestEqual(TEXT("coat parses cleanly"), Errors.Num(), 0);
		TestTrue(TEXT("is wearable"), Coat.bHasWear);
		TestEqual(TEXT("slot"), Coat.Wear.Slot, FName(TEXT("body")));
		TestEqual(TEXT("cold"), Coat.Wear.Cold, 10.0f);
		TestEqual(TEXT("armor"), Coat.Wear.Armor, 0.2f);

		FMadItemDefinition Bad;
		TArray<FMadDefinitionError> BadErrors;
		MadFall::GameplayDefinitionsJson::ParseItem(Json(TEXT(R"({ "schema": "madfall.item/1", "id": "test:glove",
			"wear": { "slot": "hands", "armor": 2.0 } })")), TEXT("glove"), FName(TEXT("test")), Bad, BadErrors);
		TestTrue(TEXT("an unknown slot is reported"), BadErrors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.ToString().Contains(TEXT("/wear/slot")); }));
		TestTrue(TEXT("too much armour is reported"), BadErrors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.ToString().Contains(TEXT("/wear/armor")); }));
		TestFalse(TEXT("and an unknown slot is not wearable"), Bad.bHasWear);
	}

	TestEqual(TEXT("head is slot 0"), MadFall::Wear::GetSlotIndex(FName(TEXT("head"))), 0);
	TestEqual(TEXT("feet is slot 3"), MadFall::Wear::GetSlotIndex(FName(TEXT("feet"))), 3);
	TestEqual(TEXT("slot names round-trip"), MadFall::Wear::GetSlotName(2), FName(TEXT("legs")));

	// Wearing shipped clothes: swaps, totals, and the cap.
	UMadInventoryComponent* Inventory = NewObject<UMadInventoryComponent>();
	Inventory->AddItem(FName(TEXT("madfall:fur_coat")), 1);
	Inventory->AddItem(FName(TEXT("madfall:scrap_vest")), 1);
	Inventory->AddItem(FName(TEXT("madfall:scrap_helmet")), 1);
	Inventory->AddItem(FName(TEXT("madfall:rock")), 3);

	Inventory->SelectSlot(3);
	TestFalse(TEXT("a rock cannot be worn"), Inventory->WearSelected());

	Inventory->SelectSlot(0);
	TestTrue(TEXT("the coat is worn"), Inventory->WearSelected());
	TestTrue(TEXT("its hotbar slot is empty"), Inventory->GetInventory().GetSlot(0).IsEmpty());
	TestEqual(TEXT("it is on the body"), Inventory->GetWorn().GetSlot(1).Item, FName(TEXT("madfall:fur_coat")));

	Inventory->SelectSlot(1);
	TestTrue(TEXT("the vest goes on"), Inventory->WearSelected());
	TestEqual(TEXT("the vest replaces the coat"), Inventory->GetWorn().GetSlot(1).Item, FName(TEXT("madfall:scrap_vest")));
	TestEqual(TEXT("the coat comes back to the hand"), Inventory->GetInventory().GetSlot(1).Item, FName(TEXT("madfall:fur_coat")));

	Inventory->SelectSlot(2);
	Inventory->WearSelected();
	const FMadWearStats Totals = Inventory->GetWearTotals();
	const FMadItemDefinition* Vest = MadFall::GetGameplayDefinitions().FindItem(FName(TEXT("madfall:scrap_vest")));
	const FMadItemDefinition* Helmet = MadFall::GetGameplayDefinitions().FindItem(FName(TEXT("madfall:scrap_helmet")));
	if (TestNotNull(TEXT("vest"), Vest) && TestNotNull(TEXT("helmet"), Helmet))
	{
		TestEqual(TEXT("armour adds up"), Totals.Armor, FMath::Min(MadFall::Wear::MaxArmor, Vest->Wear.Armor + Helmet->Wear.Armor), 1e-4f);
		TestEqual(TEXT("warmth adds up"), Totals.Cold, Vest->Wear.Cold + Helmet->Wear.Cold, 1e-4f);
	}

	// Armour absorbs attacks but never all of them.
	UMadSurvivalComponent* Survival = NewObject<UMadSurvivalComponent>();
	FMadWearStats Plated;
	Plated.Armor = 5.0f;
	Survival->SetWear(Plated);
	TestEqual(TEXT("armour is capped"), Survival->GetArmor(), MadFall::Wear::MaxArmor);

	// Warm clothing keeps the core warm in a cold that would chill a bare survivor.
	{
		const FMadSurvivalTuning Tuning;
		FMadSurvivalStats Bare;
		FMadSurvivalStats Dressed;
		FMadSurvivalEnvironment Cold;
		Cold.AmbientTemperature = -5.0f;
		FMadSurvivalEnvironment Warm = Cold;
		const FMadItemDefinition* Coat = MadFall::GetGameplayDefinitions().FindItem(FName(TEXT("madfall:fur_coat")));
		const FMadItemDefinition* Hat = MadFall::GetGameplayDefinitions().FindItem(FName(TEXT("madfall:fur_hat")));
		Warm.ColdInsulation = (Coat ? Coat->Wear.Cold : 0.0f) + (Hat ? Hat->Wear.Cold : 0.0f);
		MadFall::Survival::Step(Bare, Cold, Tuning, 120.0f);
		MadFall::Survival::Step(Dressed, Warm, Tuning, 120.0f);
		TestTrue(*FString::Printf(TEXT("dressed core %.2f C stays warmer than bare %.2f C"), Dressed.CoreTemperature, Bare.CoreTemperature),
			Dressed.CoreTemperature > Bare.CoreTemperature + 0.1f);
	}

	// Worn items survive a save, including ones from removed mods.
	FMadGameplaySave Save;
	Save.bHasPlayer = true;
	Save.Player.Worn = { FMadItemStack{ FName(TEXT("madfall:fur_hat")), 1, -1, {} }, FMadItemStack(),
		FMadItemStack{ FName(TEXT("gonemod:kilt")), 1, -1, {} }, FMadItemStack() };
	FMadGameplaySave Loaded;
	TArray<FString> Warnings;
	TestTrue(TEXT("save parses"), MadFall::GameplaySave::FromJson(MadFall::GameplaySave::ToJson(Save), Loaded, Warnings));
	TestTrue(TEXT("worn slots round-trip"), Loaded.Player.Worn.Num() == 4
		&& Loaded.Player.Worn[0].Item == FName(TEXT("madfall:fur_hat")) && Loaded.Player.Worn[2].Item == FName(TEXT("gonemod:kilt")));
	return true;
}

#endif

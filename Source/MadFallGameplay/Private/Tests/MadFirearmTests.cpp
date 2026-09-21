// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadGameplayDefinitions.h"
#include "MadItemIcons.h"
#include "MadPlayerCharacter.h"
#include "MadViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadNoiseCarriesTest,
	"MadFall.Combat.NoiseCarries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadNoiseCarriesTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Noise;
	// A swing reaches exactly a listener's hearing range and no further.
	TestTrue(TEXT("a swing is heard at the edge of hearing"), CarriesTo(20.0f, 20.0f, 1.0f));
	TestFalse(TEXT("a swing is not heard past it"), CarriesTo(21.0f, 20.0f, 1.0f));
	// A gunshot at loudness 3 reaches three times as far - the whole trade.
	TestTrue(TEXT("a gunshot is heard at three times the range"), CarriesTo(59.0f, 20.0f, 3.0f));
	TestFalse(TEXT("but not four"), CarriesTo(80.0f, 20.0f, 3.0f));
	// Silence is heard by nobody, however close.
	TestFalse(TEXT("silence is not heard even adjacent"), CarriesTo(0.0f, 20.0f, 0.0f));

	// A swing just after a shot does not make the shot quieter.
	TestEqual(TEXT("a quiet noise inside the window keeps the loud one"), Combine(3.0f, 0.5, 1.0f), 3.0f);
	TestEqual(TEXT("a louder noise raises it"), Combine(1.0f, 0.5, 3.0f), 3.0f);
	TestEqual(TEXT("after the window a new noise stands alone"), Combine(3.0f, 2.0, 1.0f), 1.0f);
	TestEqual(TEXT("heard within the window"), Heard(3.0f, 1.0), 3.0f);
	TestEqual(TEXT("silent after it"), Heard(3.0f, 1.6), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadFirearmContentTest,
	"MadFall.Items.Firearms",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadFirearmContentTest::RunTest(const FString& Parameters)
{
	const FMadGameplayDefinitions& Defs = MadFall::GetGameplayDefinitions();

	// Something a player can actually get: crafted, looted or bought.
	auto Obtainable = [&Defs](FName Item)
	{
		for (const FMadRecipeDefinition& Recipe : Defs.GetRecipes())
		{
			if (Recipe.Output.Item == Item) { return true; }
		}
		for (const FMadLootTableDefinition& Table : Defs.GetLootTables())
		{
			for (const FMadLootEntry& Entry : Table.Entries)
			{
				if (Entry.Item == Item) { return true; }
			}
		}
		for (const FMadTraderDefinition& Trader : Defs.GetTraders())
		{
			for (const FMadTraderStockEntry& Stock : Trader.Stock)
			{
				if (Stock.Item == Item) { return true; }
			}
		}
		return false;
	};

	// Every ranged weapon's ammunition can be had, or the weapon is a club.
	for (const FMadItemDefinition& Item : Defs.GetItems())
	{
		if (Item.bHasTool && Item.Tool.IsRanged())
		{
			TestTrue(FString::Printf(TEXT("%s ammo %s exists"), *Item.Id.ToString(), *Item.Tool.Ammo.ToString()), Defs.FindItem(Item.Tool.Ammo) != nullptr);
			TestTrue(FString::Printf(TEXT("%s ammo %s is obtainable"), *Item.Id.ToString(), *Item.Tool.Ammo.ToString()), Obtainable(Item.Tool.Ammo));
		}
	}

	const FMadItemDefinition* Pistol = Defs.FindItem(FName(TEXT("madfall:pistol")));
	const FMadItemDefinition* Bow = Defs.FindItem(FName(TEXT("madfall:recurve_bow")));
	if (!TestNotNull(TEXT("pistol"), Pistol) || !TestNotNull(TEXT("recurve bow"), Bow))
	{
		return false;
	}
	TestTrue(TEXT("the pistol is ranged"), Pistol->bHasTool && Pistol->Tool.IsRanged());
	TestTrue(TEXT("the pistol is obtainable"), Obtainable(Pistol->Id));
	// The trade: hits harder than any bow, and is heard much further.
	TestTrue(TEXT("the pistol outdamages the best bow"),
		Pistol->Tool.Damage.FindRef(FName(TEXT("madfall:pierce"))) > Bow->Tool.Damage.FindRef(FName(TEXT("madfall:pierce"))));
	TestTrue(TEXT("a gunshot carries at least twice a swing"), Pistol->Tool.Noise >= 2.0f);
	TestEqual(TEXT("every bow stays silent"), Bow->Tool.Noise, 0.0f);
	TestEqual(TEXT("a spent round is not picked up again"), Pistol->Tool.RecoverChance, 0.0f);

	// Drawn as a gun, not a bow.
	TestEqual(TEXT("the pistol is held as a gun"), MadFall::ViewModel::ChooseShape(Pistol), EMadHeldShape::Gun);
	TestEqual(TEXT("the bow is still held as a bow"), MadFall::ViewModel::ChooseShape(Bow), EMadHeldShape::Bow);
	TestTrue(TEXT("the pistol's icon is a pistol"), MadFall::Icons::ChooseShape(*Pistol) == MadFall::Icons::EShape::Pistol);
	if (const FMadItemDefinition* Round = Defs.FindItem(Pistol->Tool.Ammo))
	{
		TestTrue(TEXT("a round's icon is a bullet"), MadFall::Icons::ChooseShape(*Round) == MadFall::Icons::EShape::Bullet);
	}
	return true;
}

#endif

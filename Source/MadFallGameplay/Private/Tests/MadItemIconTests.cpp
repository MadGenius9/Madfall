// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadGameplayDefinitions.h"
#include "MadItemIcons.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadItemIconTest,
	"MadFall.Items.Icons",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadItemIconTest::RunTest(const FString& Parameters)
{
	using MadFall::Icons::EShape;
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();

	auto Opaque = [](const TArray<FColor>& Pixels)
	{
		int32 Count = 0;
		for (const FColor& C : Pixels)
		{
			Count += C.A > 0 ? 1 : 0;
		}
		return Count;
	};

	// Every shipped and test-mod item paints a picture: not blank, not a filled square.
	int32 Items = 0;
	TSet<uint32> Distinct;
	for (const FMadItemDefinition& Definition : Definitions.GetItems())
	{
		const FMadItemDefinition* Item = &Definition;
		const FName Id = Item->Id;
		TArray<FColor> Pixels;
		MadFall::Icons::Paint(*Item, Pixels);
		TestEqual(FString::Printf(TEXT("%s: 32x32"), *Id.ToString()), Pixels.Num(), MadFall::Icons::Size * MadFall::Icons::Size);
		const int32 Count = Opaque(Pixels);
		TestTrue(FString::Printf(TEXT("%s: a picture (%d opaque pixels)"), *Id.ToString(), Count), Count >= 40 && Count < MadFall::Icons::Size * MadFall::Icons::Size);

		TArray<FColor> Again;
		MadFall::Icons::Paint(*Item, Again);
		TestTrue(FString::Printf(TEXT("%s: deterministic"), *Id.ToString()), Pixels == Again);
		Distinct.Add(FCrc::MemCrc32(Pixels.GetData(), Pixels.Num() * sizeof(FColor)));
		++Items;
	}
	TestTrue(FString::Printf(TEXT("items were painted (%d)"), Items), Items > 20);
	TestTrue(FString::Printf(TEXT("most items look different (%d distinct of %d)"), Distinct.Num(), Items), Distinct.Num() * 10 >= Items * 8);

	// Shapes follow the data.
	auto ShapeOf = [&Definitions](const TCHAR* Id)
	{
		const FMadItemDefinition* Item = Definitions.FindItem(FName(Id));
		return Item ? MadFall::Icons::ChooseShape(*Item) : EShape::Lump;
	};
	TestTrue(TEXT("a pickaxe is a pickaxe"), ShapeOf(TEXT("madfall:stone_pickaxe")) == EShape::Pickaxe);
	TestTrue(TEXT("a bow is a bow"), ShapeOf(TEXT("madfall:wooden_bow")) == EShape::Bow);
	TestTrue(TEXT("canned food is a can"), ShapeOf(TEXT("madfall:canned_food")) == EShape::Can);
	TestTrue(TEXT("water is a bottle"), ShapeOf(TEXT("madfall:water_bottle")) == EShape::Bottle);
	TestTrue(TEXT("a helmet goes on the head"), ShapeOf(TEXT("madfall:scrap_helmet")) == EShape::Hat);
	TestTrue(TEXT("seeds are seeds, not the block they plant"), ShapeOf(TEXT("madfall:corn_seed")) == EShape::Seeds);
	TestTrue(TEXT("a coin is a coin"), ShapeOf(TEXT("madfall:coin")) == EShape::Coin);
	TestTrue(TEXT("a block item is a cube"), ShapeOf(TEXT("madfall:wood_frame")) == EShape::Block);

	// A block's cube wears its surface colour: wood is warm, concrete grey.
	auto TopCentre = [&Definitions](const TCHAR* Id)
	{
		TArray<FColor> Pixels;
		if (const FMadItemDefinition* Item = Definitions.FindItem(FName(Id)))
		{
			MadFall::Icons::Paint(*Item, Pixels);
			return Pixels[9 * MadFall::Icons::Size + 16];
		}
		return FColor::Black;
	};
	const FColor Wood = TopCentre(TEXT("madfall:wood_frame"));
	const FColor Concrete = TopCentre(TEXT("madfall:concrete_frame"));
	TestTrue(FString::Printf(TEXT("wood is warm (%s)"), *Wood.ToString()), Wood.R > Wood.B + 30);
	TestTrue(FString::Printf(TEXT("concrete is grey (%s)"), *Concrete.ToString()), FMath::Abs(Concrete.R - Concrete.B) < 25);
	return true;
}

#endif

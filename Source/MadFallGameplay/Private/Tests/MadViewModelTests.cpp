// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MadSurfaceRegistry.h"
#include "Engine/StaticMesh.h"

#include "MadGameplayDefinitions.h"
#include "MadViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadViewModelTest,
	"MadFall.Items.ViewModel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadViewModelTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::ViewModel;
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();

	auto ShapeOf = [&Definitions](const TCHAR* Id) { return ChooseShape(Definitions.FindItem(FName(Id))); };
	TestEqual(TEXT("nothing held is an empty hand"), ChooseShape(nullptr), EMadHeldShape::Empty);
	TestEqual(TEXT("stone pickaxe"), ShapeOf(TEXT("madfall:stone_pickaxe")), EMadHeldShape::Pickaxe);
	TestEqual(TEXT("stone axe"), ShapeOf(TEXT("madfall:stone_axe")), EMadHeldShape::Axe);
	TestEqual(TEXT("stone shovel"), ShapeOf(TEXT("madfall:stone_shovel")), EMadHeldShape::Shovel);
	TestEqual(TEXT("stone hoe"), ShapeOf(TEXT("madfall:stone_hoe")), EMadHeldShape::Hoe);
	TestEqual(TEXT("a ranged weapon is drawn as a bow"), ShapeOf(TEXT("madfall:wooden_bow")), EMadHeldShape::Bow);
	TestEqual(TEXT("seeds are held as the block they plant"), ShapeOf(TEXT("madfall:corn_seed")), EMadHeldShape::Block);
	TestEqual(TEXT("a weapon without a tool tag is a club"), ShapeOf(TEXT("madfall:wooden_club")), EMadHeldShape::Club);
	TestEqual(TEXT("a placeable block"), ShapeOf(TEXT("madfall:wood_frame")), EMadHeldShape::Block);
	TestEqual(TEXT("food"), ShapeOf(TEXT("madfall:canned_food")), EMadHeldShape::Food);
	TestEqual(TEXT("drink"), ShapeOf(TEXT("madfall:water_bottle")), EMadHeldShape::Drink);
	TestEqual(TEXT("a resource"), ShapeOf(TEXT("madfall:plant_fiber")), EMadHeldShape::Resource);

	// Every shipped item gets a shape without falling through to nonsense.
	int32 Items = 0;
	for (const FMadItemDefinition& Item : Definitions.GetItems())
	{
		++Items;
		const EMadHeldShape Shape = ChooseShape(&Item);
		if ((Item.Kind == EMadItemKind::Tool || Item.Kind == EMadItemKind::Weapon)
			&& Shape != EMadHeldShape::Pickaxe && Shape != EMadHeldShape::Axe && Shape != EMadHeldShape::Shovel && Shape != EMadHeldShape::Hoe && Shape != EMadHeldShape::Bow && Shape != EMadHeldShape::Club)
		{
			AddError(FString::Printf(TEXT("%s is a tool drawn as a non-tool"), *Item.Id.ToString()));
		}
	}
	TestTrue(TEXT("items checked"), Items > 10);

	// Every shape with a hand-built model has one that loads, and its slots are
	// surfaces or the tool head - a typo would silently draw the preview material.
	for (EMadHeldShape Shape : { EMadHeldShape::Pickaxe, EMadHeldShape::Axe, EMadHeldShape::Shovel, EMadHeldShape::Hoe,
		EMadHeldShape::Bow, EMadHeldShape::Club, EMadHeldShape::Food, EMadHeldShape::Drink })
	{
		const TCHAR* Path = MadFall::ViewModel::GetHeldModelPath(Shape);
		const UStaticMesh* Model = Path ? LoadObject<UStaticMesh>(nullptr, Path) : nullptr;
		if (!TestNotNull(*FString::Printf(TEXT("shape %d has a held model"), static_cast<int32>(Shape)), Model))
		{
			continue;
		}
		for (const FStaticMaterial& Slot : Model->GetStaticMaterials())
		{
			TestTrue(*FString::Printf(TEXT("%s slot %s is a surface or the head"), Path, *Slot.MaterialSlotName.ToString()),
				Slot.MaterialSlotName == FName(TEXT("head")) || MadFall::GetSurfaces().Find(Slot.MaterialSlotName) != nullptr);
		}
	}
	TestTrue(TEXT("a held block keeps its textured cube"), MadFall::ViewModel::GetHeldModelPath(EMadHeldShape::Block) == nullptr);

	// At rest there is no offset; mid-swing the tool is well forward and down.
	const FTransform Rest = ComputeOffset(1.0f, 1.0f, 0.0f, 0.0f);
	TestTrue(TEXT("rest is identity"), Rest.Equals(FTransform::Identity, 1e-3));
	const FTransform Swing = ComputeOffset(0.4f, 1.0f, 0.0f, 0.0f);
	TestTrue(TEXT("mid-swing pitches the tool forward"), Swing.Rotator().Pitch < -40.0);
	TestTrue(TEXT("mid-swing pushes it forward"), Swing.GetLocation().X > 4.0);
	const FTransform Use = ComputeOffset(1.0f, 0.5f, 0.0f, 0.0f);
	TestTrue(TEXT("placing pushes the item out"), Use.GetLocation().X > 8.0);
	TestTrue(TEXT("walking bobs"), !ComputeOffset(1.0f, 1.0f, 1.0f, 1.0f).GetLocation().IsNearlyZero(0.1));
	TestTrue(TEXT("standing still does not"), ComputeOffset(1.0f, 1.0f, 1.0f, 0.0f).GetLocation().IsNearlyZero(1e-3));

	return true;
}

#endif

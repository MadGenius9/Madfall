// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadGameplayDefinitions.h"
#include "MadProjectile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadProjectileTest,
	"MadFall.Items.Projectile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadProjectileTest::RunTest(const FString& Parameters)
{
	using MadFall::Projectile::GravityCm;

	// A level shot at 45 voxels a second with half gravity, stepped at 60 Hz for
	// half a second, lands where the closed-form parabola says, within a few cm.
	FVector Location = FVector::ZeroVector;
	FVector Velocity(4500.0, 0.0, 0.0);
	const float Dt = 1.0f / 60.0f;
	for (int32 Step = 0; Step < 30; ++Step)
	{
		MadFall::Projectile::Step(Location, Velocity, 0.5f, Dt);
	}
	const float T = 30 * Dt;
	TestEqual(TEXT("travels speed x time"), Location.X, 4500.0 * T, 1.0);
	TestEqual(TEXT("falls half-g t^2 / 2"), Location.Z, -0.5 * GravityCm * 0.5 * T * T, 5.0);
	TestEqual(TEXT("vertical speed grows linearly"), Velocity.Z, -GravityCm * 0.5 * T, 0.5);

	// Over 20 voxels the drop is under a voxel: close shots need no lead.
	TestTrue(TEXT("a 20-voxel shot drops less than a voxel"), 0.5 * GravityCm * 0.5 * FMath::Square(2000.0 / 4500.0) < 100.0);

	// No gravity, no drop.
	FVector Flat = FVector::ZeroVector;
	FVector FlatVelocity(0.0, 1000.0, 0.0);
	MadFall::Projectile::Step(Flat, FlatVelocity, 0.0f, 0.1f);
	TestEqual(TEXT("zero gravity flies straight"), Flat, FVector(0.0, 100.0, 0.0));

	// The shipped bow is ranged and its ammo exists; a club is not ranged.
	const FMadGameplayDefinitions& Defs = MadFall::GetGameplayDefinitions();
	const FMadItemDefinition* Bow = Defs.FindItem(FName(TEXT("madfall:wooden_bow")));
	if (TestNotNull(TEXT("wooden bow"), Bow))
	{
		TestTrue(TEXT("the bow is ranged"), Bow->bHasTool && Bow->Tool.IsRanged());
		TestNotNull(TEXT("its ammo exists"), Defs.FindItem(Bow->Tool.Ammo));
		TestTrue(TEXT("an arrow kills a rabbit"), Bow->Tool.Damage.FindRef(FName(TEXT("madfall:pierce"))) >= 12.0f);
		TestNotNull(TEXT("arrows are craftable"), Defs.FindRecipe(FName(TEXT("madfall:arrow"))));
	}
	const FMadItemDefinition* Club = Defs.FindItem(FName(TEXT("madfall:wooden_club")));
	TestTrue(TEXT("the club is melee"), Club != nullptr && !Club->Tool.IsRanged());
	return true;
}

#endif

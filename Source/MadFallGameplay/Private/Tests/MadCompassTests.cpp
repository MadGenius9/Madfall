// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadCompass.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadCompassTest,
	"MadFall.World.Compass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadCompassTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Compass;

	TestEqual(TEXT("+X is north"), BearingTo(FVector::ZeroVector, FVector(100.0, 0.0, 0.0)), 0.0f, 1e-3f);
	TestEqual(TEXT("+Y is east"), BearingTo(FVector::ZeroVector, FVector(0.0, 100.0, 0.0)), 90.0f, 1e-3f);
	TestEqual(TEXT("-X is south"), BearingTo(FVector::ZeroVector, FVector(-100.0, 0.0, 50.0)), 180.0f, 1e-3f);
	TestEqual(TEXT("-Y is west"), BearingTo(FVector::ZeroVector, FVector(0.0, -100.0, 0.0)), 270.0f, 1e-3f);

	TestEqual(TEXT("N"), FString(GetHeadingName(2.0f)), FString(TEXT("N")));
	TestEqual(TEXT("NE"), FString(GetHeadingName(40.0f)), FString(TEXT("NE")));
	TestEqual(TEXT("N again past 337.5"), FString(GetHeadingName(350.0f)), FString(TEXT("N")));
	TestEqual(TEXT("W"), FString(GetHeadingName(-90.0f)), FString(TEXT("W")));

	// Looking north with a 60-degree half span.
	TestTrue(TEXT("straight ahead is the centre"), ProjectBearing(0.0f, 0.0f, 60.0f).IsSet() && FMath::IsNearlyZero(ProjectBearing(0.0f, 0.0f, 60.0f).GetValue()));
	TestEqual(TEXT("30 degrees right is halfway right"), ProjectBearing(0.0f, 30.0f, 60.0f).Get(-9.0f), 0.5f, 1e-4f);
	TestEqual(TEXT("wraps across north: 350 is left"), ProjectBearing(10.0f, 350.0f, 60.0f).Get(-9.0f), -20.0f / 60.0f, 1e-4f);
	TestFalse(TEXT("behind is off the strip"), ProjectBearing(0.0f, 180.0f, 60.0f).IsSet());
	TestFalse(TEXT("just past the edge is off the strip"), ProjectBearing(0.0f, 61.0f, 60.0f).IsSet());
	return true;
}

#endif

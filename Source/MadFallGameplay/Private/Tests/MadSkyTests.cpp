// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadSkySubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSkyComputeTest,
	"MadFall.World.SkyCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSkyComputeTest::RunTest(const FString& Parameters)
{
	auto TowardLight = [](const FRotator& Rotation) { return -Rotation.Vector(); };

	const FMadSkyState Noon = MadFall::Sky::Compute(12.0f, 10.0f, 0.3f);
	TestTrue(TEXT("the noon sun is up"), TowardLight(Noon.SunRotation).Z > 0.8);
	TestTrue(TEXT("but not at the zenith, so walls and trunks are lit"), TowardLight(Noon.SunRotation).Z < 0.95);
	TestEqual(TEXT("full sun at noon"), Noon.SunIntensity, 10.0f);
	TestEqual(TEXT("no moon at noon"), Noon.MoonIntensity, 0.0f);

	const FMadSkyState Midnight = MadFall::Sky::Compute(0.0f, 10.0f, 0.3f);
	TestEqual(TEXT("no sun at midnight"), Midnight.SunIntensity, 0.0f);
	TestEqual(TEXT("full moon at midnight"), Midnight.MoonIntensity, 0.3f);
	TestTrue(TEXT("the moon is up at midnight"), TowardLight(Midnight.MoonRotation).Z > 0.8);

	const FMadSkyState Morning = MadFall::Sky::Compute(8.0f, 10.0f, 0.3f);
	const FMadSkyState Evening = MadFall::Sky::Compute(16.0f, 10.0f, 0.3f);
	TestTrue(TEXT("the sun is in the east in the morning (+Y)"), TowardLight(Morning.SunRotation).Y > 0.3);
	TestTrue(TEXT("and in the west in the evening"), TowardLight(Evening.SunRotation).Y < -0.3);
	TestTrue(TEXT("morning and evening are symmetric in height"),
		FMath::IsNearlyEqual(Morning.SunHeight, Evening.SunHeight, 0.001f));

	// No jumps: an hour-by-minute sweep never moves the light more than a
	// couple of degrees or changes intensity abruptly.
	FMadSkyState Previous = MadFall::Sky::Compute(0.0f, 10.0f, 0.3f);
	for (int32 Minute = 1; Minute <= 24 * 60; ++Minute)
	{
		const FMadSkyState Current = MadFall::Sky::Compute(static_cast<float>(Minute) / 60.0f, 10.0f, 0.3f);
		const double Degrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
			FVector::DotProduct(Current.SunRotation.Vector(), Previous.SunRotation.Vector()), -1.0, 1.0)));
		if (Degrees > 1.0 || FMath::Abs(Current.SunIntensity - Previous.SunIntensity) > 0.5f)
		{
			AddError(FString::Printf(TEXT("discontinuity at minute %d: %.2f degrees, sun %.2f -> %.2f lux"),
				Minute, Degrees, Previous.SunIntensity, Current.SunIntensity));
			break;
		}
		// Never both lights at full strength, and never a lit sun below the horizon.
		if (Current.SunIntensity > 0.0f && Current.SunHeight < -0.05f)
		{
			AddError(FString::Printf(TEXT("minute %d: sun lit %.2f while %.2f below the horizon"), Minute, Current.SunIntensity, Current.SunHeight));
			break;
		}
		Previous = Current;
	}

	return true;
}

#endif

// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadHumanoidRig.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadHumanoidPoseTest,
	"MadFall.AI.HumanoidPose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadHumanoidPoseTest::RunTest(const FString& Parameters)
{
	using MadFall::Humanoid::ComputePose;

	const FMadHumanoidPose Standing = ComputePose(1.3f, 0.0f, 1.0f, 0.0f);
	TestEqual(TEXT("standing still, the legs do not swing"), Standing.LeftLeg, 0.0f);
	TestTrue(TEXT("arms reach forward (the shamble)"), Standing.LeftArm < -60.0f && Standing.RightArm < -60.0f);
	TestEqual(TEXT("standing, not fallen"), Standing.Fall, 0.0f);

	const FMadHumanoidPose Striding = ComputePose(UE_HALF_PI, 1.0f, 1.0f, 0.0f);
	TestTrue(TEXT("legs swing opposite ways mid-stride"), Striding.LeftLeg > 20.0f && FMath::IsNearlyEqual(Striding.RightLeg, -Striding.LeftLeg));

	const FMadHumanoidPose Striking = ComputePose(0.0f, 0.0f, 0.5f, 0.0f);
	TestTrue(TEXT("mid-attack the arms are raised higher than the shamble"), Striking.LeftArm < Standing.LeftArm - 40.0f);
	const FMadHumanoidPose Recovered = ComputePose(0.0f, 0.0f, 1.0f, 0.0f);
	TestTrue(TEXT("after the swing the arms are back"), FMath::IsNearlyEqual(Recovered.LeftArm, ComputePose(0.0f, 0.0f, 1.0f, 0.0f).LeftArm) && Recovered.LeftArm > Striking.LeftArm);

	TestEqual(TEXT("fully dead is flat on the ground"), ComputePose(0.0f, 0.0f, 1.0f, 1.0f).Fall, -90.0f);
	TestTrue(TEXT("the fall accelerates"), FMath::Abs(ComputePose(0.0f, 0.0f, 1.0f, 0.5f).Fall) < 45.0f);

	// Out-of-range inputs clamp instead of folding the figure in half.
	const FMadHumanoidPose Wild = ComputePose(0.7f, 50.0f, -3.0f, 9.0f);
	TestTrue(TEXT("speed clamps"), FMath::Abs(Wild.LeftLeg) <= 32.0f);
	TestEqual(TEXT("death clamps"), Wild.Fall, -90.0f);

	return true;
}

#endif

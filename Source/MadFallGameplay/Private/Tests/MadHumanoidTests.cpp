// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadHumanoidRig.h"
#include "MadCharacterAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"

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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadCharacterAnimTest,
	"MadFall.AI.CharacterAnim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadCharacterAnimTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::CharacterAnim;

	auto Sum = [](const FMadCharacterBlend& Blend) { return Blend.Idle + Blend.Walk + Blend.Jog; };
	const FMadCharacterBlend Standing = ComputeBlend(0.0f);
	TestEqual(TEXT("standing still is all idle"), Standing.Idle, 1.0f);

	const FMadCharacterBlend Shamble = ComputeBlend(120.0f);
	TestEqual(TEXT("a zombie's walk is all walk"), Shamble.Walk, 1.0f);
	TestEqual(TEXT("at 120 cm/s the walk cycle plays at 0.4 so the feet keep pace"), Shamble.WalkRate, 0.4f, 0.001f);
	TestEqual(TEXT("a crawl does not play in slow motion"), ComputeBlend(40.0f).WalkRate, 0.35f, 0.001f);

	const FMadCharacterBlend Between = ComputeBlend(400.0f);
	TestTrue(TEXT("between a walk and a jog both play"), Between.Walk > 0.0f && Between.Jog > 0.0f);
	TestEqual(TEXT("a horde run is all jog"), ComputeBlend(600.0f).Jog, 1.0f);
	for (float Speed : { 0.0f, 30.0f, 120.0f, 350.0f, 900.0f })
	{
		TestEqual(*FString::Printf(TEXT("weights sum to one at %.0f cm/s"), Speed), Sum(ComputeBlend(Speed)), 1.0f, 0.001f);
	}

	const FVector Rest = ArmReachDirection(1.0f);
	TestTrue(TEXT("arms reach forward and a little down"), Rest.Y > 0.9 && Rest.Z < 0.0);
	TestTrue(TEXT("mid-swing they are raised"), ArmReachDirection(0.5f).Z > 0.5);
	TestTrue(TEXT("the swing starts and ends at rest"), ArmReachDirection(0.0f).Equals(Rest, 1.0e-4));
	TestEqual(TEXT("the reach is a direction"), ArmReachDirection(0.3f).Size(), 1.0, 1.0e-4);

	// Installed by Scripts/copy_mannequin.ps1, never committed: a fresh clone has
	// no mannequin and draws the box figures, which the test above covers.
	// An anim instance may only live inside a skeletal mesh component.
	UMadCharacterAnimInstance* Instance = NewObject<UMadCharacterAnimInstance>(NewObject<USkeletalMeshComponent>());
	if (UMadCharacterAnimInstance::LoadSequences(*Instance))
	{
		TestTrue(TEXT("the installed walk cycle is the length its speed constant was measured from"), FMath::IsNearlyEqual(Instance->Walk->GetPlayLength(), 1.5f, 0.05f));
		TestNotNull(TEXT("the hit reaction is installed"), Instance->HitReact.Get());
		TestNotNull(TEXT("the death fall is installed"), Instance->Death.Get());
	}
	else
	{
		AddInfo(TEXT("The UE5 mannequin is not installed (Scripts/copy_mannequin.ps1); only the pure parts were tested."));
	}
	return true;
}

#endif

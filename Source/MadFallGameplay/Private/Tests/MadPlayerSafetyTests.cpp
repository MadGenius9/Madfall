// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadPlayerCharacter.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadEntombedRescueTest,
	"MadFall.Survival.EntombedRescue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadEntombedRescueTest::RunTest(const FString& Parameters)
{
	using MadFall::Player::FindHeadroomAbove;

	// Solid up to and including z 13, open above: the survivor found at z -42
	// (where the playtest's save had them) stands on 13 at 14.
	auto Ground = [](int32 Z) { return Z <= 13; };
	TestEqual(TEXT("lifted to the first open space on the ground"), FindHeadroomAbove(-42, 2, 256, Ground), 14);

	// A one-voxel air pocket inside the ground is not somewhere to stand: the
	// survivor needs two voxels of headroom.
	auto Pocket = [](int32 Z) { return Z != 5 && Z <= 13; };
	TestEqual(TEXT("a one-voxel pocket is skipped"), FindHeadroomAbove(0, 2, 256, Pocket), 14);

	// A cave two voxels tall with a floor is a fine place to be put.
	auto Cave = [](int32 Z) { return !(Z == 5 || Z == 6) && Z <= 13; };
	TestEqual(TEXT("a cave with headroom and a floor is used"), FindHeadroomAbove(0, 2, 256, Cave), 5);

	// Never a drop: an open gap with nothing under it is not a rescue.
	auto Overhang = [](int32 Z) { return Z >= 10 && Z <= 13; };
	TestEqual(TEXT("a gap with nothing under it is not used"), FindHeadroomAbove(-5, 2, 256, Overhang), 14);

	// Too deep to reach within MaxRise: nothing, rather than a guess.
	TestEqual(TEXT("out of reach gives none"), FindHeadroomAbove(-300, 2, 64, Ground), static_cast<int32>(INDEX_NONE));
	return true;
}

#endif

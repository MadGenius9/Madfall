// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadFrameBudget.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadFrameBudgetTest,
	"MadFall.Perf.FrameBudget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadFrameBudgetTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::FrameBudget;

	// The accounting is global; a simple automation test runs inside one engine
	// frame, so no real end-of-frame lands between these calls.
	Reset();

	// Two systems each inside the budget, together over it: the case per-system
	// stats cannot see, and the reason this exists.
	Add(EMadFrameSystem::Meshing, 0.0012);
	Add(EMadFrameSystem::Streaming, 0.0011);
	EndFrame();

	// Several scopes of one system in a frame add up.
	Add(EMadFrameSystem::Zombies, 0.0004);
	Add(EMadFrameSystem::Zombies, 0.0004);
	Add(EMadFrameSystem::Player, 0.0003);
	EndFrame();

	// A frame with no MadFall work counts as a frame, not a working frame.
	EndFrame();

	// Clearly over, and dominated by one system.
	Add(EMadFrameSystem::Structural, 0.0050);
	Add(EMadFrameSystem::Debris, 0.0002);
	EndFrame();

	const FReport R = GetReport();
	TestEqual(TEXT("frames"), R.Frames, int64(4));
	TestEqual(TEXT("working frames"), R.FramesWithWork, int64(3));
	TestEqual(TEXT("two frames over 2 ms"), R.FramesOverBudget, int64(2));
	TestEqual(TEXT("worst frame is the structural one"), R.WorstFrameMs, 5.2, 1e-6);
	TestEqual(TEXT("zombie scopes summed within their frame"), R.WorstMs[static_cast<int32>(EMadFrameSystem::Zombies)], 0.8, 1e-6);
	TestEqual(TEXT("meshing named as the larger cost of the combined frame"), R.CulpritCount[static_cast<int32>(EMadFrameSystem::Meshing)], int64(1));
	TestEqual(TEXT("structural named for its frame"), R.CulpritCount[static_cast<int32>(EMadFrameSystem::Structural)], int64(1));
	TestEqual(TEXT("streaming never the largest"), R.CulpritCount[static_cast<int32>(EMadFrameSystem::Streaming)], int64(0));
	TestTrue(TEXT("report names systems"), Describe().Contains(TEXT("structural")));

	Reset();
	TestEqual(TEXT("reset clears"), GetReport().Frames, int64(0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

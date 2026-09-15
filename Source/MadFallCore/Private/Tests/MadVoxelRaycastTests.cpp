// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadVoxelRaycast.h"
#include "Math/RandomStream.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadVoxelRaycastTest,
	"MadFall.Core.VoxelRaycast",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadVoxelRaycastTest::RunTest(const FString& Parameters)
{
	FMadVoxelHit Hit;

	// Ground: everything below z = 0.
	auto Ground = [](const FIntVector& V) { return V.Z < 0; };

	TestTrue(TEXT("straight down hits"), MadFall::VoxelRaycast(FVector(3.5, 4.5, 2.5), FVector(0, 0, -1), 10.0f, Ground, Hit));
	TestEqual(TEXT("the voxel under the origin"), Hit.Voxel, FIntVector(3, 4, -1));
	TestEqual(TEXT("entered through its top face"), Hit.Normal, FIntVector(0, 0, 1));
	TestEqual(TEXT("distance to the surface"), Hit.Distance, 2.5f, 1.0e-4f);

	TestFalse(TEXT("out of range misses"), MadFall::VoxelRaycast(FVector(3.5, 4.5, 20.5), FVector(0, 0, -1), 10.0f, Ground, Hit));
	TestFalse(TEXT("upward misses"), MadFall::VoxelRaycast(FVector(0.5, 0.5, 0.5), FVector(0, 0, 1), 50.0f, Ground, Hit));

	// Negative coordinates: floor, not truncation.
	auto One = [](const FIntVector& V) { return V == FIntVector(-3, -1, 0); };
	TestTrue(TEXT("hits a voxel at negative coordinates"), MadFall::VoxelRaycast(FVector(0.2, -0.5, 0.5), FVector(-1, 0, 0), 10.0f, One, Hit));
	TestEqual(TEXT("correct negative voxel"), Hit.Voxel, FIntVector(-3, -1, 0));
	TestEqual(TEXT("entered from +X"), Hit.Normal, FIntVector(1, 0, 0));
	TestEqual(TEXT("distance to x = -2"), Hit.Distance, 2.2f, 1.0e-4f);

	// Starting inside a solid voxel reports it with no normal.
	TestTrue(TEXT("inside solid"), MadFall::VoxelRaycast(FVector(0.5, 0.5, -0.5), FVector(1, 0, 0), 5.0f, Ground, Hit));
	TestEqual(TEXT("zero normal from inside"), Hit.Normal, FIntVector::ZeroValue);

	// A diagonal ray never skips a voxel: compare against dense sampling.
	FRandomStream Random(99);
	int32 Mismatches = 0;
	for (int32 Trial = 0; Trial < 500; ++Trial)
	{
		const FIntVector Target(Random.RandRange(-6, 6), Random.RandRange(-6, 6), Random.RandRange(-6, 6));
		auto Single = [&Target](const FIntVector& V) { return V == Target; };

		const FVector Origin(Random.FRandRange(-0.9, 0.9), Random.FRandRange(-0.9, 0.9), Random.FRandRange(-0.9, 0.9));
		const FVector Aim = (FVector(Target) + FVector(Random.FRandRange(0.05, 0.95), Random.FRandRange(0.05, 0.95), Random.FRandRange(0.05, 0.95))) - Origin;
		if (Target == FIntVector(FMath::FloorToInt32(Origin.X), FMath::FloorToInt32(Origin.Y), FMath::FloorToInt32(Origin.Z)))
		{
			continue;
		}

		const bool bHit = MadFall::VoxelRaycast(Origin, Aim, 30.0f, Single, Hit);
		if (!bHit || Hit.Voxel != Target)
		{
			++Mismatches;
			continue;
		}

		// The reported distance must put the hit point on the reported face.
		const FVector Point = Origin + Aim.GetSafeNormal() * Hit.Distance;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Hit.Normal[Axis] != 0)
			{
				const double Face = Hit.Normal[Axis] > 0 ? Target[Axis] + 1.0 : static_cast<double>(Target[Axis]);
				Mismatches += FMath::Abs(Point[Axis] - Face) > 1.0e-3 ? 1 : 0;
			}
		}
	}
	TestEqual(TEXT("aimed rays always reach their target, landing on the reported face"), Mismatches, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

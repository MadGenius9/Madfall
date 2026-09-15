// Copyright MadFall. All Rights Reserved.

#include "MadVoxelRaycast.h"

namespace MadFall
{
	bool VoxelRaycast(const FVector& Origin, const FVector& Direction, float MaxDistance,
		TFunctionRef<bool(const FIntVector&)> IsSolid, FMadVoxelHit& OutHit)
	{
		const FVector Dir = Direction.GetSafeNormal();
		if (Dir.IsNearlyZero() || MaxDistance <= 0.0f)
		{
			return false;
		}

		FIntVector Voxel(FMath::FloorToInt32(Origin.X), FMath::FloorToInt32(Origin.Y), FMath::FloorToInt32(Origin.Z));

		if (IsSolid(Voxel))
		{
			OutHit.Voxel = Voxel;
			OutHit.Normal = FIntVector::ZeroValue;
			OutHit.Distance = 0.0f;
			return true;
		}

		FIntVector Step;
		FVector TMax;
		FVector TDelta;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double D = Dir[Axis];
			if (D > 0.0)
			{
				Step[Axis] = 1;
				TDelta[Axis] = 1.0 / D;
				TMax[Axis] = (static_cast<double>(Voxel[Axis]) + 1.0 - Origin[Axis]) / D;
			}
			else if (D < 0.0)
			{
				Step[Axis] = -1;
				TDelta[Axis] = -1.0 / D;
				TMax[Axis] = (static_cast<double>(Voxel[Axis]) - Origin[Axis]) / D;
			}
			else
			{
				Step[Axis] = 0;
				TDelta[Axis] = TNumericLimits<double>::Max();
				TMax[Axis] = TNumericLimits<double>::Max();
			}
		}

		// Bounded by the number of voxel faces a ray of MaxDistance can cross.
		const int32 MaxSteps = FMath::CeilToInt32(MaxDistance * 3.0f) + 3;
		for (int32 Iteration = 0; Iteration < MaxSteps; ++Iteration)
		{
			int32 Axis = 0;
			if (TMax.Y < TMax[Axis]) { Axis = 1; }
			if (TMax.Z < TMax[Axis]) { Axis = 2; }

			const double Distance = TMax[Axis];
			if (Distance > MaxDistance)
			{
				return false;
			}

			Voxel[Axis] += Step[Axis];
			TMax[Axis] += TDelta[Axis];

			if (IsSolid(Voxel))
			{
				OutHit.Voxel = Voxel;
				OutHit.Normal = FIntVector::ZeroValue;
				OutHit.Normal[Axis] = -Step[Axis];
				OutHit.Distance = static_cast<float>(Distance);
				return true;
			}
		}

		return false;
	}
}

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

/** Result of a voxel raycast. */
struct MADFALLCORE_API FMadVoxelHit
{
	/** The solid voxel that was hit. */
	FIntVector Voxel = FIntVector::ZeroValue;

	/**
	 * Unit axis vector of the face that was entered, pointing back toward the
	 * ray origin. Voxel + Normal is where a placed block goes. Zero when the
	 * ray started inside a solid voxel.
	 */
	FIntVector Normal = FIntVector::ZeroValue;

	/** Distance along the ray, in voxels. */
	float Distance = 0.0f;
};

namespace MadFall
{
	/**
	 * Walks the voxel grid along a ray (Amanatides & Woo) and returns the first
	 * voxel IsSolid accepts, within MaxDistance voxels.
	 *
	 * Why not a physics line trace: the physics scene only has collision for
	 * chunks whose mesh has been cooked, which lags an edit by a frame or more,
	 * and a mesh trace hits a smoothed isosurface that does not say which voxel
	 * it belongs to. Mining must target exactly the voxel under the crosshair,
	 * as it is in the voxel data right now.
	 *
	 * Origin is in voxel units (world centimetres / VoxelSizeUU); voxel v spans
	 * [v, v + 1) on each axis.
	 */
	MADFALLCORE_API bool VoxelRaycast(const FVector& Origin, const FVector& Direction, float MaxDistance,
		TFunctionRef<bool(const FIntVector&)> IsSolid, FMadVoxelHit& OutHit);
}

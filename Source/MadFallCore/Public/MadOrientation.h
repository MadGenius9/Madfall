// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The 24 proper rotations of a cube, as the orientation index stored in
 * FMadVoxel::Rotation bits 0-4.
 *
 * Index layout: UpFace * 4 + Spin
 *   UpFace  0 +Z, 1 -Z, 2 +X, 3 -X, 4 +Y, 5 -Y   (where the block's local +Z points)
 *   Spin    0-3 quarter turns about the block's own up axis
 *
 * WHY THIS IS MATRICES AND NOT A HAND-WRITTEN TABLE:
 * Stamping a prefab rotated 90 degrees has to rotate every block's orientation
 * as well as its position, or a staircase built facing north ends up facing
 * north inside a building that now faces east. That composition is a 24x4
 * table, and a hand-typed version of it has 96 entries in which a single wrong
 * digit silently mis-rotates one block type in one direction. Deriving the
 * table from rotation matrices at startup, and testing that the 24 are
 * distinct and that four yaw steps are the identity, turns "probably right"
 * into "provably right".
 */
namespace MadFall::Orientation
{
	inline constexpr int32 Count = 24;

	/** A 3x3 integer rotation matrix. Row-major: M[row][col]. */
	struct MADFALLCORE_API FIntMatrix3
	{
		int32 M[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

		FIntMatrix3 operator*(const FIntMatrix3& Other) const;
		bool operator==(const FIntMatrix3& Other) const;

		FIntVector Transform(const FIntVector& Vector) const;
	};

	/** The rotation matrix for an orientation index. */
	MADFALLCORE_API const FIntMatrix3& GetMatrix(uint8 Orientation);

	/**
	 * The orientation a block ends up with after the whole prefab it belongs to
	 * is rotated by YawQuarterTurns * 90 degrees about world +Z.
	 */
	MADFALLCORE_API uint8 ApplyYaw(uint8 Orientation, int32 YawQuarterTurns);

	/** Orientation index for a matrix, or INDEX_NONE if it is not one of the 24. */
	MADFALLCORE_API int32 FindIndex(const FIntMatrix3& Matrix);

	/** A quarter-turn yaw about +Z. 0-3, negative and >3 values wrap. */
	MADFALLCORE_API FIntMatrix3 MakeYaw(int32 YawQuarterTurns);

	/**
	 * Rotates a position inside a SizeX x SizeY footprint by quarter turns,
	 * keeping it inside the rotated footprint's [0, size) box.
	 *
	 * Distinct from GetMatrix: a matrix rotates about the origin, which would
	 * move a prefab off its own bounding box; this rotates about the footprint
	 * so the result is always a valid index into the rotated prefab.
	 */
	FORCEINLINE void RotateFootprint(int32 X, int32 Y, int32 SizeX, int32 SizeY, int32 YawQuarterTurns,
		int32& OutX, int32& OutY)
	{
		switch (((YawQuarterTurns % 4) + 4) % 4)
		{
		case 0:  OutX = X;              OutY = Y;              break;
		case 1:  OutX = SizeY - 1 - Y;  OutY = X;              break;
		case 2:  OutX = SizeX - 1 - X;  OutY = SizeY - 1 - Y;  break;
		default: OutX = Y;              OutY = SizeX - 1 - X;  break;
		}
	}

	/** Inverse of RotateFootprint. Input is in the ROTATED footprint; output in the original. */
	FORCEINLINE void UnrotateFootprint(int32 RotatedX, int32 RotatedY, int32 SizeX, int32 SizeY,
		int32 YawQuarterTurns, int32& OutX, int32& OutY)
	{
		switch (((YawQuarterTurns % 4) + 4) % 4)
		{
		case 0:  OutX = RotatedX;              OutY = RotatedY;              break;
		case 1:  OutX = RotatedY;              OutY = SizeY - 1 - RotatedX;  break;
		case 2:  OutX = SizeX - 1 - RotatedX;  OutY = SizeY - 1 - RotatedY;  break;
		default: OutX = SizeX - 1 - RotatedY;  OutY = RotatedX;              break;
		}
	}

	/** The footprint size after rotation: X and Y swap on odd quarter turns. */
	FORCEINLINE FIntVector RotateSize(const FIntVector& Size, int32 YawQuarterTurns)
	{
		return (((YawQuarterTurns % 4) + 4) % 2 == 1)
			? FIntVector(Size.Y, Size.X, Size.Z)
			: Size;
	}
}

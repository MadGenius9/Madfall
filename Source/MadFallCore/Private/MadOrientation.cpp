// Copyright MadFall. All Rights Reserved.

#include "MadOrientation.h"

namespace MadFall::Orientation
{
	FIntMatrix3 FIntMatrix3::operator*(const FIntMatrix3& Other) const
	{
		FIntMatrix3 Result;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				int32 Sum = 0;
				for (int32 K = 0; K < 3; ++K)
				{
					Sum += M[Row][K] * Other.M[K][Col];
				}
				Result.M[Row][Col] = Sum;
			}
		}
		return Result;
	}

	bool FIntMatrix3::operator==(const FIntMatrix3& Other) const
	{
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				if (M[Row][Col] != Other.M[Row][Col])
				{
					return false;
				}
			}
		}
		return true;
	}

	FIntVector FIntMatrix3::Transform(const FIntVector& V) const
	{
		return FIntVector(
			M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z,
			M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
			M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z);
	}

	namespace
	{
		FIntMatrix3 Make(int32 A, int32 B, int32 C, int32 D, int32 E, int32 F, int32 G, int32 H, int32 I)
		{
			FIntMatrix3 Result;
			Result.M[0][0] = A; Result.M[0][1] = B; Result.M[0][2] = C;
			Result.M[1][0] = D; Result.M[1][1] = E; Result.M[1][2] = F;
			Result.M[2][0] = G; Result.M[2][1] = H; Result.M[2][2] = I;
			return Result;
		}

		struct FTables
		{
			FIntMatrix3 Matrices[Count];
			uint8 YawTable[Count][4];

			FTables()
			{
				// Base rotations taking local +Z to each up face. These six are the
				// only hand-written data in the system; everything else is derived.
				const FIntMatrix3 UpFaces[6] =
				{
					Make( 1, 0, 0,   0, 1, 0,   0, 0, 1),   // +Z  identity
					Make( 1, 0, 0,   0,-1, 0,   0, 0,-1),   // -Z  180 about X
					Make( 0, 0, 1,   0, 1, 0,  -1, 0, 0),   // +X  +90 about Y
					Make( 0, 0,-1,   0, 1, 0,   1, 0, 0),   // -X  -90 about Y
					Make( 1, 0, 0,   0, 0, 1,   0,-1, 0),   // +Y  -90 about X: (0,0,1) -> (0,1,0)
					Make( 1, 0, 0,   0, 0,-1,   0, 1, 0)    // -Y  +90 about X: (0,0,1) -> (0,-1,0)
				};

				for (int32 Up = 0; Up < 6; ++Up)
				{
					for (int32 Spin = 0; Spin < 4; ++Spin)
					{
						// Spin is applied in the block's LOCAL frame, before the
						// up-face rotation, so that spin always means "turn about
						// the block's own up axis" whichever way up it is.
						Matrices[Up * 4 + Spin] = UpFaces[Up] * MakeYaw(Spin);
					}
				}

				for (int32 Index = 0; Index < Count; ++Index)
				{
					for (int32 Yaw = 0; Yaw < 4; ++Yaw)
					{
						// World yaw is applied AFTER the block's own rotation.
						const FIntMatrix3 Composed = MakeYaw(Yaw) * Matrices[Index];

						int32 Found = INDEX_NONE;
						for (int32 Candidate = 0; Candidate < Count; ++Candidate)
						{
							if (Matrices[Candidate] == Composed)
							{
								Found = Candidate;
								break;
							}
						}

						// The 24 rotations form a group, so composition can never
						// leave it. If this fires, one of the six base matrices
						// above is not a proper rotation.
						checkf(Found != INDEX_NONE, TEXT("Orientation %d yawed %d left the rotation group"), Index, Yaw);
						YawTable[Index][Yaw] = static_cast<uint8>(Found);
					}
				}
			}
		};

		const FTables& GetTables()
		{
			// Function-local static: thread-safe initialisation under C++11, and
			// built on first use rather than during static init order roulette.
			static const FTables Tables;
			return Tables;
		}
	}

	FIntMatrix3 MakeYaw(int32 YawQuarterTurns)
	{
		switch (((YawQuarterTurns % 4) + 4) % 4)
		{
		case 0:  return Make( 1, 0, 0,   0, 1, 0,   0, 0, 1);
		case 1:  return Make( 0,-1, 0,   1, 0, 0,   0, 0, 1);   // +X -> +Y
		case 2:  return Make(-1, 0, 0,   0,-1, 0,   0, 0, 1);
		default: return Make( 0, 1, 0,  -1, 0, 0,   0, 0, 1);
		}
	}

	const FIntMatrix3& GetMatrix(uint8 Orientation)
	{
		return GetTables().Matrices[FMath::Min<int32>(Orientation, Count - 1)];
	}

	uint8 ApplyYaw(uint8 Orientation, int32 YawQuarterTurns)
	{
		const int32 Yaw = ((YawQuarterTurns % 4) + 4) % 4;
		return GetTables().YawTable[FMath::Min<int32>(Orientation, Count - 1)][Yaw];
	}

	int32 FindIndex(const FIntMatrix3& Matrix)
	{
		const FTables& Tables = GetTables();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Tables.Matrices[Index] == Matrix)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}
}

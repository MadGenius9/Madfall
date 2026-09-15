// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBlockRegistry.h"
#include "MadChunkMesher.h"
#include "MadChunkSampleGrid.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadLodTests
{
	/** Rolling terrain in world voxels: the surface height, and a density ramp two voxels deep across it. */
	float SurfaceHeight(float WorldX, float WorldY)
	{
		return 16.0f + FMath::Sin(WorldX * 0.13f) * 7.0f + FMath::Cos(WorldY * 0.09f) * 5.0f;
	}

	void SampleTerrain(int32 WorldX, int32 WorldY, int32 WorldZ, uint16 Rock, uint16& OutBlock, uint8& OutDensity, uint8& OutFlags)
	{
		const float Normalized = FMath::Clamp((SurfaceHeight(static_cast<float>(WorldX), static_cast<float>(WorldY)) - static_cast<float>(WorldZ)) * 0.5f + 0.5f, 0.0f, 1.0f);
		OutDensity = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
		OutBlock = OutDensity >= 128 ? Rock : MadFall::BlockTypeAir;
		OutFlags = 0;
	}

	using FSampler = TFunctionRef<void(int32 WorldX, int32 WorldY, int32 WorldZ, uint16& OutBlock, uint8& OutDensity, uint8& OutFlags)>;

	/** Meshes chunk (ChunkX, 0, 0) of a world at a detail level. */
	FMadChunkMesh MeshChunk(int32 ChunkX, int32 Lod, FSampler Sample, const FMadBlockRegistry& Registry)
	{
		const int32 OffsetX = ChunkX * MadFall::ChunkSize;
		FMadChunkSampleGrid Grid;
		Grid.Coord = FMadChunkCoord(ChunkX, 0, 0);
		for (int32 Z = -1; Z <= MadFall::ChunkSize; ++Z)
		{
			for (int32 Y = -1; Y <= MadFall::ChunkSize; ++Y)
			{
				for (int32 X = -1; X <= MadFall::ChunkSize; ++X)
				{
					const int32 Index = FMadChunkSampleGrid::Index(X, Y, Z);
					Sample(X + OffsetX, Y, Z, Grid.BlockId[Index], Grid.Density[Index], Grid.Flags[Index]);
				}
			}
		}

		FMadLodSampleGrid LodGrid;
		LodGrid.Coord = Grid.Coord;
		LodGrid.Init(1 << Lod);
		LodGrid.Fill([OffsetX, &Sample](int32 X, int32 Y, int32 Z, uint16& OutBlock, uint8& OutDensity, uint8& OutFlags)
		{
			Sample(X + OffsetX, Y, Z, OutBlock, OutDensity, OutFlags);
		});

		MadFall::ChunkMesher::FMeshSettings Settings;
		Settings.LodLevel = Lod;
		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, &LodGrid, Registry, Settings, Mesh);
		return Mesh;
	}

	/**
	 * The worst distance, in voxels, between a mesh's vertices and the terrain
	 * surface they approximate, inside the chunk: a coarse chunk's outer rows are
	 * deliberately pulled past its boundary (see the mesher), so they are not
	 * where the surface is.
	 */
	float WorstHeightError(const FMadChunkMesh& Mesh, int32 ChunkX)
	{
		float Worst = 0.0f;
		for (const FMadMeshSection& Section : Mesh.Sections)
		{
			for (const FVector3f& Position : Section.Positions)
			{
				// A voxel's sample sits at its centre, so the surface through samples
				// at height H is drawn at H + 0.5 (see the mesher's vertex placement).
				const float X = Position.X / MadFall::VoxelSizeUU + ChunkX * MadFall::ChunkSize;
				const float Y = Position.Y / MadFall::VoxelSizeUU;
				const float LocalX = Position.X / MadFall::VoxelSizeUU;
				if (LocalX < 1.0f || LocalX > MadFall::ChunkSize - 1.0f || Y < 1.0f || Y > MadFall::ChunkSize - 1.0f)
				{
					continue;
				}
				const float Expected = SurfaceHeight(X - 0.5f, Y - 0.5f) + 0.5f;
				Worst = FMath::Max(Worst, FMath::Abs(Position.Z / MadFall::VoxelSizeUU - Expected));
			}
		}
		return Worst;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadChunkLodTest,
	"MadFall.Mesher.Lod",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadChunkLodTest::RunTest(const FString& Parameters)
{
	using namespace MadLodTests;
	using MadFall::ChunkMesher::ChooseLod;

	// --- choosing a level ---------------------------------------------------------
	TestEqual(TEXT("near chunks are full detail"), ChooseLod(5, -1, 5, 11), 0);
	TestEqual(TEXT("past the first distance, half"), ChooseLod(6, -1, 5, 11), 1);
	TestEqual(TEXT("past the second, quarter"), ChooseLod(12, -1, 5, 11), 2);
	TestEqual(TEXT("walking away, a full-detail chunk keeps its level one chunk past the boundary"), ChooseLod(6, 0, 5, 11), 0);
	TestEqual(TEXT("and coarsens two past it"), ChooseLod(7, 0, 5, 11), 1);
	TestEqual(TEXT("walking back, detail returns at the boundary itself"), ChooseLod(5, 1, 5, 11), 0);
	TestEqual(TEXT("a half chunk inside the band stays half"), ChooseLod(6, 1, 5, 11), 1);
	TestEqual(TEXT("the band applies at the second boundary too"), ChooseLod(12, 1, 5, 11), 1);

	// --- meshing a coarse lattice ---------------------------------------------------
	FMadBlockRegistry Registry;
	{
		TArray<FMadDefinitionError> Errors;
		Registry.BeginLoad();
		FMadBlockDefinitionData Rock;
		Rock.Id = FName(TEXT("test:lod_rock"));
		Rock.MaterialClass = FName(TEXT("test:rock"));
		Rock.SourceModId = FName(TEXT("test"));
		Registry.AddFromAsset(Rock, Errors);
		Registry.FinishLoad(Errors);
	}
	const uint16 Rock = Registry.ResolveRuntimeId(FName(TEXT("test:lod_rock")));
	auto Terrain = [Rock](int32 X, int32 Y, int32 Z, uint16& OutBlock, uint8& OutDensity, uint8& OutFlags)
	{
		SampleTerrain(X, Y, Z, Rock, OutBlock, OutDensity, OutFlags);
	};

	const FMadChunkMesh Full = MeshChunk(1, 0, Terrain, Registry);
	const FMadChunkMesh Half = MeshChunk(1, 1, Terrain, Registry);
	const FMadChunkMesh Quarter = MeshChunk(1, 2, Terrain, Registry);

	TestEqual(TEXT("a half-detail mesh says so"), Half.LodLevel, 1);
	// The stride is horizontal, so a level keeps the terrain's vertical steps: on
	// this deliberately steep ground (slopes near 45 degrees) a level is 36-40%
	// of the one before; on rolling ground it approaches a quarter.
	TestTrue(FString::Printf(TEXT("half detail is at most 40%% of the triangles (%d of %d)"), Half.TotalTriangles(), Full.TotalTriangles()),
		Half.TotalTriangles() > 0 && Half.TotalTriangles() * 10 <= Full.TotalTriangles() * 4);
	TestTrue(FString::Printf(TEXT("quarter detail is at most half of half (%d of %d)"), Quarter.TotalTriangles(), Half.TotalTriangles()),
		Quarter.TotalTriangles() > 0 && Quarter.TotalTriangles() * 2 <= Half.TotalTriangles());

	const float FullError = WorstHeightError(Full, 1);
	const float HalfError = WorstHeightError(Half, 1);
	const float QuarterError = WorstHeightError(Quarter, 1);
	TestTrue(FString::Printf(TEXT("full detail follows the terrain within half a voxel (%.2f)"), FullError), FullError <= 0.5f);
	TestTrue(FString::Printf(TEXT("half detail within a voxel (%.2f)"), HalfError), HalfError <= 1.0f);
	TestTrue(FString::Printf(TEXT("quarter detail within two (%.2f)"), QuarterError), QuarterError <= 2.0f);

	// A flat surface with a hard density step - water, a levelled floor - lies at
	// exactly the same height at every level. A vertical stride put it half a
	// voxel off in the distance, a visible step at every level boundary.
	{
		auto Sea = [Rock](int32 X, int32 Y, int32 Z, uint16& OutBlock, uint8& OutDensity, uint8& OutFlags)
		{
			OutDensity = Z <= 19 ? 255 : 0;
			OutBlock = Z <= 19 ? Rock : MadFall::BlockTypeAir;
			OutFlags = 0;
		};
		for (int32 Lod = 0; Lod <= 2; ++Lod)
		{
			const FMadChunkMesh Mesh = MeshChunk(1, Lod, Sea, Registry);
			float Lowest = TNumericLimits<float>::Max();
			float Highest = TNumericLimits<float>::Lowest();
			for (const FMadMeshSection& Section : Mesh.Sections)
			{
				for (const FVector3f& Position : Section.Positions)
				{
					Lowest = FMath::Min(Lowest, Position.Z);
					Highest = FMath::Max(Highest, Position.Z);
				}
			}
			TestTrue(FString::Printf(TEXT("a flat surface at level %d lies at 20 voxels (%.1f to %.1f uu)"), Lod, Lowest, Highest),
				Mesh.TotalTriangles() > 0 && FMath::IsNearlyEqual(Lowest, 2000.0f, 1.0f) && FMath::IsNearlyEqual(Highest, 2000.0f, 1.0f));
		}
	}

	// A coarse lattice without its grid, or at the wrong stride, meshes at full detail.
	{
		FMadChunkSampleGrid Grid;
		FMadLodSampleGrid WrongStride;
		WrongStride.Init(4);
		MadFall::ChunkMesher::FMeshSettings Settings;
		Settings.LodLevel = 1;
		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, &WrongStride, Registry, Settings, Mesh);
		TestEqual(TEXT("a mismatched lattice falls back to full detail"), Mesh.LodLevel, 0);
	}

	// --- no cracks where levels meet ---------------------------------------------------
	// Rows of four chunks along X, finer to the west and finer to the east.
	// Terrain here is a heightfield, so a crack is a column of the ground plane no
	// triangle covers from above.
	static constexpr int32 Rows[2][4] = { { 0, 1, 1, 2 }, { 2, 1, 0, 0 } };
	for (const int32* Levels : { Rows[0], Rows[1] })
	{
		TArray<FMadChunkMesh> Row;
		for (int32 ChunkX = 0; ChunkX < 4; ++ChunkX)
		{
			Row.Add(MeshChunk(ChunkX, Levels[ChunkX], Terrain, Registry));
		}

		// Covered sample points on a quarter-voxel lattice. Y stays clear of the
		// row's outer edges, where no neighbouring chunk was meshed.
		constexpr int32 PerVoxel = 4;
		const int32 SamplesX = 4 * MadFall::ChunkSize * PerVoxel;
		const int32 MinY = 2 * PerVoxel;
		const int32 MaxY = (MadFall::ChunkSize - 2) * PerVoxel;
		const int32 SamplesY = MaxY - MinY + 1;
		TBitArray<> Covered(false, SamplesX * SamplesY);

		for (int32 ChunkX = 0; ChunkX < Row.Num(); ++ChunkX)
		{
			const float OffsetX = ChunkX * MadFall::ChunkSize;
			for (const FMadMeshSection& Section : Row[ChunkX].Sections)
			{
				for (int32 Index = 0; Index + 2 < Section.Indices.Num(); Index += 3)
				{
					FVector2f P[3];
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						const FVector3f& Position = Section.Positions[Section.Indices[Index + Corner]];
						P[Corner] = FVector2f(Position.X / MadFall::VoxelSizeUU + OffsetX, Position.Y / MadFall::VoxelSizeUU);
					}
					const float Area = (P[1] - P[0]) ^ (P[2] - P[0]);
					if (FMath::Abs(Area) < 1.0e-6f)
					{
						continue;
					}
					const int32 X0 = FMath::Max(0, FMath::FloorToInt32(FMath::Min3(P[0].X, P[1].X, P[2].X) * PerVoxel));
					const int32 X1 = FMath::Min(SamplesX - 1, FMath::CeilToInt32(FMath::Max3(P[0].X, P[1].X, P[2].X) * PerVoxel));
					const int32 Y0 = FMath::Max(MinY, FMath::FloorToInt32(FMath::Min3(P[0].Y, P[1].Y, P[2].Y) * PerVoxel));
					const int32 Y1 = FMath::Min(MaxY, FMath::CeilToInt32(FMath::Max3(P[0].Y, P[1].Y, P[2].Y) * PerVoxel));
					for (int32 SY = Y0; SY <= Y1; ++SY)
					{
						for (int32 SX = X0; SX <= X1; ++SX)
						{
							const FVector2f Q(static_cast<float>(SX) / PerVoxel, static_cast<float>(SY) / PerVoxel);
							// Barycentric signs with a hair of slack, so a point exactly on
							// a shared edge counts for both triangles.
							const float W0 = ((P[1] - Q) ^ (P[2] - Q)) / Area;
							const float W1 = ((P[2] - Q) ^ (P[0] - Q)) / Area;
							const float W2 = 1.0f - W0 - W1;
							if (W0 >= -1.0e-3f && W1 >= -1.0e-3f && W2 >= -1.0e-3f)
							{
								Covered[SX + (SY - MinY) * SamplesX] = true;
							}
						}
					}
				}
			}
		}

		// The first voxel of the row has no chunk to its west, so start past it.
		int32 Holes = 0;
		FVector2f FirstHole(-1.0f);
		for (int32 SY = MinY; SY <= MaxY; ++SY)
		{
			for (int32 SX = 2 * PerVoxel; SX < SamplesX - 2 * PerVoxel; ++SX)
			{
				if (!Covered[SX + (SY - MinY) * SamplesX])
				{
					if (Holes++ == 0)
					{
						FirstHole = FVector2f(static_cast<float>(SX) / PerVoxel, static_cast<float>(SY) / PerVoxel);
					}
				}
			}
		}
		TestEqual(FString::Printf(TEXT("the ground is covered across chunks at levels %d %d %d %d (first hole at voxel %.2f, %.2f)"),
			Levels[0], Levels[1], Levels[2], Levels[3], FirstHole.X, FirstHole.Y), Holes, 0);
	}

	return true;
}

#endif

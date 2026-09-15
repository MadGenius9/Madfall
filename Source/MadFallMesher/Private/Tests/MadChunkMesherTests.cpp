// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBlockRegistry.h"
#include "MadChunkMesher.h"
#include "MadChunkSampleGrid.h"
#include "MadSurfaceRegistry.h"
#include "MadWorldGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadMesherTests
{
	struct FTestBlocks
	{
		uint16 Rock = 0;
		uint16 Timber = 0;
	};

	/**
	 * Two blocks with DIFFERENT material classes, registered without touching
	 * disk.
	 *
	 * The material class matters: mesh sections are keyed by it, not by block
	 * id, so twenty concrete variants share one draw call. Two blocks with the
	 * same material class are supposed to end up in one section.
	 */
	FTestBlocks BuildRegistry(FMadBlockRegistry& Registry)
	{
		TArray<FMadDefinitionError> Errors;
		Registry.BeginLoad();

		FMadBlockDefinitionData Rock;
		Rock.Id = FName(TEXT("test:rock_block"));
		Rock.MaterialClass = FName(TEXT("test:rock"));
		Rock.SourceModId = FName(TEXT("test"));
		Registry.AddFromAsset(Rock, Errors);

		FMadBlockDefinitionData Timber;
		Timber.Id = FName(TEXT("test:timber_block"));
		Timber.MaterialClass = FName(TEXT("test:timber"));
		Timber.SourceModId = FName(TEXT("test"));
		Registry.AddFromAsset(Timber, Errors);

		Registry.FinishLoad(Errors);

		FTestBlocks Blocks;
		Blocks.Rock = Registry.ResolveRuntimeId(Rock.Id);
		Blocks.Timber = Registry.ResolveRuntimeId(Timber.Id);
		return Blocks;
	}

	/** Fills a grid from a density function over world-voxel coordinates. */
	template <typename FuncType>
	FMadChunkSampleGrid MakeGrid(uint16 SolidBlockId, FuncType&& DensityAt, bool bCubic = false)
	{
		FMadChunkSampleGrid Grid;

		for (int32 Z = -1; Z <= MadFall::ChunkSize; ++Z)
		{
			for (int32 Y = -1; Y <= MadFall::ChunkSize; ++Y)
			{
				for (int32 X = -1; X <= MadFall::ChunkSize; ++X)
				{
					const int32 Index = FMadChunkSampleGrid::Index(X, Y, Z);
					const uint8 Density = DensityAt(X, Y, Z);

					Grid.Density[Index] = Density;
					Grid.BlockId[Index] = (Density >= 128) ? SolidBlockId : MadFall::BlockTypeAir;
					Grid.Flags[Index] = (bCubic && Density >= 128)
						? static_cast<uint8>(EMadVoxelFlags::Cubic) : 0;
				}
			}
		}

		return Grid;
	}

	/**
	 * Unreal's own face-normal formula, lifted from
	 * KismetProceduralMeshLibrary::CalculateTangentsForMesh:
	 *
	 *     N = (P1 - P2) x (P0 - P2)
	 *
	 * Expanding it shows N is the NEGATION of the conventional right-handed
	 * CCW normal (P1-P0) x (P2-P0), because Unreal is left-handed. That is the
	 * whole reason the mesher flips its winding, and asserting against this
	 * formula is the only way to know the flip is right rather than assumed.
	 */
	FVector3f UnrealFaceNormal(const FVector3f& P0, const FVector3f& P1, const FVector3f& P2)
	{
		return FVector3f::CrossProduct(P1 - P2, P0 - P2).GetSafeNormal();
	}

	struct FMeshAudit
	{
		int32 Triangles = 0;
		int32 DegenerateTriangles = 0;
		int32 BackfacingTriangles = 0;
		int32 OutOfBoundsVertices = 0;
		int32 BadNormals = 0;
		float MinDot = 1.0f;
	};

	FMeshAudit AuditMesh(const FMadChunkMesh& Mesh)
	{
		FMeshAudit Audit;

		// Geometry may reach one voxel outside the chunk: the -1 cell row exists
		// so a chunk can close its own minimum faces using neighbour samples.
		const float Low = -2.0f * MadFall::VoxelSizeUU;
		const float High = (MadFall::ChunkSize + 2.0f) * MadFall::VoxelSizeUU;

		for (const FMadMeshSection& Section : Mesh.Sections)
		{
			for (const FVector3f& Position : Section.Positions)
			{
				if (Position.X < Low || Position.X > High
					|| Position.Y < Low || Position.Y > High
					|| Position.Z < Low || Position.Z > High)
				{
					++Audit.OutOfBoundsVertices;
				}
			}

			for (const FVector3f& Normal : Section.Normals)
			{
				if (!FMath::IsNearlyEqual(Normal.Size(), 1.0f, 0.01f))
				{
					++Audit.BadNormals;
				}
			}

			for (int32 Index = 0; Index + 2 < Section.Indices.Num(); Index += 3)
			{
				const int32 I0 = static_cast<int32>(Section.Indices[Index]);
				const int32 I1 = static_cast<int32>(Section.Indices[Index + 1]);
				const int32 I2 = static_cast<int32>(Section.Indices[Index + 2]);

				const FVector3f& P0 = Section.Positions[I0];
				const FVector3f& P1 = Section.Positions[I1];
				const FVector3f& P2 = Section.Positions[I2];

				++Audit.Triangles;

				const float Area = FVector3f::CrossProduct(P1 - P0, P2 - P0).Size() * 0.5f;
				if (Area < 1.0f)   // 1 square uu out of a 100 uu voxel
				{
					++Audit.DegenerateTriangles;
					continue;
				}

				const FVector3f FaceNormal = UnrealFaceNormal(P0, P1, P2);
				const FVector3f ShadingNormal =
					(Section.Normals[I0] + Section.Normals[I1] + Section.Normals[I2]).GetSafeNormal();

				const float Dot = FVector3f::DotProduct(FaceNormal, ShadingNormal);
				Audit.MinDot = FMath::Min(Audit.MinDot, Dot);

				if (Dot <= 0.0f)
				{
					++Audit.BackfacingTriangles;
				}
			}
		}

		return Audit;
	}
}

// ===========================================================================
// Smooth terrain: surface boundaries
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSmoothMaterialBoundaryTest,
	"MadFall.Mesher.SmoothMaterialBoundary",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSmoothMaterialBoundaryTest::RunTest(const FString& Parameters)
{
	// Two shipped surfaces with different patterns, meeting on a slope.
	const FName StoneClass(TEXT("madfall:stone"));
	const FName GrassClass(TEXT("madfall:grass"));
	const FColor StoneColour = MadFall::GetSurfaces().GetVertexColor(StoneClass);
	const FColor GrassColour = MadFall::GetSurfaces().GetVertexColor(GrassClass);
	if (!TestNotEqual(TEXT("the two surfaces use different patterns"), static_cast<int32>(StoneColour.A), static_cast<int32>(GrassColour.A)))
	{
		return false;
	}

	FMadBlockRegistry Registry;
	TArray<FMadDefinitionError> Errors;
	Registry.BeginLoad();
	FMadBlockDefinitionData Stone;
	Stone.Id = FName(TEXT("test:stone_block"));
	Stone.MaterialClass = StoneClass;
	Stone.SourceModId = FName(TEXT("test"));
	Registry.AddFromAsset(Stone, Errors);
	FMadBlockDefinitionData Grass;
	Grass.Id = FName(TEXT("test:grass_block"));
	Grass.MaterialClass = GrassClass;
	Grass.SourceModId = FName(TEXT("test"));
	Registry.AddFromAsset(Grass, Errors);
	Registry.FinishLoad(Errors);
	const uint16 StoneId = Registry.ResolveRuntimeId(Stone.Id);
	const uint16 GrassId = Registry.ResolveRuntimeId(Grass.Id);

	FMadChunkSampleGrid Grid;
	for (int32 Z = -1; Z <= MadFall::ChunkSize; ++Z)
	{
		for (int32 Y = -1; Y <= MadFall::ChunkSize; ++Y)
		{
			for (int32 X = -1; X <= MadFall::ChunkSize; ++X)
			{
				// A tilted ground plane, stone on one side of a diagonal line, grass on the other.
				const float Surface = 12.0f + X * 0.3f + Y * 0.2f;
				const int32 Index = FMadChunkSampleGrid::Index(X, Y, Z);
				Grid.Density[Index] = MadFall::DistanceToDensity(Surface - static_cast<float>(Z));
				Grid.BlockId[Index] = Grid.Density[Index] >= 128 ? (X + Y < 30 ? StoneId : GrassId) : MadFall::BlockTypeAir;
				Grid.Flags[Index] = 0;
			}
		}
	}

	MadFall::ChunkMesher::FMeshSettings Settings;
	FMadChunkMesh Mesh;
	MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);
	TestTrue(TEXT("both surfaces have a section"), Mesh.Sections.Num() >= 2);

	int32 WrongPattern = 0;
	int32 Blended = 0;
	for (const FMadMeshSection& Section : Mesh.Sections)
	{
		const FColor Own = MadFall::GetSurfaces().GetVertexColor(Section.MaterialClass);
		for (const FColor& Colour : Section.Colors)
		{
			WrongPattern += Colour.A != Own.A ? 1 : 0;
			Blended += (Colour.R != Own.R || Colour.G != Own.G || Colour.B != Own.B) ? 1 : 0;
		}
	}
	TestEqual(TEXT("every vertex carries its section's pattern, so no other pattern is interpolated in between"), WrongPattern, 0);
	TestTrue(TEXT("colour still blends across the boundary"), Blended > 0);
	return true;
}

// ===========================================================================
// Cubic (greedy) meshing
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadGreedyMesherTest,
	"MadFall.Mesher.GreedyCubic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadGreedyMesherTest::RunTest(const FString& Parameters)
{
	using namespace MadMesherTests;

	FMadBlockRegistry Registry;
	const FTestBlocks Blocks = BuildRegistry(Registry);

	MadFall::ChunkMesher::FMeshSettings Settings;
	Settings.bIsosurface = false;

	// --- a single block ---
	{
		const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[](int32 X, int32 Y, int32 Z) -> uint8
			{
				return (X == 5 && Y == 5 && Z == 5) ? 255 : 0;
			},
			/*bCubic*/ true);

		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);

		TestEqual(TEXT("one block is 12 triangles"), Mesh.TotalTriangles(), 12);
		TestEqual(TEXT("one block is 24 vertices (no sharing across faces)"), Mesh.TotalVertices(), 24);

		const FMeshAudit Audit = AuditMesh(Mesh);
		TestEqual(TEXT("no degenerate triangles"), Audit.DegenerateTriangles, 0);
		TestEqual(TEXT("no out-of-bounds vertices"), Audit.OutOfBoundsVertices, 0);

		// THE winding check. If bFlipWindingForUnreal were wrong, every one of
		// these 12 triangles would be backfacing and the block would render as
		// a hole rather than a cube.
		TestEqual(FString::Printf(
			TEXT("every triangle faces outward by Unreal's own normal formula (min dot %.3f)"), Audit.MinDot),
			Audit.BackfacingTriangles, 0);
	}

	// --- greedy merging actually merges ---
	{
		// A 32x32 slab one voxel thick. Un-merged that is 32*32*2 = 2048
		// triangles for the top face alone; merged, the top face is 2.
		const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[](int32 X, int32 Y, int32 Z) -> uint8
			{
				const bool bInside = X >= 0 && X < MadFall::ChunkSize
					&& Y >= 0 && Y < MadFall::ChunkSize && Z == 0;
				return bInside ? 255 : 0;
			},
			/*bCubic*/ true);

		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);

		// 2 large faces (top, bottom) + 4 edge strips, each of which merges into
		// one rectangle = 6 quads = 12 triangles.
		TestEqual(TEXT("a 32x32x1 slab greedy-merges to 12 triangles"), Mesh.TotalTriangles(), 12);

		const FMeshAudit Audit = AuditMesh(Mesh);
		TestEqual(TEXT("slab has no backfacing triangles"), Audit.BackfacingTriangles, 0);
		TestEqual(TEXT("slab has no degenerate triangles"), Audit.DegenerateTriangles, 0);
	}

	// --- interior faces are culled ---
	{
		// A solid 32^3 chunk whose margin is also solid has no visible face at
		// all. If the margin were ignored, this would emit the chunk's six
		// outer faces and every wall in the game would be hollow-looking.
		const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[](int32, int32, int32) -> uint8 { return 255; },
			/*bCubic*/ true);

		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);

		TestEqual(TEXT("a fully enclosed solid chunk emits nothing"), Mesh.TotalTriangles(), 0);
	}

	// --- two block types do not merge into one quad ---
	{
		const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[](int32 X, int32 Y, int32 Z) -> uint8
			{
				return (Z == 0 && X >= 0 && X < 4 && Y == 0) ? 255 : 0;
			},
			/*bCubic*/ true);

		FMadChunkSampleGrid Mixed = Grid;
		// Give the second half a block with a DIFFERENT material class. Two
		// blocks sharing a material class would correctly stay in one section,
		// so the test has to differ in the thing sections are keyed by.
		for (int32 X = 2; X < 4; ++X)
		{
			Mixed.BlockId[FMadChunkSampleGrid::Index(X, 0, 0)] = Blocks.Timber;
		}

		FMadChunkMesh Merged;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Merged);

		FMadChunkMesh Split;
		MadFall::ChunkMesher::BuildChunkMesh(Mixed, Registry, Settings, Split);

		TestTrue(TEXT("two block types produce more geometry than one"),
			Split.TotalTriangles() > Merged.TotalTriangles());
		TestTrue(TEXT("two block types produce two sections"), Split.Sections.Num() >= 2);
	}

	return true;
}

// ===========================================================================
// Corner ambient occlusion
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadCornerOcclusionTest,
	"MadFall.Mesher.CornerOcclusion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadCornerOcclusionTest::RunTest(const FString& Parameters)
{
	using namespace MadMesherTests;

	FMadBlockRegistry Registry;
	const FTestBlocks Blocks = BuildRegistry(Registry);
	MadFall::ChunkMesher::FMeshSettings Settings;
	Settings.bIsosurface = false;

	const float Floor = 1.0f * MadFall::VoxelSizeUU;   // top of a floor at Z = 0

	/**
	 * Highest occlusion of the floor's top-face vertices at voxel corner (X, Y),
	 * or -1 if no vertex is there - the inside of a merged quad, which is open
	 * by construction (only faces with the same occlusion at all four corners merge).
	 */
	auto TopOcclusionAt = [this, Floor](const FMadChunkMesh& Mesh, int32 X, int32 Y) -> int32
	{
		int32 Found = -1;
		for (const FMadMeshSection& Section : Mesh.Sections)
		{
			TestNotEqual(TEXT("occlusion per vertex"), Section.Occlusion.Num(), 0);
			for (int32 Index = 0; Index < Section.NumVertices(); ++Index)
			{
				const FVector3f& P = Section.Positions[Index];
				if (Section.Normals[Index].Z > 0.5f && FMath::IsNearlyEqual(P.Z, Floor)
					&& FMath::IsNearlyEqual(P.X, X * MadFall::VoxelSizeUU) && FMath::IsNearlyEqual(P.Y, Y * MadFall::VoxelSizeUU))
				{
					Found = FMath::Max(Found, static_cast<int32>(Section.Occlusion[Index] & 3));
					TestTrue(TEXT("a placed block's face is flagged for the bevel"), (Section.Occlusion[Index] & FMadMeshSection::CubicFaceFlag) != 0);
				}
			}
		}
		return Found;
	};

	// --- an open floor: nothing occluded, and it still merges to one quad a face ---
	{
		const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[](int32 X, int32 Y, int32 Z) -> uint8 { return (Z == 0 && X >= 0 && X < 8 && Y >= 0 && Y < 8) ? 255 : 0; },
			/*bCubic*/ true);
		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);
		TestEqual(TEXT("an open 8x8 slab still merges to 6 quads"), Mesh.TotalTriangles(), 12);
		TestEqual(TEXT("an open floor corner is unoccluded"), TopOcclusionAt(Mesh, 0, 0), 0);
		TestTrue(TEXT("an open floor centre is unoccluded"), TopOcclusionAt(Mesh, 4, 4) <= 0);
	}

	// --- a 2-voxel wall standing on the floor along X = 4 ---
	{
		const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[](int32 X, int32 Y, int32 Z) -> uint8
			{
				const bool bFloor = Z == 0 && X >= 0 && X < 8 && Y >= 0 && Y < 8;
				const bool bWall = X == 4 && Y >= 2 && Y < 6 && (Z == 1 || Z == 2);
				return (bFloor || bWall) ? 255 : 0;
			},
			/*bCubic*/ true);
		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);

		const FMeshAudit Audit = AuditMesh(Mesh);
		TestEqual(TEXT("occluded quads still face outward"), Audit.BackfacingTriangles, 0);
		TestEqual(TEXT("occluded quads are not degenerate"), Audit.DegenerateTriangles, 0);

		// The floor's top corner at (4, 3) touches the wall's base on one side:
		// one side occluder plus its diagonal along the wall = 2.
		TestEqual(TEXT("a floor corner along the foot of a wall is darkened"), TopOcclusionAt(Mesh, 4, 3), 2);
		// (4, 2) is the wall's end: only one block touches that corner.
		TestEqual(TEXT("a floor corner at the end of a wall is lightly darkened"), TopOcclusionAt(Mesh, 4, 2), 1);
		TestTrue(TEXT("the floor away from the wall stays open"), TopOcclusionAt(Mesh, 1, 1) <= 0);
		TestTrue(TEXT("the floor a voxel beyond the wall's end stays open"), TopOcclusionAt(Mesh, 4, 1) <= 0);

		// Merging must keep going away from the wall: far fewer quads than faces.
		TestTrue(FString::Printf(TEXT("the floor still merges away from the wall (%d triangles)"), Mesh.TotalTriangles()),
			Mesh.TotalTriangles() < 120);
	}

	// --- an inside corner: two walls meeting darken the corner fully ---
	{
		const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[](int32 X, int32 Y, int32 Z) -> uint8
			{
				const bool bFloor = Z == 0 && X >= 0 && X < 8 && Y >= 0 && Y < 8;
				const bool bWalls = Z == 1 && ((X == 0 && Y < 8) || (Y == 0 && X < 8));
				return (bFloor || bWalls) ? 255 : 0;
			},
			/*bCubic*/ true);
		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);
		TestEqual(TEXT("the floor corner where two walls meet is fully occluded"), TopOcclusionAt(Mesh, 1, 1), 3);
		TestEqual(TEXT("the floor along one wall is half occluded"), TopOcclusionAt(Mesh, 4, 1), 2);
	}

	return true;
}

// ===========================================================================
// Isosurface (Surface Nets) meshing
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSurfaceNetsTest,
	"MadFall.Mesher.SurfaceNets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSurfaceNetsTest::RunTest(const FString& Parameters)
{
	using namespace MadMesherTests;

	FMadBlockRegistry Registry;
	const FTestBlocks Blocks = BuildRegistry(Registry);

	MadFall::ChunkMesher::FMeshSettings Settings;
	Settings.bCubic = false;

	// --- a sphere ---
	{
		const FVector3f Centre(16.0f, 16.0f, 16.0f);
		constexpr float Radius = 10.0f;

		const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[Centre](int32 X, int32 Y, int32 Z) -> uint8
			{
				const float Distance = (FVector3f(
					static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z)) - Centre).Size();

				// A soft band around the surface, exactly as the generator will
				// produce. A binary field would mesh as voxel steps.
				const float Normalized = FMath::Clamp((Radius - Distance) * 0.5f + 0.5f, 0.0f, 1.0f);
				return static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
			});

		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);

		TestTrue(TEXT("a sphere produces geometry"), Mesh.TotalTriangles() > 100);

		const FMeshAudit Audit = AuditMesh(Mesh);
		TestEqual(TEXT("sphere has no out-of-bounds vertices"), Audit.OutOfBoundsVertices, 0);
		TestEqual(TEXT("sphere normals are unit length"), Audit.BadNormals, 0);
		TestEqual(FString::Printf(
			TEXT("every sphere triangle faces outward (min dot %.3f)"), Audit.MinDot),
			Audit.BackfacingTriangles, 0);

		// Surface Nets places one vertex per cell the surface passes through, so
		// a sphere of radius 10 voxels should have vertices roughly on its
		// surface. Checking the radius of every vertex catches a vertex-placement
		// bug that a triangle count alone would not.
		float MinRadius = TNumericLimits<float>::Max();
		float MaxRadius = 0.0f;

		for (const FMadMeshSection& Section : Mesh.Sections)
		{
			for (const FVector3f& Position : Section.Positions)
			{
				const FVector3f VoxelSpace = Position / MadFall::VoxelSizeUU - FVector3f(0.5f);
				const float R = (VoxelSpace - Centre).Size();
				MinRadius = FMath::Min(MinRadius, R);
				MaxRadius = FMath::Max(MaxRadius, R);
			}
		}

		TestTrue(FString::Printf(TEXT("sphere vertices sit near radius 10 (got %.2f..%.2f)"), MinRadius, MaxRadius),
			MinRadius > Radius - 2.0f && MaxRadius < Radius + 2.0f);
	}

	// --- empty and solid chunks are free ---
	{
		FMadChunkMesh Empty;
		MadFall::ChunkMesher::BuildChunkMesh(
			MakeGrid(Blocks.Rock, [](int32, int32, int32) -> uint8 { return 0; }), Registry, Settings, Empty);
		TestEqual(TEXT("an all-air chunk emits nothing"), Empty.TotalTriangles(), 0);

		FMadChunkMesh Solid;
		MadFall::ChunkMesher::BuildChunkMesh(
			MakeGrid(Blocks.Rock, [](int32, int32, int32) -> uint8 { return 255; }), Registry, Settings, Solid);
		TestEqual(TEXT("an all-solid chunk emits nothing"), Solid.TotalTriangles(), 0);
	}

	// --- cubic voxels are excluded from the isosurface ---
	{
		// Every voxel solid AND cubic. The isosurface path must see nothing,
		// because construction belongs to the greedy mesher. If it did not, a
		// placed block would bulge the terrain surface around itself.
		FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
			[](int32, int32 Y, int32) -> uint8 { return (Y < 16) ? 255 : 0; },
			/*bCubic*/ true);

		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);

		TestEqual(TEXT("cubic voxels contribute nothing to the isosurface"), Mesh.TotalTriangles(), 0);
	}

	return true;
}

// ===========================================================================
// Chunk seams
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadMesherSeamTest,
	"MadFall.Mesher.ChunkSeams",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadMesherSeamTest::RunTest(const FString& Parameters)
{
	using namespace MadMesherTests;

	FMadBlockRegistry Registry;
	const FTestBlocks Blocks = BuildRegistry(Registry);

	MadFall::ChunkMesher::FMeshSettings Settings;
	Settings.bCubic = false;

	// One continuous field sampled by two chunks side by side in X. Chunk A is
	// at chunk coordinate 0, chunk B at 1, so B's world X is offset by 32.
	auto Field = [](float WorldX, float WorldY, float WorldZ) -> uint8
	{
		const float Height = 16.0f + FMath::Sin(WorldX * 0.15f) * 6.0f + FMath::Cos(WorldY * 0.11f) * 4.0f;
		const float Normalized = FMath::Clamp((Height - WorldZ) * 0.5f + 0.5f, 0.0f, 1.0f);
		return static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
	};

	FMadChunkSampleGrid GridA = MakeGrid(Blocks.Rock, [&Field](int32 X, int32 Y, int32 Z) -> uint8
	{
		return Field(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
	});
	GridA.Coord = FMadChunkCoord(0, 0, 0);

	FMadChunkSampleGrid GridB = MakeGrid(Blocks.Rock, [&Field](int32 X, int32 Y, int32 Z) -> uint8
	{
		return Field(static_cast<float>(X + MadFall::ChunkSize), static_cast<float>(Y), static_cast<float>(Z));
	});
	GridB.Coord = FMadChunkCoord(1, 0, 0);

	FMadChunkMesh MeshA;
	FMadChunkMesh MeshB;
	MadFall::ChunkMesher::BuildChunkMesh(GridA, Registry, Settings, MeshA);
	MadFall::ChunkMesher::BuildChunkMesh(GridB, Registry, Settings, MeshB);

	TestTrue(TEXT("chunk A produced geometry"), MeshA.TotalTriangles() > 0);
	TestTrue(TEXT("chunk B produced geometry"), MeshB.TotalTriangles() > 0);

	// Collect vertices near the shared plane, in WORLD space.
	const float SeamX = MadFall::ChunkSize * MadFall::VoxelSizeUU;
	constexpr float Tolerance = 1.0f;   // 1 uu out of a 100 uu voxel

	auto CollectSeam = [&](const FMadChunkMesh& Mesh, float ChunkOffsetX, TArray<FVector3f>& Out)
	{
		for (const FMadMeshSection& Section : Mesh.Sections)
		{
			for (const FVector3f& Position : Section.Positions)
			{
				const FVector3f World = Position + FVector3f(ChunkOffsetX, 0.0f, 0.0f);
				if (FMath::Abs(World.X - SeamX) < MadFall::VoxelSizeUU * 0.51f)
				{
					Out.Add(World);
				}
			}
		}
	};

	TArray<FVector3f> SeamA;
	TArray<FVector3f> SeamB;
	CollectSeam(MeshA, 0.0f, SeamA);
	CollectSeam(MeshB, SeamX, SeamB);

	TestTrue(TEXT("chunk A has vertices at the shared boundary"), SeamA.Num() > 0);
	TestTrue(TEXT("chunk B has vertices at the shared boundary"), SeamB.Num() > 0);

	// Every boundary vertex one chunk produces must be matched by the other.
	// This is what the one-voxel sample margin exists for: without it each
	// chunk would end its surface at its own edge and the world would be full
	// of one-voxel cracks that only show up where two chunks meet.
	int32 Unmatched = 0;
	FVector3f WorstVertex = FVector3f::ZeroVector;
	float WorstDistance = 0.0f;

	for (const FVector3f& A : SeamA)
	{
		float Closest = TNumericLimits<float>::Max();
		for (const FVector3f& B : SeamB)
		{
			Closest = FMath::Min(Closest, (A - B).Size());
		}

		if (Closest > Tolerance)
		{
			++Unmatched;
			if (Closest > WorstDistance)
			{
				WorstDistance = Closest;
				WorstVertex = A;
			}
		}
	}

	TestEqual(FString::Printf(
		TEXT("every boundary vertex is shared by both chunks (worst gap %.3f uu at %s)"),
		WorstDistance, *WorstVertex.ToString()),
		Unmatched, 0);

	return true;
}

// ===========================================================================
// Performance
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadMesherPerformanceTest,
	"MadFall.Mesher.Performance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadMesherPerformanceTest::RunTest(const FString& Parameters)
{
	using namespace MadMesherTests;

	FMadBlockRegistry Registry;
	const FTestBlocks Blocks = BuildRegistry(Registry);

	// A worst-case chunk: a surface that passes through most of it, so nearly
	// every cell produces a vertex.
	const FMadChunkSampleGrid Grid = MakeGrid(Blocks.Rock,
		[](int32 X, int32 Y, int32 Z) -> uint8
		{
			const float Height = 16.0f
				+ FMath::Sin(X * 0.4f) * 6.0f
				+ FMath::Cos(Y * 0.35f) * 6.0f
				+ FMath::Sin((X + Y) * 0.2f) * 3.0f;
			const float Normalized = FMath::Clamp((Height - Z) * 0.5f + 0.5f, 0.0f, 1.0f);
			return static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));
		});

	MadFall::ChunkMesher::FMeshSettings Settings;

	// Warm up, then measure, so the first run's allocations are not counted.
	FMadChunkMesh Warmup;
	MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Warmup);

	constexpr int32 Iterations = 8;
	double TotalMs = 0.0;
	double WorstMs = 0.0;
	FMadChunkMesh Mesh;

	for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
	{
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Mesh);
		TotalMs += Mesh.BuildMilliseconds;
		WorstMs = FMath::Max(WorstMs, Mesh.BuildMilliseconds);
	}

	const double MeanMs = TotalMs / Iterations;

	AddInfo(FString::Printf(
		TEXT("Worst-case chunk: %d verts, %d tris. Mesh time mean %.2f ms, worst %.2f ms (worker thread)."),
		Mesh.TotalVertices(), Mesh.TotalTriangles(), MeanMs, WorstMs));

	TestTrue(TEXT("the worst-case chunk produced geometry"), Mesh.TotalTriangles() > 1000);

    // This runs on a WORKER thread, so it is not bound by the 2 ms game-thread
    // rule. The bound that matters is throughput: at 100 ms a chunk, a player
    // flying through the world would out-run the mesher on any core count.
    // The threshold is deliberately loose - this test exists to catch an
    // algorithmic regression (an accidental O(n^2), a per-voxel map lookup),
    // not to police microseconds on whatever machine CI runs on.
	TestTrue(FString::Printf(TEXT("a worst-case chunk meshes in under 100 ms (took %.2f ms)"), WorstMs),
		WorstMs < 100.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadFarTerrain.h"
#include "MadWorldGenerator.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadFarTerrainTest,
	"MadFall.Mesher.FarTerrain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadFarTerrainTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::FarTerrain;
	const TArray<FMadFarLevel>& Levels = GetLevels();

	// --- which tiles ---------------------------------------------------------------
	const FVector2D Viewer(37.0, -90.0);
	const int32 Hidden = 224;
	TArray<FMadFarTileKey> Wanted;
	GatherWanted(Viewer, Hidden, 100, Wanted);
	TestTrue(TEXT("tiles are wanted"), Wanted.Num() > 50);

	auto Covers = [&Levels](const FMadFarTileKey& Key, const FVector2D& P)
	{
		const FMadFarLevel& L = Levels[Key.Level];
		return P.X >= Key.X * L.TileVoxels && P.X < (Key.X + 1) * L.TileVoxels && P.Y >= Key.Y * L.TileVoxels && P.Y < (Key.Y + 1) * L.TileVoxels;
	};

	for (const FMadFarTileKey& Key : Wanted)
	{
		if (Key.Level != 0)
		{
			continue;
		}
		const FMadFarLevel& L = Levels[0];
		const bool bInside = Key.X * L.TileVoxels >= Viewer.X - Hidden && (Key.X + 1) * L.TileVoxels <= Viewer.X + Hidden
			&& Key.Y * L.TileVoxels >= Viewer.Y - Hidden && (Key.Y + 1) * L.TileVoxels <= Viewer.Y + Hidden;
		if (bInside)
		{
			AddError(FString::Printf(TEXT("level-0 tile %d,%d lies wholly under the streamed chunks"), Key.X, Key.Y));
		}
	}

	// Every point from the chunk edge to 4 km is under at least one tile: no holes in the landscape.
	int32 Holes = 0;
	for (double Y = -4000.0; Y <= 4000.0; Y += 97.0)
	{
		for (double X = -4000.0; X <= 4000.0; X += 97.0)
		{
			const FVector2D P = Viewer + FVector2D(X, Y);
			if (FMath::Max(FMath::Abs(X), FMath::Abs(Y)) < Hidden)
			{
				continue;
			}
			if (!Wanted.ContainsByPredicate([&](const FMadFarTileKey& Key) { return Covers(Key, P); }))
			{
				++Holes;
			}
		}
	}
	TestEqual(TEXT("no holes between the chunk edge and 4 km"), Holes, 0);

	// No duplicates, and a smaller range asks for fewer tiles.
	TSet<FMadFarTileKey> Unique(Wanted);
	TestEqual(TEXT("no tile twice"), Unique.Num(), Wanted.Num());
	TArray<FMadFarTileKey> Near;
	GatherWanted(Viewer, Hidden, 50, Near);
	TestTrue(TEXT("half the range, fewer tiles"), Near.Num() < Wanted.Num());

	// --- a tile's shape ---------------------------------------------------------------
	FMadBlockRegistry Blocks;
	FMadBiomeRegistry Biomes;
	TArray<FMadDefinitionError> Errors;
	Blocks.BeginLoad();
	Blocks.AddFromDirectory(FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"), TEXT("blocks")), FName(TEXT("madfall")), Errors);
	Blocks.FinishLoad(Errors);
	Biomes.BeginLoad();
	Biomes.AddFromDirectory(FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"), TEXT("biomes")), FName(TEXT("madfall")), Errors);
	Biomes.FinishLoad(Errors);
	FMadWorldGenSettings Settings;
	Settings.Seed = 20260913u;
	const FMadWorldGenerator Generator(Settings, Biomes, Blocks);

	TArray<FColor> Colours;
	Colours.Init(FColor(10, 200, 10), Biomes.Num());
	const FColor Water(0, 0, 255);
	for (int32 LevelIndex = 0; LevelIndex < Levels.Num(); ++LevelIndex)
	{
		const FMadFarLevel& L = Levels[LevelIndex];
		const FMadFarTileKey Key{ LevelIndex, 3, -2 };
		FMadFarTileMesh Mesh;
		BuildTile(Generator, Key, Colours, Water, Mesh);

		const int32 Side = L.TileVoxels / L.SpacingVoxels + 1;
		TestEqual(*FString::Printf(TEXT("level %d: surface plus four skirts"), LevelIndex), Mesh.Positions.Num(), Side * Side + 4 * Side);
		TestEqual(*FString::Printf(TEXT("level %d: triangles"), LevelIndex), Mesh.Triangles.Num(), 6 * (Side - 1) * (Side - 1) + 4 * 6 * (Side - 1));
		TestTrue(*FString::Printf(TEXT("level %d: attributes line up"), LevelIndex), Mesh.Normals.Num() == Mesh.Positions.Num() && Mesh.Colors.Num() == Mesh.Positions.Num());

		const float SeaTop = static_cast<float>(Settings.SeaLevel + 1);
		int32 Wrong = 0;
		for (int32 J = 0; J < Side; J += 3)
		{
			for (int32 I = 0; I < Side; I += 3)
			{
				const float WX = Key.X * L.TileVoxels + I * L.SpacingVoxels;
				const float WY = Key.Y * L.TileVoxels + J * L.SpacingVoxels;
				const float Expected = (FMath::Max(Generator.GetSurfaceHeight(WX, WY), SeaTop) - L.DropVoxels) * MadFall::VoxelSizeUU;
				const FVector& P = Mesh.Positions[I + J * Side];
				Wrong += FMath::IsNearlyEqual(P.Z, Expected, 1.0) && FMath::IsNearlyEqual(P.X, I * L.SpacingVoxels * MadFall::VoxelSizeUU, 0.01) ? 0 : 1;
				Wrong += Mesh.Normals[I + J * Side].Z > 0.0 ? 0 : 1;
			}
		}
		TestEqual(*FString::Printf(TEXT("level %d: vertices sit on the generator's surface, sunk by the drop, facing up"), LevelIndex), Wrong, 0);

		// The first skirt hangs from the south edge, vertex for vertex.
		int32 SkirtsNotBelow = 0;
		for (int32 K = 0; K < Side; ++K)
		{
			SkirtsNotBelow += Mesh.Positions[Side * Side + K].Z < Mesh.Positions[K].Z - 100.0 ? 0 : 1;
		}
		TestEqual(*FString::Printf(TEXT("level %d: skirts hang below the edge"), LevelIndex), SkirtsNotBelow, 0);
	}

	// Two neighbouring tiles of a level agree on their shared edge: no cracks within a level.
	FMadFarTileMesh Left;
	FMadFarTileMesh Right;
	BuildTile(Generator, { 1, 0, 0 }, Colours, Water, Left);
	BuildTile(Generator, { 1, 1, 0 }, Colours, Water, Right);
	const FMadFarLevel& L1 = Levels[1];
	const int32 Side = L1.TileVoxels / L1.SpacingVoxels + 1;
	int32 Mismatches = 0;
	for (int32 J = 0; J < Side; ++J)
	{
		Mismatches += FMath::IsNearlyEqual(Left.Positions[(Side - 1) + J * Side].Z, Right.Positions[J * Side].Z, 0.01) ? 0 : 1;
	}
	TestEqual(TEXT("adjacent tiles share their edge heights"), Mismatches, 0);
	return true;
}

#endif

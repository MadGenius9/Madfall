// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadBiomeDefinition.h"
#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadWorldGenerator.h"
#include "MadWorldScatter.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadScatterTests
{
	TSharedRef<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		return Object.IsValid() ? Object.ToSharedRef() : MakeShared<FJsonObject>();
	}

	const FIntVector Steps[6] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadScatterDefinitionTest,
	"MadFall.WorldGen.ScatterDefinitions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadScatterDefinitionTest::RunTest(const FString& Parameters)
{
	using namespace MadScatterTests;

	// --- parsing ---
	{
		const TSharedRef<FJsonObject> Json = ParseJson(TEXT(R"({
			"schema": "madfall.biome/1", "id": "test:woods",
			"scatter": [
				{ "feature": "tree", "block": "test:log", "leaves": "test:leaves", "chance": 0.02, "height": [5, 7], "radius": [2, 3] },
				{ "feature": "boulder", "block": "test:rock", "chance": 0.001, "radius": [9, 12] },
				{ "feature": "volcano", "block": "test:rock" },
				{ "feature": "plant", "chance": 0.1 },
				{ "feature": "plant", "block": "test:bush", "chance": 0.003, "colour": "red" }
			]
		})"));

		FMadBiomeDefinitionData Data;
		TArray<FMadDefinitionError> Errors;
		MadFall::BiomeDefinitionJson::ParseObject(Json, TEXT("test.json"), FName(TEXT("test")), Data, Errors);

		TestEqual(TEXT("three valid features kept"), Data.Scatter.Num(), 3);
		if (Data.Scatter.Num() == 3)
		{
			const FMadScatterFeature& Tree = Data.Scatter[0];
			TestEqual(TEXT("tree kind"), Tree.Kind, EMadScatterKind::Tree);
			TestEqual(TEXT("tree leaves"), Tree.Leaves, FName(TEXT("test:leaves")));
			TestEqual(TEXT("tree height range"), Tree.MinHeight * 100 + Tree.MaxHeight, 507);
			TestEqual(TEXT("tree reach is its canopy plus one"), Tree.GetReach(), 4);
			TestEqual(TEXT("oversized boulder clamped"), Data.Scatter[1].MaxRadius, 6.0f);
			TestEqual(TEXT("plant reaches no further than its own column"), Data.Scatter[2].GetReach(), 0);
		}

		auto HasError = [&Errors](const TCHAR* Pointer)
		{
			return Errors.ContainsByPredicate([Pointer](const FMadDefinitionError& E) { return E.Pointer.StartsWith(Pointer); });
		};
		TestTrue(TEXT("oversized radius reported"), HasError(TEXT("/scatter/1/radius")));
		TestTrue(TEXT("unknown feature reported"), HasError(TEXT("/scatter/2/feature")));
		TestTrue(TEXT("missing block reported"), HasError(TEXT("/scatter/3/block")));
		TestTrue(TEXT("unknown field reported"), HasError(TEXT("/colour")));
	}

	// --- tree shape ---
	for (uint32 Seed = 1; Seed <= 40; ++Seed)
	{
		const int32 Height = 4 + static_cast<int32>(Seed % 4);
		const float Radius = 1.5f + static_cast<float>(Seed % 4) * 0.5f;
		TArray<FIntVector> Trunk, Leaves;
		MadFall::Scatter::BuildTree(Seed, Height, Radius, true, Trunk, Leaves);

		TArray<FIntVector> TrunkAgain, LeavesAgain;
		MadFall::Scatter::BuildTree(Seed, Height, Radius, true, TrunkAgain, LeavesAgain);
		if (Leaves != LeavesAgain || Trunk != TrunkAgain)
		{
			AddError(FString::Printf(TEXT("seed %u: the same tree came out differently"), Seed));
		}

		if (Trunk.Num() != Height)
		{
			AddError(FString::Printf(TEXT("seed %u: trunk has %d voxels, expected %d"), Seed, Trunk.Num(), Height));
		}
		if (Leaves.Num() < 8)
		{
			AddError(FString::Printf(TEXT("seed %u: only %d leaves"), Seed, Leaves.Num()));
		}

		// Every leaf connects to the trunk, and none is further from it than a
		// leaf's structural span (4) - otherwise generated trees would drop
		// their outer leaves the moment they load.
		TSet<FIntVector> Reached(Trunk);
		TArray<FIntVector> Frontier = Trunk;
		const TSet<FIntVector> LeafSet(Leaves);
		while (Frontier.Num() > 0)
		{
			const FIntVector Current = Frontier.Pop();
			for (const FIntVector& Step : Steps)
			{
				const FIntVector Next = Current + Step;
				if (LeafSet.Contains(Next) && !Reached.Contains(Next))
				{
					Reached.Add(Next);
					Frontier.Add(Next);
				}
			}
		}
		for (const FIntVector& Leaf : Leaves)
		{
			if (!Reached.Contains(Leaf))
			{
				AddError(FString::Printf(TEXT("seed %u: leaf %s is not connected to the trunk"), Seed, *Leaf.ToString()));
			}
			if (FMath::Abs(Leaf.X) + FMath::Abs(Leaf.Y) > 4)
			{
				AddError(FString::Printf(TEXT("seed %u: leaf %s is beyond a leaf's span"), Seed, *Leaf.ToString()));
			}
			if (Trunk.Contains(Leaf))
			{
				AddError(FString::Printf(TEXT("seed %u: leaf %s overlaps the trunk"), Seed, *Leaf.ToString()));
			}
		}
	}

	// A bare trunk (cactus) has no leaves.
	{
		TArray<FIntVector> Trunk, Leaves;
		MadFall::Scatter::BuildTree(7, 3, 2.0f, false, Trunk, Leaves);
		TestEqual(TEXT("bare trunk"), Trunk.Num() * 100 + Leaves.Num(), 300);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadScatterGenerationTest,
	"MadFall.WorldGen.Scatter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadScatterGenerationTest::RunTest(const FString& Parameters)
{
	using namespace MadScatterTests;

	FMadBlockRegistry Blocks;
	FMadBiomeRegistry Biomes;
	TArray<FMadDefinitionError> Errors;
	Blocks.BeginLoad();
	Blocks.AddFromDirectory(FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"), TEXT("blocks")), FName(TEXT("madfall")), Errors);
	Blocks.FinishLoad(Errors);
	Biomes.BeginLoad();
	Biomes.AddFromDirectory(FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"), TEXT("biomes")), FName(TEXT("madfall")), Errors);
	Biomes.FinishLoad(Errors);

	const int32 Forest = Biomes.FindIndex(FName(TEXT("madfall:forest")));
	if (!TestTrue(TEXT("the forest biome loaded"), Forest != INDEX_NONE))
	{
		return false;
	}
	TestTrue(TEXT("the forest scatters trees"), Biomes.Get(Forest).Scatter.ContainsByPredicate(
		[](const FMadScatterFeature& F) { return F.Kind == EMadScatterKind::Tree; }));

	FMadWorldGenSettings Settings;
	Settings.Seed = 20260913u;
	FMadWorldGenerator Generator(Settings, Biomes, Blocks);

	// Find a chunk well inside a forest.
	FIntPoint ForestColumn;
	if (!TestTrue(TEXT("found a forest within reach of the origin"), Generator.FindBiomeNear(Forest, FIntPoint::ZeroValue, 6000, ForestColumn)))
	{
		return false;
	}

	const int32 SurfaceZ = FMath::FloorToInt(Generator.GetSurfaceHeight(static_cast<float>(ForestColumn.X), static_cast<float>(ForestColumn.Y)));
	const FMadChunkCoord Centre = MadFall::WorldToChunk(ForestColumn.X, ForestColumn.Y, SurfaceZ);

	// 3x3x3 chunks, generated independently, merged into one voxel lookup.
	TMap<FMadChunkCoord, FMadChunkStorage> Chunks;
	for (int32 DZ = -1; DZ <= 1; ++DZ)
	{
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const FMadChunkCoord Coord(Centre.X + DX, Centre.Y + DY, Centre.Z + DZ);
				Generator.GenerateChunk(Coord, Chunks.Add(Coord));
			}
		}
	}
	auto GetVoxel = [&Chunks](const FIntVector& World, bool& bKnown) -> FMadVoxel
	{
		const FMadChunkCoord Coord = MadFall::WorldToChunk(World.X, World.Y, World.Z);
		const FMadChunkStorage* Storage = Chunks.Find(Coord);
		bKnown = Storage != nullptr;
		if (!Storage)
		{
			return FMadVoxel::Air();
		}
		int32 LX, LY, LZ;
		MadFall::WorldToLocal(World.X, World.Y, World.Z, LX, LY, LZ);
		return Storage->GetVoxel(LX, LY, LZ);
	};

	const uint16 Log = Blocks.ResolveRuntimeId(FName(TEXT("madfall:oak_log")));
	const uint16 Leaves = Blocks.ResolveRuntimeId(FName(TEXT("madfall:oak_leaves")));
	const uint16 Bush = Blocks.ResolveRuntimeId(FName(TEXT("madfall:berry_bush")));

	// Every tree voxel in the centre chunk, and the grounded trunk voxels anywhere.
	TArray<FIntVector> CentreTree;
	TArray<FIntVector> Grounded;
	int32 Logs = 0, LeafCount = 0, Bushes = 0, NonCubic = 0, Perched = 0;
	for (const TPair<FMadChunkCoord, FMadChunkStorage>& Pair : Chunks)
	{
		for (int32 Z = 0; Z < MadFall::ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < MadFall::ChunkSize; ++Y)
			{
				for (int32 X = 0; X < MadFall::ChunkSize; ++X)
				{
					const FMadVoxel Voxel = Pair.Value.GetVoxel(X, Y, Z);
					if (Voxel.BlockTypeID != Log && Voxel.BlockTypeID != Leaves && Voxel.BlockTypeID != Bush)
					{
						continue;
					}
					int32 WX, WY, WZ;
					MadFall::ChunkToWorld(Pair.Key, X, Y, Z, WX, WY, WZ);
					const FIntVector World(WX, WY, WZ);
					NonCubic += Voxel.HasFlag(EMadVoxelFlags::Cubic) ? 0 : 1;

					if (Voxel.BlockTypeID == Bush)
					{
						++Bushes;
						bool bKnown = false;
						const FMadVoxel Below = GetVoxel(World - FIntVector(0, 0, 1), bKnown);
						if (bKnown && (!Below.IsSolid() || Below.HasFlag(EMadVoxelFlags::Cubic)))
						{
							AddError(FString::Printf(TEXT("berry bush at %s is not standing on terrain"), *World.ToString()));
						}
						else if (bKnown && Below.Density < 255)
						{
							// A partly full ground voxel puts the smooth surface below
							// the cube: the bush floats (seen in play, 2026-09-21).
							++Perched;
						}
						continue;
					}

					if (Pair.Key == Centre)
					{
						CentreTree.Add(World);
					}
					(Voxel.BlockTypeID == Log ? Logs : LeafCount) += 1;

					// Resting on terrain counts as grounded: a trunk that runs into an
					// overhang continues above it, carried by the rock.
					bool bKnown = false;
					const FMadVoxel Below = GetVoxel(World - FIntVector(0, 0, 1), bKnown);
					if (Below.IsSolid() && !Below.HasFlag(EMadVoxelFlags::Cubic))
					{
						Grounded.Add(World);
						if (Voxel.BlockTypeID == Log && Below.Density < 255)
						{
							++Perched;
							FString Column;
							for (int32 CZ = World.Z - 5; CZ <= World.Z + 2; ++CZ)
							{
								bool bK = false;
								const FMadVoxel V = GetVoxel(FIntVector(World.X, World.Y, CZ), bK);
								Column += FString::Printf(TEXT(" z%d:%s/%d%s"), CZ, *Blocks.GetStringId(V.BlockTypeID).ToString(), V.Density, V.HasFlag(EMadVoxelFlags::Cubic) ? TEXT("c") : TEXT(""));
							}
							AddWarning(FString::Printf(TEXT("log at %s stands on density %d:%s"), *World.ToString(), Below.Density, *Column));
						}
					}
				}
			}
		}
	}

	AddInfo(FString::Printf(TEXT("Forest at %d,%d (chunk %s): %d logs, %d leaves, %d berry bushes over 27 chunks."),
		ForestColumn.X, ForestColumn.Y, *Centre.ToString(), Logs, LeafCount, Bushes));
	TestTrue(TEXT("a forest grows trees"), Logs > 20 && LeafCount > 100);
	TestEqual(TEXT("scattered trees and plants are construction-style (cubic) voxels"), NonCubic, 0);
	TestEqual(TEXT("every trunk and bush stands on a full ground voxel, so the surface meets it"), Perched, 0);

	// Connectivity across chunk borders: flood from grounded trunks through
	// logs and leaves in all 27 chunks. A tree cut off at a border - its canopy
	// in one chunk, its trunk missing from the next - leaves voxels unreached.
	TSet<FIntVector> Reached(Grounded);
	TArray<FIntVector> Frontier = Grounded;
	while (Frontier.Num() > 0)
	{
		const FIntVector Current = Frontier.Pop();
		for (const FIntVector& Step : Steps)
		{
			const FIntVector Next = Current + Step;
			if (Reached.Contains(Next))
			{
				continue;
			}
			bool bKnown = false;
			const FMadVoxel Voxel = GetVoxel(Next, bKnown);
			if (bKnown && (Voxel.BlockTypeID == Log || Voxel.BlockTypeID == Leaves))
			{
				Reached.Add(Next);
				Frontier.Add(Next);
			}
		}
	}
	int32 Floating = 0;
	for (const FIntVector& Voxel : CentreTree)
	{
		if (!Reached.Contains(Voxel))
		{
			if (++Floating <= 5)
			{
				AddError(FString::Printf(TEXT("tree voxel %s in the centre chunk is not connected to a trunk on the ground"), *Voxel.ToString()));
			}
		}
	}
	TestEqual(TEXT("no floating tree voxels in the centre chunk"), Floating, 0);

	return true;
}

#endif

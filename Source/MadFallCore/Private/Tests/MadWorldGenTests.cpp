// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadNoise.h"
#include "MadWorldGenerator.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadWorldGenTests
{
	/**
	 * Loads the project's real block and biome definitions.
	 *
	 * Deliberately the shipped content rather than fixtures: these tests are as
	 * much about the definitions being coherent as about the generator code, and
	 * a biome whose climate range no noise value ever reaches is a content bug
	 * that a fixture would hide.
	 */
	struct FGenFixture
	{
		FMadBlockRegistry Blocks;
		FMadBiomeRegistry BiomeRegistry;
		TArray<FMadDefinitionError> Errors;

		FGenFixture()
		{
			Blocks.BeginLoad();
			Blocks.AddFromDirectory(
				FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"), TEXT("blocks")),
				FName(TEXT("madfall")), Errors);
			Blocks.FinishLoad(Errors);

			BiomeRegistry.BeginLoad();
			BiomeRegistry.AddFromDirectory(
				FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"), TEXT("biomes")),
				FName(TEXT("madfall")), Errors);
			BiomeRegistry.FinishLoad(Errors);
		}

		FMadWorldGenerator MakeGenerator(uint32 Seed)
		{
			FMadWorldGenSettings Settings;
			Settings.Seed = Seed;
			return FMadWorldGenerator(Settings, BiomeRegistry, Blocks);
		}
	};
}

// ===========================================================================
// Noise
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadNoiseTest,
	"MadFall.WorldGen.Noise",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadNoiseTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Noise;

	// --- determinism ---
	TestEqual(TEXT("the hash is deterministic"), HashInt(12345u), HashInt(12345u));
	TestNotEqual(TEXT("different inputs hash differently"), HashInt(1u), HashInt(2u));
	TestNotEqual(TEXT("mirrored coordinates hash differently"),
		Hash3(1, 2, 3, 99u), Hash3(3, 2, 1, 99u));

	TestEqual(TEXT("gradient noise is deterministic"),
		Gradient3D(1.5f, 2.5f, 3.5f, 42u), Gradient3D(1.5f, 2.5f, 3.5f, 42u));
	TestNotEqual(TEXT("a different seed gives different noise"),
		Gradient3D(1.5f, 2.5f, 3.5f, 42u), Gradient3D(1.5f, 2.5f, 3.5f, 43u));

	// --- value at lattice points ---
	// Gradient noise is exactly 0 at integer lattice points by construction.
	// If this drifts, the fade or the gradient selection is broken.
	for (int32 X = -2; X <= 2; ++X)
	{
		for (int32 Y = -2; Y <= 2; ++Y)
		{
			TestTrue(FString::Printf(TEXT("2D noise is ~0 at lattice point (%d, %d)"), X, Y),
				FMath::Abs(Gradient2D(static_cast<float>(X), static_cast<float>(Y), 7u)) < 0.001f);
		}
	}

	// --- range ---
	float MinValue = TNumericLimits<float>::Max();
	float MaxValue = -TNumericLimits<float>::Max();
	double Sum = 0.0;
	int32 Samples = 0;

	for (int32 X = 0; X < 200; ++X)
	{
		for (int32 Y = 0; Y < 200; ++Y)
		{
			const float Value = Gradient2D(X * 0.37f, Y * 0.37f, 1234u);
			MinValue = FMath::Min(MinValue, Value);
			MaxValue = FMath::Max(MaxValue, Value);
			Sum += Value;
			++Samples;
		}
	}

	AddInfo(FString::Printf(TEXT("Gradient2D over 40000 samples: min %.3f mean %.4f max %.3f"),
		MinValue, Sum / Samples, MaxValue));

	TestTrue(TEXT("2D noise stays within [-1.1, 1.1]"), MinValue > -1.1f && MaxValue < 1.1f);
	TestTrue(TEXT("2D noise actually uses its range"), MinValue < -0.5f && MaxValue > 0.5f);
	TestTrue(TEXT("2D noise is centred near zero"), FMath::Abs(Sum / Samples) < 0.05);

	// --- the chunk RNG is a function of the chunk, not of call order ---
	{
		FChunkRandom First(99u, 3, -4, 1);
		FChunkRandom Second(99u, 3, -4, 1);

		bool bIdentical = true;
		for (int32 Draw = 0; Draw < 32; ++Draw)
		{
			if (First.NextUInt() != Second.NextUInt())
			{
				bIdentical = false;
				break;
			}
		}
		TestTrue(TEXT("two RNGs for the same chunk produce the same stream"), bIdentical);

		FChunkRandom Neighbour(99u, 4, -4, 1);
		FChunkRandom Origin(99u, 3, -4, 1);
		TestNotEqual(TEXT("a neighbouring chunk gets a different stream"),
			Neighbour.NextUInt(), Origin.NextUInt());
	}

	return true;
}

// ===========================================================================
// Determinism
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWorldGenDeterminismTest,
	"MadFall.WorldGen.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWorldGenDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace MadWorldGenTests;

	FGenFixture Fixture;

	if (!TestTrue(TEXT("the project's biome definitions loaded"), Fixture.BiomeRegistry.Num() > 0))
	{
		return false;
	}

	const FMadChunkCoord Coord(3, -5, 0);

	// --- the same generator, twice ---
	{
		FMadWorldGenerator Generator = Fixture.MakeGenerator(20260912u);

		FMadChunkStorage First;
		FMadChunkStorage Second;
		Generator.GenerateChunk(Coord, First);
		Generator.GenerateChunk(Coord, Second);

		TestTrue(TEXT("regenerating a chunk produces an identical result"),
			First.EqualsVoxelwise(Second));
	}

	// --- two generators, same seed ---
	// This is the property that makes a seed shareable: a different process,
	// with a different history, must produce the same world.
	{
		FMadWorldGenerator A = Fixture.MakeGenerator(20260912u);
		FMadWorldGenerator B = Fixture.MakeGenerator(20260912u);

		FMadChunkStorage FromA;
		FMadChunkStorage FromB;

		// Generate a different chunk from B first, to prove generation carries
		// no state between calls.
		FMadChunkStorage Scratch;
		B.GenerateChunk(FMadChunkCoord(100, 100, 0), Scratch);

		A.GenerateChunk(Coord, FromA);
		B.GenerateChunk(Coord, FromB);

		TestTrue(TEXT("two generators with the same seed agree, regardless of call order"),
			FromA.EqualsVoxelwise(FromB));
	}

	// --- different seeds ---
	{
		FMadWorldGenerator A = Fixture.MakeGenerator(1u);
		FMadWorldGenerator B = Fixture.MakeGenerator(2u);

		FMadChunkStorage FromA;
		FMadChunkStorage FromB;
		A.GenerateChunk(Coord, FromA);
		B.GenerateChunk(Coord, FromB);

		TestFalse(TEXT("a different seed produces a different world"),
			FromA.EqualsVoxelwise(FromB));

		TestNotEqual(TEXT("a different seed produces a different generation version"),
			A.GetSettings().GetGenerationVersion(), B.GetSettings().GetGenerationVersion());
	}

	// --- settings changes are detectable ---
	{
		FMadWorldGenSettings Base;
		Base.Seed = 7u;

		FMadWorldGenSettings Changed = Base;
		Changed.CaveThreshold += 0.05f;

		TestNotEqual(TEXT("changing a generation parameter changes the generation version"),
			Base.GetGenerationVersion(), Changed.GetGenerationVersion());

		FMadWorldGenSettings Same = Base;
		TestEqual(TEXT("identical settings produce the same generation version"),
			Base.GetGenerationVersion(), Same.GetGenerationVersion());
	}

	return true;
}

// ===========================================================================
// Terrain shape and composition
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWorldGenTerrainTest,
	"MadFall.WorldGen.Terrain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWorldGenTerrainTest::RunTest(const FString& Parameters)
{
	using namespace MadWorldGenTests;

	FGenFixture Fixture;
	if (!TestTrue(TEXT("biome definitions loaded"), Fixture.BiomeRegistry.Num() > 0))
	{
		return false;
	}

	FMadWorldGenerator Generator = Fixture.MakeGenerator(20260912u);
	const FMadWorldGenSettings& Settings = Generator.GetSettings();

	const uint16 BedrockId = Fixture.Blocks.ResolveRuntimeId(FName(TEXT("madfall:bedrock")));
	const uint16 WaterId = Fixture.Blocks.ResolveRuntimeId(FName(TEXT("madfall:water")));

	// --- the world has a floor ---
	// A cave or an overhang that reached the bottom of the world would be a
	// hole out of the level, and a player who fell through it would fall
	// forever. Bedrock is unconditional for a reason.
	{
		FMadChunkStorage Bottom;
		Generator.GenerateChunk(FMadChunkCoord(0, 0, MadFall::WorldMinChunkZ), Bottom);

		int32 NonBedrockAtFloor = 0;
		for (int32 Y = 0; Y < MadFall::ChunkSize; ++Y)
		{
			for (int32 X = 0; X < MadFall::ChunkSize; ++X)
			{
				const FMadVoxel Voxel = Bottom.GetVoxel(X, Y, 0);
				if (Voxel.BlockTypeID != BedrockId || Voxel.Density < 128)
				{
					++NonBedrockAtFloor;
				}
			}
		}

		TestEqual(TEXT("the bottom layer of the world is solid bedrock everywhere"),
			NonBedrockAtFloor, 0);
	}

	// --- surface heights stay inside the world ---
	{
		float MinHeight = TNumericLimits<float>::Max();
		float MaxHeight = -TNumericLimits<float>::Max();

		for (int32 Y = -2048; Y <= 2048; Y += 64)
		{
			for (int32 X = -2048; X <= 2048; X += 64)
			{
				const float Height = Generator.GetSurfaceHeight(static_cast<float>(X), static_cast<float>(Y));
				MinHeight = FMath::Min(MinHeight, Height);
				MaxHeight = FMath::Max(MaxHeight, Height);
			}
		}

		AddInfo(FString::Printf(TEXT("Surface height over a 4096x4096 area: %.1f .. %.1f"), MinHeight, MaxHeight));

		// Comfortably inside, not merely inside: terrain that grazes the build
		// ceiling leaves players nowhere to build.
		TestTrue(FString::Printf(TEXT("terrain stays above bedrock (min %.1f)"), MinHeight),
			MinHeight > MadFall::WorldMinZ + 16);
		TestTrue(FString::Printf(TEXT("terrain leaves headroom below the build ceiling (max %.1f)"), MaxHeight),
			MaxHeight < MadFall::WorldMaxZ - 64);
	}

	// --- the climate fields reach their extremes ---
	// A biome whose climate range sits where the noise never goes is
	// unreachable and silently absent. This is the test that would have caught
	// highlands covering 0% of the world.
	{
		float MinContinent = TNumericLimits<float>::Max();
		float MaxContinent = -TNumericLimits<float>::Max();
		float MinTemperature = TNumericLimits<float>::Max();
		float MaxTemperature = -TNumericLimits<float>::Max();

		for (int32 Y = -4096; Y <= 4096; Y += 64)
		{
			for (int32 X = -4096; X <= 4096; X += 64)
			{
				const float Xf = static_cast<float>(X);
				const float Yf = static_cast<float>(Y);

				const float Continent = Generator.GetContinentalness(Xf, Yf);
				MinContinent = FMath::Min(MinContinent, Continent);
				MaxContinent = FMath::Max(MaxContinent, Continent);

				const float Temperature = Generator.GetTemperature(Xf, Yf, Generator.GetSurfaceHeight(Xf, Yf));
				MinTemperature = FMath::Min(MinTemperature, Temperature);
				MaxTemperature = FMath::Max(MaxTemperature, Temperature);
			}
		}

		AddInfo(FString::Printf(TEXT("continentalness %.3f..%.3f, temperature %.3f..%.3f"),
			MinContinent, MaxContinent, MinTemperature, MaxTemperature));

		TestTrue(FString::Printf(TEXT("continentalness spans most of [0,1] (got %.2f..%.2f)"),
			MinContinent, MaxContinent),
			MinContinent < 0.15f && MaxContinent > 0.85f);

		TestTrue(FString::Printf(TEXT("temperature spans most of [0,1] (got %.2f..%.2f)"),
			MinTemperature, MaxTemperature),
			MinTemperature < 0.15f && MaxTemperature > 0.85f);
	}

	// --- every biome is actually reachable ---
	{
		TSet<int32> Seen;
		for (int32 Y = -6144; Y <= 6144; Y += 48)
		{
			for (int32 X = -6144; X <= 6144; X += 48)
			{
				Seen.Add(Generator.GetDominantBiome(static_cast<float>(X), static_cast<float>(Y)));
			}
		}

		TArray<FString> Missing;
		for (int32 Index = 0; Index < Fixture.BiomeRegistry.Num(); ++Index)
		{
			const FMadBiomeDefinitionData& Biome = Fixture.BiomeRegistry.Get(Index);

			// Weight 0 marks a template that exists only to be inherited from.
			if (Biome.Weight <= 0.0f)
			{
				continue;
			}

			if (!Seen.Contains(Index))
			{
				Missing.Add(Biome.Id.ToString());
			}
		}

		TestTrue(FString::Printf(
			TEXT("every selectable biome appears somewhere in a 12288x12288 area. Missing: %s"),
			Missing.Num() ? *FString::Join(Missing, TEXT(", ")) : TEXT("none")),
			Missing.Num() == 0);
	}

	// --- caves ---
	{
		// Generate a column of chunks and count air pockets that are fully
		// enclosed by terrain. Zero would mean the cave pass does nothing.
		int32 UndergroundAir = 0;
		int32 UndergroundSolid = 0;

		for (int32 ChunkZ = -2; ChunkZ <= 0; ++ChunkZ)
		{
			FMadChunkStorage Chunk;
			Generator.GenerateChunk(FMadChunkCoord(5, 5, ChunkZ), Chunk);

			for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
			{
				const FMadVoxel Voxel = Chunk.GetVoxel(Index);

				int32 X, Y, Z;
				MadFall::VoxelCoords(Index, X, Y, Z);
				const int32 WorldZ = ChunkZ * MadFall::ChunkSize + Z;

				if (WorldZ <= Settings.BedrockTop)
				{
					continue;
				}

				if (Voxel.Density >= 128) { ++UndergroundSolid; }
				else { ++UndergroundAir; }
			}
		}

		AddInfo(FString::Printf(TEXT("Underground sample: %d solid, %d air (%.2f%% carved)"),
			UndergroundSolid, UndergroundAir,
			100.0f * UndergroundAir / FMath::Max(UndergroundSolid + UndergroundAir, 1)));

		TestTrue(TEXT("caves carve some underground air"), UndergroundAir > 0);

		// If caves ate most of the rock, the threshold is wrong and the world
		// would be a sponge.
		TestTrue(TEXT("caves do not hollow out the world"),
			UndergroundAir < UndergroundSolid / 2);
	}

	// --- water fills to sea level, and no higher ---
	{
		FMadChunkStorage Chunk;
		Generator.GenerateChunk(FMadChunkCoord(0, 0, 0), Chunk);

		int32 WaterAboveSeaLevel = 0;
		for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
		{
			if (Chunk.GetVoxel(Index).BlockTypeID != WaterId)
			{
				continue;
			}

			int32 X, Y, Z;
			MadFall::VoxelCoords(Index, X, Y, Z);

			if (Z > Settings.SeaLevel)
			{
				++WaterAboveSeaLevel;
			}
		}

		TestEqual(TEXT("no water is generated above sea level"), WaterAboveSeaLevel, 0);
	}

	return true;
}

// ===========================================================================
// Ores
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWorldGenOreTest,
	"MadFall.WorldGen.Ores",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWorldGenOreTest::RunTest(const FString& Parameters)
{
	using namespace MadWorldGenTests;

	FGenFixture Fixture;
	if (!TestTrue(TEXT("biome definitions loaded"), Fixture.BiomeRegistry.Num() > 0))
	{
		return false;
	}

	FMadWorldGenerator Generator = Fixture.MakeGenerator(20260912u);

	const uint16 IronId = Fixture.Blocks.ResolveRuntimeId(FName(TEXT("madfall:iron_ore")));
	TestNotEqual(TEXT("iron ore is a registered block"),
		static_cast<int32>(IronId), static_cast<int32>(MadFall::BlockTypeUnresolved));

	int32 IronCount = 0;
	int32 FloatingOre = 0;
	int32 OreOutOfRange = 0;

	// The widest min_z..max_z any biome declares for iron in the shipped
	// definitions. Reading it from the definitions rather than hardcoding means
	// retuning a biome does not silently break this test.
	int32 AllowedMinZ = TNumericLimits<int32>::Max();
	int32 AllowedMaxZ = TNumericLimits<int32>::Min();

	for (int32 Index = 0; Index < Fixture.BiomeRegistry.Num(); ++Index)
	{
		for (const FMadOreDistribution& Ore : Fixture.BiomeRegistry.Get(Index).Ores)
		{
			if (Ore.Block == FName(TEXT("madfall:iron_ore")))
			{
				AllowedMinZ = FMath::Min(AllowedMinZ, Ore.MinZ);
				AllowedMaxZ = FMath::Max(AllowedMaxZ, Ore.MaxZ);
			}
		}
	}

	for (int32 ChunkY = 0; ChunkY < 4; ++ChunkY)
	{
		for (int32 ChunkX = 0; ChunkX < 4; ++ChunkX)
		{
			for (int32 ChunkZ = -2; ChunkZ <= 0; ++ChunkZ)
			{
				FMadChunkStorage Chunk;
				Generator.GenerateChunk(FMadChunkCoord(ChunkX, ChunkY, ChunkZ), Chunk);

				for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
				{
					const FMadVoxel Voxel = Chunk.GetVoxel(Index);
					if (Voxel.BlockTypeID != IronId)
					{
						continue;
					}

					++IronCount;

					int32 X, Y, Z;
					MadFall::VoxelCoords(Index, X, Y, Z);
					const int32 WorldZ = ChunkZ * MadFall::ChunkSize + Z;

					// Ore replaces stone in place, so it must be solid. Ore
					// hanging in a cave or poking out of the grass reads as a
					// bug to any player who sees it.
					if (Voxel.Density < 128)
					{
						++FloatingOre;
					}

					if (WorldZ < AllowedMinZ || WorldZ > AllowedMaxZ)
					{
						++OreOutOfRange;
					}
				}
			}
		}
	}

	AddInfo(FString::Printf(TEXT("48 chunks contained %d iron ore voxels (allowed Z %d..%d)"),
		IronCount, AllowedMinZ, AllowedMaxZ));

	TestTrue(TEXT("ore is actually generated"), IronCount > 0);
	TestEqual(TEXT("no ore is generated in open air"), FloatingOre, 0);
	TestEqual(TEXT("no ore is generated outside its declared depth range"), OreOutOfRange, 0);

	// A whole chunk of ore would mean the cluster walk is not bounded.
	TestTrue(FString::Printf(TEXT("ore is rare relative to stone (%d voxels in 48 chunks)"), IronCount),
		IronCount < 48 * MadFall::ChunkVoxelCount / 100);

	// --- the mountains carry their own iron ---------------------------------
	// The sweep above covers world Z -64..31, which is where ore has always
	// been. Since the highlands became mountains their upper slopes are a place
	// a player can reach without digging, and they have a band of their own:
	// without it the top half of every mountain was bare rock and climbing one
	// bought nothing.
	{
		// The tallest columns of a 4 km square, rather than a line through it:
		// mountains are massifs a few hundred voxels across, and a diagonal
		// sample walked straight past them.
		TArray<TPair<float, FIntPoint>> Tallest;
		for (int32 Y = -2000; Y <= 2000; Y += 64)
		{
			for (int32 X = -2000; X <= 2000; X += 64)
			{
				Tallest.Emplace(Generator.GetSurfaceHeight(static_cast<float>(X), static_cast<float>(Y)), FIntPoint(X, Y));
			}
		}
		Tallest.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key > B.Key; });

		int32 ColumnsChecked = 0;
		int32 ColumnsWithIron = 0;
		int32 HighIron = 0;
		for (int32 Index = 0; Index < Tallest.Num() && ColumnsChecked < 6; ++Index)
		{
			const float Height = Tallest[Index].Key;
			if (Height < 85.0f)
			{
				break;
			}

			++ColumnsChecked;
			const FMadChunkCoord Coord(
				FMath::FloorToInt32(static_cast<float>(Tallest[Index].Value.X) / MadFall::ChunkSize),
				FMath::FloorToInt32(static_cast<float>(Tallest[Index].Value.Y) / MadFall::ChunkSize),
				FMath::FloorToInt32(Height / MadFall::ChunkSize));

			FMadChunkStorage Chunk;
			Generator.GenerateChunk(Coord, Chunk);

			int32 InThisChunk = 0;
			for (int32 Voxel = 0; Voxel < MadFall::ChunkVoxelCount; ++Voxel)
			{
				if (Chunk.GetVoxel(Voxel).BlockTypeID == IronId)
				{
					++InThisChunk;
				}
			}
			HighIron += InThisChunk;
			ColumnsWithIron += InThisChunk > 0 ? 1 : 0;
		}

		AddInfo(FString::Printf(TEXT("%d of %d summit chunks held iron (%d voxels); tallest column %.0f"),
			ColumnsWithIron, ColumnsChecked, HighIron, Tallest.Num() > 0 ? Tallest[0].Key : 0.0f));
		TestTrue(TEXT("the test found mountains to look at"), ColumnsChecked >= 3);
		TestTrue(TEXT("their chunks carry iron, so a climb is worth something"), ColumnsWithIron >= ColumnsChecked / 2);
	}

	return true;
}

// ===========================================================================
// Performance
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWorldGenPerformanceTest,
	"MadFall.WorldGen.Performance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWorldGenPerformanceTest::RunTest(const FString& Parameters)
{
	using namespace MadWorldGenTests;

	FGenFixture Fixture;
	if (!TestTrue(TEXT("biome definitions loaded"), Fixture.BiomeRegistry.Num() > 0))
	{
		return false;
	}

	FMadWorldGenerator Generator = Fixture.MakeGenerator(20260912u);

	// Warm up so first-call allocations are not counted.
	FMadChunkStorage Warmup;
	Generator.GenerateChunk(FMadChunkCoord(0, 0, 0), Warmup);

	constexpr int32 Iterations = 16;
	const double Start = FPlatformTime::Seconds();

	for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
	{
		FMadChunkStorage Chunk;
		Generator.GenerateChunk(FMadChunkCoord(Iteration, Iteration * 3, 0), Chunk);
	}

	const double MeanMs = (FPlatformTime::Seconds() - Start) * 1000.0 / Iterations;

	AddInfo(FString::Printf(TEXT("Chunk generation: %.2f ms mean over %d chunks (worker thread)."),
		MeanMs, Iterations));

	// Generation runs on a worker thread, so this is a throughput bound rather
	// than a frame budget. At 50 ms a chunk a player flying at any speed would
	// out-run the generator no matter how many cores are thrown at it. Loose on
	// purpose: this catches an algorithmic regression, not microseconds.
	TestTrue(FString::Printf(TEXT("a chunk generates in under 50 ms (took %.2f ms)"), MeanMs),
		MeanMs < 50.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

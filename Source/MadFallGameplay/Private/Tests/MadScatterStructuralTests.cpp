// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadChunkStorage.h"
#include "MadFallCoordinates.h"
#include "MadStructuralSolver.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadScatterStructuralTests
{
	/** Generated chunks as a structural world; anything outside them is unloaded. */
	class FChunkWorld final : public IMadStructuralWorld
	{
	public:
		TMap<FMadChunkCoord, FMadChunkStorage> Chunks;

		virtual bool IsLoaded(const FIntVector& Position) const override
		{
			return Chunks.Contains(MadFall::WorldToChunk(Position.X, Position.Y, Position.Z));
		}

		virtual FMadVoxel GetVoxel(const FIntVector& Position) const override
		{
			const FMadChunkStorage* Storage = Chunks.Find(MadFall::WorldToChunk(Position.X, Position.Y, Position.Z));
			if (Storage == nullptr)
			{
				return FMadVoxel::Air();
			}
			int32 LX, LY, LZ;
			MadFall::WorldToLocal(Position.X, Position.Y, Position.Z, LX, LY, LZ);
			return Storage->GetVoxel(LX, LY, LZ);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadGeneratedScatterStandsTest,
	"MadFall.Structural.GeneratedScatterStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadGeneratedScatterStandsTest::RunTest(const FString& Parameters)
{
	using namespace MadScatterStructuralTests;

	// The real content, the real generator and the real solver: every tree,
	// cactus and bush a biome scatters must survive the structural check a
	// chunk gets when it loads. Anything that fails falls out of the sky the
	// first time a player walks into that forest.
	const FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const FMadBiomeRegistry& Biomes = UMadVoxelWorldSubsystem::GetBiomeRegistry();
	FMadStructuralMaterials Materials;
	Materials.Build(Registry);

	const TCHAR* BiomeIds[] = { TEXT("madfall:forest"), TEXT("madfall:plains"), TEXT("madfall:tundra"), TEXT("madfall:desert"), TEXT("madfall:highlands") };
	const uint32 Seeds[] = { 0u, 1337u, 20260913u };

	for (uint32 Seed : Seeds)
	{
		FMadWorldGenSettings Settings;
		Settings.Seed = Seed;
		FMadWorldGenerator Generator(Settings, Biomes, const_cast<FMadBlockRegistry&>(Registry), &UMadVoxelWorldSubsystem::GetPrefabRegistry());

		for (const TCHAR* BiomeId : BiomeIds)
		{
			const int32 BiomeIndex = Biomes.FindIndex(FName(BiomeId));
			FIntPoint Column;
			if (BiomeIndex == INDEX_NONE || !Generator.FindBiomeNear(BiomeIndex, FIntPoint::ZeroValue, 8000, Column))
			{
				AddWarning(FString::Printf(TEXT("seed %u: no %s found near the origin; skipped"), Seed, BiomeId));
				continue;
			}

			const int32 SurfaceZ = FMath::FloorToInt(Generator.GetSurfaceHeight(static_cast<float>(Column.X), static_cast<float>(Column.Y)));
			const FMadChunkCoord Centre = MadFall::WorldToChunk(Column.X, Column.Y, SurfaceZ);

			FChunkWorld World;
			for (int32 DZ = -1; DZ <= 1; ++DZ)
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const FMadChunkCoord Coord(Centre.X + DX, Centre.Y + DY, Centre.Z + DZ);
						Generator.GenerateChunk(Coord, World.Chunks.Add(Coord));
					}
				}
			}

			// Load seeding: every cubic voxel in the centre chunk.
			TArray<FIntVector> Seeds3D;
			for (int32 Z = 0; Z < MadFall::ChunkSize; ++Z)
			{
				for (int32 Y = 0; Y < MadFall::ChunkSize; ++Y)
				{
					for (int32 X = 0; X < MadFall::ChunkSize; ++X)
					{
						const FMadVoxel Voxel = World.Chunks[Centre].GetVoxel(X, Y, Z);
						if (Voxel.IsSolid() && Voxel.HasFlag(EMadVoxelFlags::Cubic))
						{
							int32 WX, WY, WZ;
							MadFall::ChunkToWorld(Centre, X, Y, Z, WX, WY, WZ);
							Seeds3D.Add(FIntVector(WX, WY, WZ));
						}
					}
				}
			}

			FMadStructuralJob Job(Seeds3D, FMadStructuralSettings());
			Job.RunToCompletion(World, Materials);
			const TArray<FMadStructuralFailureRecord>& Failures = Job.GetFailures();

			TestEqual(FString::Printf(TEXT("seed %u, %s at %d,%d: generated scatter stands"), Seed, BiomeId, Column.X, Column.Y), Failures.Num(), 0);
			for (int32 Index = 0; Index < FMath::Min(Failures.Num(), 6); ++Index)
			{
				const FMadStructuralFailureRecord& F = Failures[Index];
				FMadStructuralNodeReport Report;
				Job.GetNodeReport(F.Position, Report);
				const FMadVoxel Below = World.GetVoxel(F.Position - FIntVector(0, 0, 1));
				AddError(FString::Printf(TEXT("  %s at %s: %s, %s, carrying %.0f of %.0f kg; below is %s (density %d, %s)"),
					*Registry.GetStringId(F.Voxel.BlockTypeID).ToString(), *F.Position.ToString(),
					F.Reason == EMadStructuralFailure::Overloaded ? TEXT("overloaded") : TEXT("unsupported"),
					Report.SupportDistance == MadFall::Structural::Unreachable ? TEXT("no path")
						: *FString::Printf(TEXT("%.2f span"), static_cast<double>(Report.SupportDistance) / MadFall::Structural::SupportScale),
					Report.CarriedKg, Report.CapacityKg,
					*Registry.GetStringId(Below.BlockTypeID).ToString(), Below.Density,
					Below.HasFlag(EMadVoxelFlags::Cubic) ? TEXT("cubic") : TEXT("terrain")));
				AddError(FString::Printf(TEXT("    neighbours: %s"), *Job.DescribeNeighbours(F.Position)));
			}
			AddInfo(FString::Printf(TEXT("seed %u, %s: %d members, %d failures."), Seed, BiomeId, Job.NumMembers(), Failures.Num()));
		}
	}

	return true;
}

#endif

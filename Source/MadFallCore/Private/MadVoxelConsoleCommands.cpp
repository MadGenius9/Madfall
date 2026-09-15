// Copyright MadFall. All Rights Reserved.

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadPrefabRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MadFallCore.h"
#include "MadVoxelWorldSubsystem.h"
#include "Misc/DefaultValueHelper.h"

/**
 * Console commands for the voxel world.
 *
 * These are the Phase 1 deliverable's user interface: load a chunk, edit
 * voxels, save, reload, and confirm what came back. They are real commands
 * against the real subsystem, not a debug shim - the same SetVoxel funnel the
 * game will use.
 *
 * All coordinates are world VOXEL coordinates unless a command says "chunk".
 */
namespace
{
	UMadVoxelWorldSubsystem* GetSubsystem(UWorld* World)
	{
		if (World == nullptr)
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("No world; run this from a running game or PIE session."));
			return nullptr;
		}

		UMadVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UMadVoxelWorldSubsystem>();
		if (Subsystem == nullptr)
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("The voxel world subsystem is not available in this world."));
		}

		return Subsystem;
	}

	bool ParseInts(const TArray<FString>& Args, int32 First, int32 Count, int32* Out, const TCHAR* Usage)
	{
		if (Args.Num() < First + Count)
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Usage: %s"), Usage);
			return false;
		}

		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (!FDefaultValueHelper::ParseInt(Args[First + Index], Out[Index]))
			{
				UE_LOG(LogMadFallVoxel, Error, TEXT("'%s' is not an integer. Usage: %s"),
					*Args[First + Index], Usage);
				return false;
			}
		}

		return true;
	}

	void LogMultiline(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("%s"), *Line);
		}
	}
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

static FAutoConsoleCommand GMadBlocksCommand(
	TEXT("mad.blocks"),
	TEXT("Lists every registered block definition and its runtime id."),
	FConsoleCommandDelegate::CreateStatic([]
	{
		LogMultiline(UMadVoxelWorldSubsystem::GetBlockRegistry().DescribeContents());
	}));

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------

static FAutoConsoleCommandWithWorld GMadWorldInfoCommand(
	TEXT("mad.world.info"),
	TEXT("Prints the world directory, loaded chunk count and voxel memory use."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World))
		{
			LogMultiline(Subsystem->DescribeWorld());
		}
	}));

static FAutoConsoleCommandWithWorld GMadWorldSaveCommand(
	TEXT("mad.world.save"),
	TEXT("Saves every dirty chunk and flushes all open region files."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr)
		{
			return;
		}

		FString Error;
		if (Subsystem->SaveAll(Error))
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("World saved."));
		}
		else
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Save failed: %s"), *Error);
		}
	}));

// ---------------------------------------------------------------------------
// Chunks
// ---------------------------------------------------------------------------

static FAutoConsoleCommandWithWorldAndArgs GMadWorldAutosaveCommand(
	TEXT("mad.world.autosave"),
	TEXT("mad.world.autosave [wait] - the autosave's world write: copies dirty chunks, writes and disk-flushes them on a worker. 'wait' blocks until done."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
		if (Subsystem == nullptr)
		{
			return;
		}
		const int32 Copied = Subsystem->SaveAllAsync();
		if (Args.Num() > 0 && Args[0].Equals(TEXT("wait"), ESearchCase::IgnoreCase))
		{
			Subsystem->WaitForSaves();
		}
		UE_LOG(LogMadFallVoxel, Display, TEXT("Autosave started for %d chunk(s)%s."), Copied, Args.Num() > 0 ? TEXT(" and finished") : TEXT(""));
	}));

#if !UE_BUILD_SHIPPING
static FAutoConsoleCommand GMadWorldCrashCommand(
	TEXT("mad.world.crash"),
	TEXT("Terminates the process at once - no shutdown save, no region close - to test what a crash or power cut leaves on disk."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UE_LOG(LogMadFallVoxel, Display, TEXT("Terminating without saving (mad.world.crash)."));
		FPlatformMisc::RequestExitWithStatus(/*Force*/ true, 0);
	}));
#endif

static FAutoConsoleCommandWithWorldAndArgs GMadChunkLoadCommand(
	TEXT("mad.chunk.load"),
	TEXT("mad.chunk.load <chunkX> <chunkY> <chunkZ> - loads a chunk from disk (or creates it empty)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[3];
		if (!ParseInts(Args, 0, 3, Coords, TEXT("mad.chunk.load <chunkX> <chunkY> <chunkZ>"))) { return; }

		const FMadChunkCoord Coord(Coords[0], Coords[1], Coords[2]);

		FString Error;
		if (Subsystem->LoadChunkSync(Coord, Error))
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("Loaded chunk %s. %d chunk(s) resident."),
				*Coord.ToString(), Subsystem->NumLoadedChunks());
		}
		else
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Could not load chunk %s: %s"), *Coord.ToString(), *Error);
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadChunkUnloadCommand(
	TEXT("mad.chunk.unload"),
	TEXT("mad.chunk.unload <chunkX> <chunkY> <chunkZ> [nosave] - saves (unless nosave) and drops a chunk."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[3];
		if (!ParseInts(Args, 0, 3, Coords, TEXT("mad.chunk.unload <chunkX> <chunkY> <chunkZ> [nosave]"))) { return; }

		const bool bSave = !(Args.Num() > 3 && Args[3].Equals(TEXT("nosave"), ESearchCase::IgnoreCase));
		const FMadChunkCoord Coord(Coords[0], Coords[1], Coords[2]);

		FString Error;
		if (Subsystem->UnloadChunk(Coord, bSave, Error))
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("Unloaded chunk %s%s."),
				*Coord.ToString(), bSave ? TEXT(" (saved)") : TEXT(" (discarded)"));
		}
		else
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Could not unload chunk %s: %s"), *Coord.ToString(), *Error);
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadChunkInfoCommand(
	TEXT("mad.chunk.info"),
	TEXT("mad.chunk.info <chunkX> <chunkY> <chunkZ> - palette, memory use and dirty state."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[3];
		if (!ParseInts(Args, 0, 3, Coords, TEXT("mad.chunk.info <chunkX> <chunkY> <chunkZ>"))) { return; }

		LogMultiline(Subsystem->DescribeChunk(FMadChunkCoord(Coords[0], Coords[1], Coords[2])));
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadChunkFillCommand(
	TEXT("mad.chunk.fill"),
	TEXT("mad.chunk.fill <chunkX> <chunkY> <chunkZ> <blockId> [density] - fills a loaded chunk uniformly."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[3];
		if (!ParseInts(Args, 0, 3, Coords, TEXT("mad.chunk.fill <chunkX> <chunkY> <chunkZ> <blockId> [density]"))) { return; }

		if (Args.Num() < 4)
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Usage: mad.chunk.fill <chunkX> <chunkY> <chunkZ> <blockId> [density]"));
			return;
		}

		const FName BlockId(*Args[3]);
		FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();

		if (!Registry.IsRegistered(BlockId))
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("'%s' is not a registered block. Run mad.blocks to list them."),
				*BlockId.ToString());
			return;
		}

		int32 Density = (BlockId == FName(TEXT("madfall:air"))) ? 0 : 255;
		if (Args.Num() > 4)
		{
			FDefaultValueHelper::ParseInt(Args[4], Density);
		}

		FMadVoxel Voxel;
		Voxel.BlockTypeID = Registry.ResolveRuntimeId(BlockId);
		Voxel.Density = static_cast<uint8>(FMath::Clamp(Density, 0, 255));
		Voxel.Damage = 0;
		Voxel.Rotation = 0;
		Voxel.Flags = static_cast<uint8>(EMadVoxelFlags::PlayerModified);

		const FMadChunkCoord Coord(Coords[0], Coords[1], Coords[2]);
		if (Subsystem->FillChunk(Coord, Voxel))
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("Filled chunk %s with %s (density %d)."),
				*Coord.ToString(), *BlockId.ToString(), Voxel.Density);
		}
		else
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Chunk %s is not loaded."), *Coord.ToString());
		}
	}));

// ---------------------------------------------------------------------------
// Voxels
// ---------------------------------------------------------------------------

static FAutoConsoleCommandWithWorldAndArgs GMadVoxelGetCommand(
	TEXT("mad.voxel.get"),
	TEXT("mad.voxel.get <x> <y> <z> - reads one voxel at world voxel coordinates."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[3];
		if (!ParseInts(Args, 0, 3, Coords, TEXT("mad.voxel.get <x> <y> <z>"))) { return; }

		const FMadChunkCoord ChunkCoord = MadFall::WorldToChunk(Coords[0], Coords[1], Coords[2]);
		if (!Subsystem->FindChunk(ChunkCoord).IsValid())
		{
			UE_LOG(LogMadFallVoxel, Warning,
				TEXT("Chunk %s is not loaded; run mad.chunk.load %d %d %d first."),
				*ChunkCoord.ToString(), ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
			return;
		}

		const FMadVoxel Voxel = Subsystem->GetVoxel(Coords[0], Coords[1], Coords[2]);
		const FName BlockId = UMadVoxelWorldSubsystem::GetBlockRegistry().GetStringId(Voxel.BlockTypeID);

		UE_LOG(LogMadFallVoxel, Display,
			TEXT("(%d, %d, %d) = %s  density=%u damage=%u orientation=%u variant=%u flags=0x%02X solid=%s"),
			Coords[0], Coords[1], Coords[2],
			*BlockId.ToString(), Voxel.Density, Voxel.Damage,
			Voxel.GetOrientation(), Voxel.GetShapeVariant(), Voxel.Flags,
			Voxel.IsSolid() ? TEXT("yes") : TEXT("no"));
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadVoxelSetCommand(
	TEXT("mad.voxel.set"),
	TEXT("mad.voxel.set <x> <y> <z> <blockId> [density] [orientation] [variant] - writes one voxel."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[3];
		if (!ParseInts(Args, 0, 3, Coords,
			TEXT("mad.voxel.set <x> <y> <z> <blockId> [density] [orientation] [variant]"))) { return; }

		if (Args.Num() < 4)
		{
			UE_LOG(LogMadFallVoxel, Error,
				TEXT("Usage: mad.voxel.set <x> <y> <z> <blockId> [density] [orientation] [variant]"));
			return;
		}

		const FName BlockId(*Args[3]);
		FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();

		if (!Registry.IsRegistered(BlockId))
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("'%s' is not a registered block. Run mad.blocks to list them."),
				*BlockId.ToString());
			return;
		}

		const bool bAir = (BlockId == FName(TEXT("madfall:air")));

		int32 Density = bAir ? 0 : 255;
		int32 Orientation = 0;
		int32 Variant = 0;

		if (Args.Num() > 4) { FDefaultValueHelper::ParseInt(Args[4], Density); }
		if (Args.Num() > 5) { FDefaultValueHelper::ParseInt(Args[5], Orientation); }
		if (Args.Num() > 6) { FDefaultValueHelper::ParseInt(Args[6], Variant); }

		FMadVoxel Voxel;
		Voxel.BlockTypeID = Registry.ResolveRuntimeId(BlockId);
		Voxel.Density = static_cast<uint8>(FMath::Clamp(Density, 0, 255));
		Voxel.Damage = 0;
		Voxel.Rotation = 0;
		Voxel.SetOrientation(static_cast<uint8>(FMath::Clamp(Orientation, 0, 23)));
		Voxel.SetShapeVariant(static_cast<uint8>(FMath::Clamp(Variant, 0, 7)));
		Voxel.Flags = 0;

		// Anything placed by hand is player construction, which is what routes
		// it to the cubic mesher rather than the isosurface path in Phase 2 -
		// except liquid, which is flagged as generated water is. Without the flag
		// the structural solver judged a console-placed water block an
		// unsupported member and dropped it the instant it was set.
		const FMadBlockDefinitionData* Def = Registry.FindDefinition(Voxel.BlockTypeID);
		const bool bLiquid = Def != nullptr && Def->bLiquid;
		Voxel.SetFlag(EMadVoxelFlags::Liquid, bLiquid);
		Voxel.SetFlag(EMadVoxelFlags::Cubic, !bAir && !bLiquid);

		if (Subsystem->SetVoxel(Coords[0], Coords[1], Coords[2], Voxel))
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("Set (%d, %d, %d) to %s."),
				Coords[0], Coords[1], Coords[2], *BlockId.ToString());
		}
		else
		{
			const FMadChunkCoord ChunkCoord = MadFall::WorldToChunk(Coords[0], Coords[1], Coords[2]);
			UE_LOG(LogMadFallVoxel, Error,
				TEXT("Could not write (%d, %d, %d). Chunk %s is not loaded, or Z is outside %d..%d."),
				Coords[0], Coords[1], Coords[2], *ChunkCoord.ToString(),
				MadFall::WorldMinZ, MadFall::WorldMaxZ);
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadWorldLoadAreaCommand(
	TEXT("mad.world.loadarea"),
	TEXT("mad.world.loadarea <chunkX> <chunkY> <chunkZ> <radius> [verticalRadius] - loads/generates a cube of chunks."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[4];
		if (!ParseInts(Args, 0, 4, Coords,
			TEXT("mad.world.loadarea <chunkX> <chunkY> <chunkZ> <radius> [verticalRadius]"))) { return; }

		int32 VerticalRadius = 0;
		if (Args.Num() > 4) { FDefaultValueHelper::ParseInt(Args[4], VerticalRadius); }

		FString Error;
		const int32 Loaded = Subsystem->LoadArea(
			FMadChunkCoord(Coords[0], Coords[1], Coords[2]),
			FMath::Clamp(Coords[3], 0, 16), FMath::Clamp(VerticalRadius, 0, 8), Error);

		UE_LOG(LogMadFallVoxel, Display, TEXT("Loaded %d chunk(s). %d resident, %.2f MiB of voxels."),
			Loaded, Subsystem->NumLoadedChunks(), Subsystem->GetLoadedVoxelBytes() / (1024.0 * 1024.0));
	}));

// ---------------------------------------------------------------------------
// World generation
// ---------------------------------------------------------------------------

static FAutoConsoleCommand GMadBiomesCommand(
	TEXT("mad.biomes"),
	TEXT("Lists every registered biome with its climate ranges and terrain shape."),
	FConsoleCommandDelegate::CreateStatic([]
	{
		LogMultiline(UMadVoxelWorldSubsystem::GetBiomeRegistry().DescribeContents());
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadWorldSeedCommand(
	TEXT("mad.world.seed"),
	TEXT("mad.world.seed [newSeed] - prints the seed, or sets it and rebuilds the generator."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		if (Args.Num() > 0)
		{
			int32 NewSeed = 0;
			if (!FDefaultValueHelper::ParseInt(Args[0], NewSeed))
			{
				UE_LOG(LogMadFallVoxel, Error, TEXT("'%s' is not an integer."), *Args[0]);
				return;
			}

			if (Subsystem->NumLoadedChunks() > 0)
			{
				// Changing the seed under loaded chunks would leave terrain from
				// two different worlds touching each other.
				UE_LOG(LogMadFallVoxel, Warning,
					TEXT("%d chunk(s) are loaded. Unload them before changing the seed, or the world will have a seam."),
					Subsystem->NumLoadedChunks());
			}

			Subsystem->SetSeed(NewSeed);
		}

		UE_LOG(LogMadFallVoxel, Display, TEXT("World seed: %lld  (worldgen version 0x%llX)"),
			Subsystem->GetSeed(), Subsystem->GetWorldGenSettings().GetGenerationVersion());
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadWorldGenProbeCommand(
	TEXT("mad.worldgen.probe"),
	TEXT("mad.worldgen.probe <worldX> <worldY> - climate, height and biome blend for one column."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[2];
		if (!ParseInts(Args, 0, 2, Coords, TEXT("mad.worldgen.probe <worldX> <worldY>"))) { return; }

		const FMadWorldGenerator* Generator = Subsystem->GetWorldGenerator();
		if (Generator == nullptr)
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("The world generator is not built."));
			return;
		}

		LogMultiline(Generator->ProbeColumn(Coords[0], Coords[1]));
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadWorldGenSurveyCommand(
	TEXT("mad.worldgen.survey"),
	TEXT("mad.worldgen.survey [radiusInChunks] [step] - biome and height distribution over an area."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		// Answers the question a single probe cannot: is the biome mix sane, or
		// does one biome cover 95% of the world because its climate ranges are
		// wider than everything else's?
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		const FMadWorldGenerator* Generator = Subsystem->GetWorldGenerator();
		if (Generator == nullptr)
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("The world generator is not built."));
			return;
		}

		int32 RadiusChunks = 32;
		int32 Step = 8;
		if (Args.Num() > 0) { FDefaultValueHelper::ParseInt(Args[0], RadiusChunks); }
		if (Args.Num() > 1) { FDefaultValueHelper::ParseInt(Args[1], Step); }

		RadiusChunks = FMath::Clamp(RadiusChunks, 1, 512);
		Step = FMath::Clamp(Step, 1, 64);

		const int32 Extent = RadiusChunks * MadFall::ChunkSize;
		const FMadBiomeRegistry& Biomes = UMadVoxelWorldSubsystem::GetBiomeRegistry();

		TArray<int32> Counts;
		Counts.SetNumZeroed(Biomes.Num());

		float MinHeight = TNumericLimits<float>::Max();
		float MaxHeight = -TNumericLimits<float>::Max();
		double HeightSum = 0.0;
		int32 Samples = 0;
		int32 UnderwaterSamples = 0;

		// A biome that never triggers is invisible in a biome histogram - it
		// just is not listed - and the reason is almost always that its climate
		// range sits where the noise never goes. Reporting the FIELD
		// distribution alongside turns "highlands is missing" into "nothing ever
		// reaches continentalness 0.78".
		struct FFieldStats
		{
			float Min = TNumericLimits<float>::Max();
			float Max = -TNumericLimits<float>::Max();
			double Sum = 0.0;
			int32 Buckets[10] = {};

			void Add(float Value)
			{
				Min = FMath::Min(Min, Value);
				Max = FMath::Max(Max, Value);
				Sum += Value;
				++Buckets[FMath::Clamp(static_cast<int32>(Value * 10.0f), 0, 9)];
			}
		};

		FFieldStats Continent;
		FFieldStats Temperature;
		FFieldStats Moisture;

		const int32 SeaLevel = Subsystem->GetWorldGenSettings().SeaLevel;

		for (int32 Y = -Extent; Y <= Extent; Y += Step)
		{
			for (int32 X = -Extent; X <= Extent; X += Step)
			{
				const float Xf = static_cast<float>(X);
				const float Yf = static_cast<float>(Y);

				const int32 BiomeIndex = Generator->GetDominantBiome(Xf, Yf);
				if (Counts.IsValidIndex(BiomeIndex))
				{
					++Counts[BiomeIndex];
				}

				const float Height = Generator->GetSurfaceHeight(Xf, Yf);
				MinHeight = FMath::Min(MinHeight, Height);
				MaxHeight = FMath::Max(MaxHeight, Height);
				HeightSum += Height;
				++Samples;

				Continent.Add(Generator->GetContinentalness(Xf, Yf));
				Temperature.Add(Generator->GetTemperature(Xf, Yf, Height));
				Moisture.Add(Generator->GetMoisture(Xf, Yf));

				if (Height < static_cast<float>(SeaLevel))
				{
					++UnderwaterSamples;
				}
			}
		}

		auto LogField = [Samples](const TCHAR* Name, const FFieldStats& Stats)
		{
			FString Histogram;
			for (int32 Bucket = 0; Bucket < 10; ++Bucket)
			{
				Histogram += FString::Printf(TEXT("%4.1f "), 100.0f * Stats.Buckets[Bucket] / FMath::Max(Samples, 1));
			}

			UE_LOG(LogMadFallVoxel, Display, TEXT("  %-16s min %.3f mean %.3f max %.3f | %s"),
				Name, Stats.Min, Stats.Sum / FMath::Max(Samples, 1), Stats.Max, *Histogram);
		};

		UE_LOG(LogMadFallVoxel, Display,
			TEXT("Survey of %d x %d voxels (%d samples, step %d), seed %lld:"),
			Extent * 2, Extent * 2, Samples, Step, Subsystem->GetSeed());
		UE_LOG(LogMadFallVoxel, Display,
			TEXT("  height: min %.1f  mean %.1f  max %.1f  (sea level %d)"),
			MinHeight, HeightSum / FMath::Max(Samples, 1), MaxHeight, SeaLevel);
		UE_LOG(LogMadFallVoxel, Display,
			TEXT("  below sea level: %.1f%%"), 100.0f * UnderwaterSamples / FMath::Max(Samples, 1));

		UE_LOG(LogMadFallVoxel, Display, TEXT("  climate fields (10 buckets, %% of samples in each decile):"));
		LogField(TEXT("continentalness"), Continent);
		LogField(TEXT("temperature"), Temperature);
		LogField(TEXT("moisture"), Moisture);

		UE_LOG(LogMadFallVoxel, Display, TEXT("  dominant biome:"));

		for (int32 Index = 0; Index < Counts.Num(); ++Index)
		{
			if (Counts[Index] > 0)
			{
				UE_LOG(LogMadFallVoxel, Display, TEXT("  %-28s %5.1f%%"),
					*Biomes.Get(Index).Id.ToString(), 100.0f * Counts[Index] / FMath::Max(Samples, 1));
			}
		}
	}));

// ---------------------------------------------------------------------------
// Points of interest
// ---------------------------------------------------------------------------

static FAutoConsoleCommand GMadPrefabsCommand(
	TEXT("mad.prefabs"),
	TEXT("Lists every loaded prefab with its tier, size, markers and placement weight."),
	FConsoleCommandDelegate::CreateStatic([]
	{
		LogMultiline(UMadVoxelWorldSubsystem::GetPrefabRegistry().DescribeContents());
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadPoiCellCommand(
	TEXT("mad.poi.cell"),
	TEXT("mad.poi.cell <worldX> <worldY> - the POI planned for the cell containing a world column."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[2];
		if (!ParseInts(Args, 0, 2, Coords, TEXT("mad.poi.cell <worldX> <worldY>"))) { return; }

		const FMadWorldGenerator* Generator = Subsystem->GetWorldGenerator();
		if (Generator == nullptr) { return; }

		const FMadPoiPlanner& Planner = Generator->GetPoiPlanner();
		const FIntPoint Cell = Planner.ChunkToCell(MadFall::WorldToChunk(Coords[0], Coords[1], 0));

		LogMultiline(Planner.DescribeCell(*Generator, Cell.X, Cell.Y));
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadPoiNearCommand(
	TEXT("mad.poi.near"),
	TEXT("mad.poi.near <worldX> <worldY> [cellRadius] - every POI within a radius of cells, nearest first."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[2];
		if (!ParseInts(Args, 0, 2, Coords, TEXT("mad.poi.near <worldX> <worldY> [cellRadius]"))) { return; }

		int32 Radius = 2;
		if (Args.Num() > 2) { FDefaultValueHelper::ParseInt(Args[2], Radius); }
		Radius = FMath::Clamp(Radius, 0, 16);

		const FMadWorldGenerator* Generator = Subsystem->GetWorldGenerator();
		if (Generator == nullptr) { return; }

		const FMadPoiPlanner& Planner = Generator->GetPoiPlanner();
		const FIntPoint Centre = Planner.ChunkToCell(MadFall::WorldToChunk(Coords[0], Coords[1], 0));

		struct FFound { FMadPoiInstance Poi; float Distance; };
		TArray<FFound> Found;
		int32 Cells = 0;

		for (int32 CellY = Centre.Y - Radius; CellY <= Centre.Y + Radius; ++CellY)
		{
			for (int32 CellX = Centre.X - Radius; CellX <= Centre.X + Radius; ++CellX)
			{
				++Cells;
				FMadPoiInstance Poi;
				if (Planner.PlanCell(*Generator, CellX, CellY, Poi))
				{
					const FIntVector Entrance = Planner.GetWorldEntrance(Poi);
					const float Distance = FVector2f(
						static_cast<float>(Entrance.X - Coords[0]),
						static_cast<float>(Entrance.Y - Coords[1])).Size();
					Found.Add({ Poi, Distance });
				}
			}
		}

		Found.Sort([](const FFound& A, const FFound& B) { return A.Distance < B.Distance; });

		UE_LOG(LogMadFallVoxel, Display, TEXT("%d POI(s) in %d cells around (%d, %d):"),
			Found.Num(), Cells, Coords[0], Coords[1]);

		for (const FFound& Item : Found)
		{
			const FMadPrefab* Prefab = Planner.GetPrefab(Item.Poi);
			const FIntVector Entrance = Planner.GetWorldEntrance(Item.Poi);
			UE_LOG(LogMadFallVoxel, Display,
				TEXT("  %6.0f voxels  %-24s tier %d (cell tier %d)  entrance (%d, %d, %d)  yaw %d  biome %s"),
				Item.Distance, *Prefab->Id.ToString(), Prefab->Tier, Item.Poi.CellTier,
				Entrance.X, Entrance.Y, Entrance.Z, Item.Poi.Yaw * 90, *Item.Poi.BiomeId.ToString());
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadPrefabCaptureCommand(
	TEXT("mad.prefab.capture"),
	TEXT("mad.prefab.capture <x0> <y0> <z0> <x1> <y1> <z1> <id> [tier] - saves a box of loaded voxels as a prefab."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		// The authoring path. Build the thing in the world with the same tools a
		// player uses, then capture it. The output is madfall.prefab/1 JSON in
		// Saved/Prefabs, ready to review in a diff and move into Definitions/ or a
		// mod folder. Air inside the box is captured as air, which carves terrain
		// on placement; use a prefab editor pass to mark padding as void.
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Box[6];
		if (!ParseInts(Args, 0, 6, Box,
			TEXT("mad.prefab.capture <x0> <y0> <z0> <x1> <y1> <z1> <id> [tier]"))) { return; }

		if (Args.Num() < 7)
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Usage: mad.prefab.capture <x0> <y0> <z0> <x1> <y1> <z1> <id> [tier]"));
			return;
		}

		const FIntVector Min(FMath::Min(Box[0], Box[3]), FMath::Min(Box[1], Box[4]), FMath::Min(Box[2], Box[5]));
		const FIntVector Max(FMath::Max(Box[0], Box[3]), FMath::Max(Box[1], Box[4]), FMath::Max(Box[2], Box[5]));

		FMadPrefab Prefab;
		Prefab.Id = FName(*Args[6]);
		Prefab.DisplayName = Args[6];
		Prefab.Size = Max - Min + FIntVector(1, 1, 1);
		Prefab.Tier = 1;
		if (Args.Num() > 7) { FDefaultValueHelper::ParseInt(Args[7], Prefab.Tier); }
		Prefab.Tier = FMath::Clamp(Prefab.Tier, 1, 5);

		FString Reason;
		if (!MadFall::BlockDefinitionJson::IsValidBlockId(Prefab.Id, Reason))
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Invalid prefab id: %s"), *Reason);
			return;
		}

		if (Prefab.Size.X > MadFall::PrefabMaxFootprint || Prefab.Size.Y > MadFall::PrefabMaxFootprint
			|| Prefab.Size.Z > MadFall::PrefabMaxHeight)
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Box is %dx%dx%d; prefabs are limited to %dx%dx%d."),
				Prefab.Size.X, Prefab.Size.Y, Prefab.Size.Z,
				MadFall::PrefabMaxFootprint, MadFall::PrefabMaxFootprint, MadFall::PrefabMaxHeight);
			return;
		}

		FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();

		// Palette keyed by the full block state, so two blocks of the same type in
		// different orientations stay distinct.
		TMap<uint64, uint16> PaletteLookup;
		int32 UnloadedVoxels = 0;

		Prefab.Voxels.Reserve(Prefab.Size.X * Prefab.Size.Y * Prefab.Size.Z);

		for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				for (int32 X = Min.X; X <= Max.X; ++X)
				{
					if (!Subsystem->FindChunk(MadFall::WorldToChunk(X, Y, Z)).IsValid())
					{
						++UnloadedVoxels;
					}

					const FMadVoxel Voxel = Subsystem->GetVoxel(X, Y, Z);
					const bool bSolid = Voxel.IsSolid();

					FMadPrefabPaletteEntry Entry;
					if (!bSolid)
					{
						Entry.Block = FName(TEXT("madfall:air"));
						Entry.Density = 0;
						Entry.bCubic = false;
					}
					else
					{
						Entry.Block = Registry.GetStringId(Voxel.BlockTypeID);
						Entry.Orientation = Voxel.GetOrientation();
						Entry.Variant = Voxel.GetShapeVariant();
						Entry.Density = Voxel.Density;
						Entry.bCubic = Voxel.HasFlag(EMadVoxelFlags::Cubic);
					}

					const uint64 Key = bSolid
						? (static_cast<uint64>(Voxel.BlockTypeID) << 24) | (static_cast<uint64>(Voxel.Rotation) << 16)
							| (static_cast<uint64>(Voxel.Density) << 8) | (Entry.bCubic ? 1u : 0u)
						: MAX_uint64;

					uint16* Found = PaletteLookup.Find(Key);
					if (Found == nullptr)
					{
						Found = &PaletteLookup.Add(Key, static_cast<uint16>(Prefab.Palette.Num()));
						Prefab.Palette.Add(Entry);
					}

					Prefab.Voxels.Add(*Found);
				}
			}
		}

		if (UnloadedVoxels > 0)
		{
			// Unloaded chunks read as air, which would silently capture a building
			// with a missing corner.
			UE_LOG(LogMadFallVoxel, Error,
				TEXT("%d voxel(s) in the box are in unloaded chunks. Load the area first (mad.world.loadarea)."),
				UnloadedVoxels);
			return;
		}

		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Prefabs"));
		IFileManager::Get().MakeDirectory(*Directory, true);

		FString FileName = Prefab.Id.ToString();
		FileName.ReplaceInline(TEXT(":"), TEXT("__"));
		const FString Path = FPaths::Combine(Directory, FileName + TEXT(".json"));

		const FString Text = MadFall::PrefabJson::WriteText(Prefab);
		if (!FFileHelper::SaveStringToFile(Text, *Path))
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Could not write %s"), *Path);
			return;
		}

		UE_LOG(LogMadFallVoxel, Display,
			TEXT("Captured %s: %dx%dx%d, %d palette entries, %d solid voxels, %d bytes -> %s"),
			*Prefab.Id.ToString(), Prefab.Size.X, Prefab.Size.Y, Prefab.Size.Z,
			Prefab.Palette.Num(), Prefab.CountSolidVoxels(), Text.Len(), *FPaths::ConvertRelativePathToFull(Path));
	}));

// ---------------------------------------------------------------------------
// Debug fixtures
// ---------------------------------------------------------------------------

static FAutoConsoleCommandWithWorldAndArgs GMadDebugTerrainCommand(
	TEXT("mad.debug.fillterrain"),
	TEXT("mad.debug.fillterrain <chunkX> <chunkY> <chunkZ> [amplitude] - writes a smooth heightfield into a loaded chunk."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		// A TEST FIXTURE, not a world generator. It exists so the isosurface
		// mesher can be exercised and looked at before Phase 3 exists; the real
		// generator will be layered noise driven by a seed, and this command
		// will keep being useful for reproducing meshing bugs in isolation.
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[3];
		if (!ParseInts(Args, 0, 3, Coords,
			TEXT("mad.debug.fillterrain <chunkX> <chunkY> <chunkZ> [amplitude]"))) { return; }

		float Amplitude = 8.0f;
		if (Args.Num() > 3)
		{
			FDefaultValueHelper::ParseFloat(Args[3], Amplitude);
		}

		const FMadChunkCoord Coord(Coords[0], Coords[1], Coords[2]);
		if (!Subsystem->FindChunk(Coord).IsValid())
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Chunk %s is not loaded."), *Coord.ToString());
			return;
		}

		FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();
		const uint16 Stone = Registry.ResolveRuntimeId(FName(TEXT("madfall:stone")));
		const uint16 Grass = Registry.ResolveRuntimeId(FName(TEXT("madfall:grass")));
		const uint16 Air = MadFall::BlockTypeAir;

		const int32 BaseX = Coord.X * MadFall::ChunkSize;
		const int32 BaseY = Coord.Y * MadFall::ChunkSize;
		const int32 BaseZ = Coord.Z * MadFall::ChunkSize;

		int32 Written = 0;

		for (int32 LocalY = 0; LocalY < MadFall::ChunkSize; ++LocalY)
		{
			for (int32 LocalX = 0; LocalX < MadFall::ChunkSize; ++LocalX)
			{
				const float WorldX = static_cast<float>(BaseX + LocalX);
				const float WorldY = static_cast<float>(BaseY + LocalY);

				// Two octaves is enough to produce slopes, saddles and a peak
				// inside one chunk, which is what the mesher needs to be shown.
				const float Height =
					FMath::Sin(WorldX * 0.12f) * Amplitude
					+ FMath::Cos(WorldY * 0.09f) * Amplitude * 0.75f
					+ FMath::Sin((WorldX + WorldY) * 0.05f) * Amplitude * 0.5f
					+ MadFall::ChunkSize * 0.5f;

				for (int32 LocalZ = 0; LocalZ < MadFall::ChunkSize; ++LocalZ)
				{
					const float Depth = Height - static_cast<float>(LocalZ);

					// Density is a soft band around the surface rather than a
					// hard 0/255 step. Surface Nets interpolates the crossing
					// within a cell, so a gradient is what turns a staircase
					// into a smooth slope - a binary field would mesh as blocks.
					const float Normalized = FMath::Clamp(Depth * 0.5f + 0.5f, 0.0f, 1.0f);
					const uint8 Density = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.0f));

					FMadVoxel Voxel;
					Voxel.Density = Density;
					Voxel.Damage = 0;
					Voxel.Rotation = 0;
					Voxel.Flags = 0;

					if (Density < 128)
					{
						Voxel.BlockTypeID = Air;
					}
					else
					{
						Voxel.BlockTypeID = (Depth < 2.0f) ? Grass : Stone;
					}

					Subsystem->SetVoxel(BaseX + LocalX, BaseY + LocalY, BaseZ + LocalZ, Voxel);
					++Written;
				}
			}
		}

		UE_LOG(LogMadFallVoxel, Display, TEXT("Wrote a heightfield into chunk %s (%d voxels, amplitude %.1f)."),
			*Coord.ToString(), Written, Amplitude);
	}));

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

static FAutoConsoleCommandWithWorldAndArgs GMadRegionInfoCommand(
	TEXT("mad.region.info"),
	TEXT("mad.region.info <regionX> <regionY> - chunk count, sector use and string table size."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadVoxelWorldSubsystem* Subsystem = GetSubsystem(World);
		if (Subsystem == nullptr) { return; }

		int32 Coords[2];
		if (!ParseInts(Args, 0, 2, Coords, TEXT("mad.region.info <regionX> <regionY>"))) { return; }

		LogMultiline(Subsystem->DescribeRegion(FMadRegionCoord(Coords[0], Coords[1])));
	}));

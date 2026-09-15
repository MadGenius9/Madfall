// Copyright MadFall. All Rights Reserved.

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "MadChunkMeshSubsystem.h"
#include "MadFallMesher.h"
#include "Misc/DefaultValueHelper.h"

/**
 * Console commands for the mesher.
 *
 * `mad.mesh.stats` is the important one: it reports worst-case game-thread cost
 * against the 2 ms budget from the engineering standards, measured rather than
 * assumed.
 */
namespace
{
	UMadChunkMeshSubsystem* GetMeshSubsystem(UWorld* World)
	{
		if (World == nullptr)
		{
			UE_LOG(LogMadFallMesher, Error, TEXT("No world; run this from a running game or PIE session."));
			return nullptr;
		}

		UMadChunkMeshSubsystem* Subsystem = World->GetSubsystem<UMadChunkMeshSubsystem>();
		if (Subsystem == nullptr)
		{
			UE_LOG(LogMadFallMesher, Error, TEXT("The chunk mesh subsystem is not available in this world."));
		}

		return Subsystem;
	}

	void LogMultiline(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogMadFallMesher, Display, TEXT("%s"), *Line);
		}
	}
}

static FAutoConsoleCommandWithWorld GMadMeshStatsCommand(
	TEXT("mad.mesh.stats"),
	TEXT("Chunk meshing throughput and worst-case game-thread cost against the 2 ms budget."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadChunkMeshSubsystem* Subsystem = GetMeshSubsystem(World))
		{
			LogMultiline(Subsystem->DescribeStats());
		}
	}));

static FAutoConsoleCommandWithWorld GMadMeshResetStatsCommand(
	TEXT("mad.mesh.resetstats"),
	TEXT("Zeroes the worst-case timings, so a measurement can exclude the initial world load."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadChunkMeshSubsystem* Subsystem = GetMeshSubsystem(World))
		{
			Subsystem->ResetStats();
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadMeshRebuildCommand(
	TEXT("mad.mesh.rebuild"),
	TEXT("mad.mesh.rebuild <chunkX> <chunkY> <chunkZ> - remeshes one chunk synchronously."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadChunkMeshSubsystem* Subsystem = GetMeshSubsystem(World);
		if (Subsystem == nullptr) { return; }

		if (Args.Num() < 3)
		{
			UE_LOG(LogMadFallMesher, Error, TEXT("Usage: mad.mesh.rebuild <chunkX> <chunkY> <chunkZ>"));
			return;
		}

		int32 Coords[3] = { 0, 0, 0 };
		for (int32 Index = 0; Index < 3; ++Index)
		{
			if (!FDefaultValueHelper::ParseInt(Args[Index], Coords[Index]))
			{
				UE_LOG(LogMadFallMesher, Error, TEXT("'%s' is not an integer."), *Args[Index]);
				return;
			}
		}

		const FMadChunkCoord Coord(Coords[0], Coords[1], Coords[2]);

		FString Error;
		if (Subsystem->RebuildChunkNow(Coord, Error))
		{
			UE_LOG(LogMadFallMesher, Display, TEXT("Rebuilt chunk %s."), *Coord.ToString());
			LogMultiline(Subsystem->DescribeStats());
		}
		else
		{
			UE_LOG(LogMadFallMesher, Error, TEXT("Could not rebuild chunk %s: %s"), *Coord.ToString(), *Error);
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadMeshInspectCommand(
	TEXT("mad.mesh.inspect"),
	TEXT("mad.mesh.inspect <chunkX> <chunkY> <chunkZ> - meshes a chunk and dumps per-section detail WITHOUT applying it."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		// Separating "what did the mesher produce" from "what does it look
		// like" is the difference between diagnosing a bug and guessing at it.
		UMadChunkMeshSubsystem* Subsystem = GetMeshSubsystem(World);
		if (Subsystem == nullptr) { return; }

		if (Args.Num() < 3)
		{
			UE_LOG(LogMadFallMesher, Error, TEXT("Usage: mad.mesh.inspect <chunkX> <chunkY> <chunkZ>"));
			return;
		}

		int32 Coords[3] = { 0, 0, 0 };
		for (int32 Index = 0; Index < 3; ++Index)
		{
			if (!FDefaultValueHelper::ParseInt(Args[Index], Coords[Index]))
			{
				UE_LOG(LogMadFallMesher, Error, TEXT("'%s' is not an integer."), *Args[Index]);
				return;
			}
		}

		FString Error;
		const FString Report = Subsystem->InspectChunkMesh(
			FMadChunkCoord(Coords[0], Coords[1], Coords[2]), Error);

		if (Report.IsEmpty())
		{
			UE_LOG(LogMadFallMesher, Error, TEXT("%s"), *Error);
			return;
		}

		LogMultiline(Report);
	}));

static FAutoConsoleCommandWithWorld GMadMeshRebuildAllNowCommand(
	TEXT("mad.mesh.rebuildallnow"),
	TEXT("Meshes every loaded chunk synchronously. Blocks. For captures and tests."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadChunkMeshSubsystem* Subsystem = GetMeshSubsystem(World))
		{
			Subsystem->RebuildAllNow();
			LogMultiline(Subsystem->DescribeStats());
		}
	}));

static FAutoConsoleCommandWithWorld GMadMeshRebuildAllCommand(
	TEXT("mad.mesh.rebuildall"),
	TEXT("Queues every meshed chunk for a rebuild."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadChunkMeshSubsystem* Subsystem = GetMeshSubsystem(World))
		{
			const int32 Count = Subsystem->RebuildAll();
			UE_LOG(LogMadFallMesher, Display, TEXT("Queued %d chunk(s) for rebuild."), Count);
		}
	}));

// Copyright MadFall. All Rights Reserved.

#include "MadChunkStreamingSubsystem.h"

#include "MadFrameBudget.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadFallCore.h"
#include "MadFallStats.h"
#include "MadVoxelWorldSubsystem.h"

DECLARE_CYCLE_STAT(TEXT("Chunk Streaming"), STAT_MadStreaming, STATGROUP_MadFallVoxel);

namespace
{
	TAutoConsoleVariable<int32> CVarStreamRadius(
		TEXT("mad.stream.Radius"),
		8,
		TEXT("Horizontal streaming radius around each player, in chunks (32 m each)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarStreamVertical(
		TEXT("mad.stream.VerticalRadius"),
		2,
		TEXT("Chunk layers loaded above and below each player's layer."),
		ECVF_Default);

	/**
	 * Worker-thread loads are cheap for the game thread, but each finished one
	 * triggers a mesh job. Capping in-flight loads keeps a teleport from queueing
	 * a thousand chunks behind the ones the player can actually see.
	 */
	TAutoConsoleVariable<int32> CVarMaxLoadsInFlight(
		TEXT("mad.stream.MaxLoadsInFlight"),
		24,
		TEXT("Upper bound on concurrent async chunk loads requested by streaming."),
		ECVF_Default);

	/**
	 * Unloading is cheap now: clean chunks are not written, and dirty ones are
	 * saved by a worker. What remains per unload is the unload broadcast (mesh
	 * component back to the pool, structural and container bookkeeping).
	 */
	TAutoConsoleVariable<int32> CVarMaxUnloadsPerFrame(
		TEXT("mad.stream.MaxUnloadsPerFrame"),
		8,
		TEXT("Chunks streaming may unload (and save) per frame."),
		ECVF_Default);

	/** Chunks stay loaded this far beyond the radius, so walking along a border does not thrash. */
	constexpr int32 UnloadHysteresis = 2;
}

bool UMadChunkStreamingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UMadChunkStreamingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	VoxelWorld = Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
}

TStatId UMadChunkStreamingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadChunkStreamingSubsystem, STATGROUP_Tickables);
}

void UMadChunkStreamingSubsystem::SetExtraSource(FName Name, const FVector& WorldLocation, int32 RadiusChunks)
{
	ExtraSources.Add(Name, TPair<FVector, int32>(WorldLocation, RadiusChunks));
}

void UMadChunkStreamingSubsystem::RemoveExtraSource(FName Name)
{
	ExtraSources.Remove(Name);
}

void UMadChunkStreamingSubsystem::GatherSources(TArray<FSource>& OutSources) const
{
	auto ToChunk = [](const FVector& Location)
	{
		return MadFall::WorldToChunk(
			FMath::FloorToInt32(Location.X / MadFall::VoxelSizeUU),
			FMath::FloorToInt32(Location.Y / MadFall::VoxelSizeUU),
			FMath::FloorToInt32(Location.Z / MadFall::VoxelSizeUU));
	};

	const int32 Radius = FMath::Clamp(CVarStreamRadius.GetValueOnGameThread(), 1, 32);

	if (const UWorld* World = GetWorld())
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* Controller = It->Get();
			if (Controller == nullptr)
			{
				continue;
			}

			FVector Location;
			if (const APawn* Pawn = Controller->GetPawn())
			{
				Location = Pawn->GetActorLocation();
			}
			else
			{
				FRotator Unused;
				Controller->GetPlayerViewPoint(Location, Unused);
			}
			OutSources.Add(FSource{ ToChunk(Location), Radius });
		}
	}

	for (const TPair<FName, TPair<FVector, int32>>& Pair : ExtraSources)
	{
		OutSources.Add(FSource{ ToChunk(Pair.Value.Key), FMath::Clamp(Pair.Value.Value, 1, 32) });
	}
}

void UMadChunkStreamingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Streaming);

	if (VoxelWorld == nullptr)
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_MadStreaming);

	TArray<FSource> Sources;
	GatherSources(Sources);
	if (Sources.Num() == 0)
	{
		return;
	}

	const int32 Vertical = FMath::Clamp(CVarStreamVertical.GetValueOnGameThread(), 0, MadFall::WorldChunkLayers);
	const double Now = FPlatformTime::Seconds();

	// The wanted and unload sets only change when a source crosses into another
	// chunk (every few seconds at a sprint). Scanning ~1000 coordinates and
	// copying every loaded chunk key each frame cost about a millisecond of
	// every frame; now it happens on a change, plus once a second so chunks
	// unloaded or loaded by anything else are noticed.
	const bool bSourcesChanged = Sources.Num() != LastSources.Num()
		|| Vertical != LastVertical
		|| Sources.ContainsByPredicate([this, &Sources](const FSource& Source)
		{
			const int32 Index = static_cast<int32>(&Source - Sources.GetData());
			return !(Source.Centre == LastSources[Index].Centre) || Source.Radius != LastSources[Index].Radius;
		});
	if (bSourcesChanged || Now >= NextRescanSeconds)
	{
		RebuildQueues(Sources, Vertical);
		LastSources = Sources;
		LastVertical = Vertical;
		NextRescanSeconds = Now + 1.0;
	}

	// --- loads, nearest first ---------------------------------------------------
	int32 Slots = CVarMaxLoadsInFlight.GetValueOnGameThread() - VoxelWorld->NumLoadsInFlight();
	while (Slots > 0 && LoadCursor < LoadQueue.Num())
	{
		const FMadChunkCoord& Coord = LoadQueue[LoadCursor++];
		if (VoxelWorld->FindChunk(Coord).IsValid() || VoxelWorld->IsChunkLoading(Coord))
		{
			continue;
		}
		VoxelWorld->RequestLoadChunk(Coord);
		if (VoxelWorld->IsChunkLoading(Coord))
		{
			++TotalRequested;
			--Slots;
		}
		// Not loading means the request was deferred (a save of that chunk is
		// still being written); the next rescan queues it again.
	}
	LastPendingLoads = LoadQueue.Num() - LoadCursor;

	// --- unloads, past the hysteresis margin --------------------------------------
	int32 UnloadBudget = CVarMaxUnloadsPerFrame.GetValueOnGameThread();
	while (UnloadBudget > 0 && UnloadCursor < UnloadQueue.Num())
	{
		const FMadChunkCoord& Coord = UnloadQueue[UnloadCursor++];
		if (!VoxelWorld->FindChunk(Coord).IsValid())
		{
			continue;
		}

		FString Error;
		if (!VoxelWorld->UnloadChunk(Coord, /*bSave*/ true, Error))
		{
			UE_LOG(LogMadFallVoxel, Warning, TEXT("Streaming could not unload %s: %s"), *Coord.ToString(), *Error);
		}
		++TotalUnloaded;
		--UnloadBudget;
	}
}

void UMadChunkStreamingSubsystem::RebuildQueues(const TArray<FSource>& Sources, int32 Vertical)
{
	++TotalRescans;

	// Offsets depend only on radius and vertical range, so they are generated
	// and sorted by distance once and reused for every rebuild.
	LoadQueue.Reset();
	LoadCursor = 0;
	for (const FSource& Source : Sources)
	{
		const TArray<FIntVector>& Offsets = GetSortedOffsets(Source.Radius, Vertical);
		for (const FIntVector& Offset : Offsets)
		{
			const FMadChunkCoord Coord(Source.Centre.X + Offset.X, Source.Centre.Y + Offset.Y, Source.Centre.Z + Offset.Z);
			if (Coord.IsValidZ() && !VoxelWorld->FindChunk(Coord).IsValid())
			{
				LoadQueue.Add(Coord);
			}
		}
	}

	UnloadQueue.Reset();
	UnloadCursor = 0;
	TArray<FMadChunkCoord> Loaded;
	VoxelWorld->GetLoadedChunkCoords(Loaded);
	for (const FMadChunkCoord& Coord : Loaded)
	{
		bool bKeep = false;
		for (const FSource& Source : Sources)
		{
			const int32 Keep = Source.Radius + UnloadHysteresis;
			if (FMath::Abs(Coord.X - Source.Centre.X) <= Keep && FMath::Abs(Coord.Y - Source.Centre.Y) <= Keep
				&& FMath::Abs(Coord.Z - Source.Centre.Z) <= Vertical + UnloadHysteresis)
			{
				bKeep = true;
				break;
			}
		}
		if (!bKeep)
		{
			UnloadQueue.Add(Coord);
		}
	}
}

const TArray<FIntVector>& UMadChunkStreamingSubsystem::GetSortedOffsets(int32 Radius, int32 Vertical)
{
	const FIntPoint Key(Radius, Vertical);
	if (const TArray<FIntVector>* Cached = OffsetCache.Find(Key))
	{
		return *Cached;
	}

	TArray<FIntVector> Offsets;
	for (int32 DZ = -Vertical; DZ <= Vertical; ++DZ)
	{
		for (int32 DY = -Radius; DY <= Radius; ++DY)
		{
			for (int32 DX = -Radius; DX <= Radius; ++DX)
			{
				// Circular footprint: the corners of the square are the chunks
				// least likely to be seen and most expensive to keep.
				if (DX * DX + DY * DY <= Radius * Radius + Radius)
				{
					Offsets.Emplace(DX, DY, DZ);
				}
			}
		}
	}

	// Nearest first, with vertical distance weighted up: the layer the player
	// stands in matters far more than the one 30 m below them.
	Offsets.Sort([](const FIntVector& A, const FIntVector& B)
	{
		return A.X * A.X + A.Y * A.Y + A.Z * A.Z * 4 < B.X * B.X + B.Y * B.Y + B.Z * B.Z * 4;
	});
	return OffsetCache.Add(Key, MoveTemp(Offsets));
}

bool UMadChunkStreamingSubsystem::IsAreaLoaded(const FVector& WorldLocation, int32 RadiusChunks) const
{
	if (VoxelWorld == nullptr)
	{
		return false;
	}

	const FMadChunkCoord Centre = MadFall::WorldToChunk(
		FMath::FloorToInt32(WorldLocation.X / MadFall::VoxelSizeUU),
		FMath::FloorToInt32(WorldLocation.Y / MadFall::VoxelSizeUU),
		FMath::FloorToInt32(WorldLocation.Z / MadFall::VoxelSizeUU));

	for (int32 DZ = -1; DZ <= 1; ++DZ)
	{
		for (int32 DY = -RadiusChunks; DY <= RadiusChunks; ++DY)
		{
			for (int32 DX = -RadiusChunks; DX <= RadiusChunks; ++DX)
			{
				const FMadChunkCoord Coord(Centre.X + DX, Centre.Y + DY, Centre.Z + DZ);
				if (Coord.IsValidZ() && !VoxelWorld->FindChunk(Coord).IsValid())
				{
					return false;
				}
			}
		}
	}
	return true;
}

FString UMadChunkStreamingSubsystem::DescribeStatus() const
{
	return FString::Printf(TEXT("Streaming: radius %d, vertical %d; %d wanted, %d in flight; %lld requested, %lld unloaded total"),
		CVarStreamRadius.GetValueOnGameThread(), CVarStreamVertical.GetValueOnGameThread(), LastPendingLoads,
		VoxelWorld ? VoxelWorld->NumLoadsInFlight() : 0, TotalRequested, TotalUnloaded);
}

static FAutoConsoleCommandWithWorld GMadStreamStatusCommand(
	TEXT("mad.stream.status"),
	TEXT("Chunk streaming counters."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadChunkStreamingSubsystem* Streaming = World ? World->GetSubsystem<UMadChunkStreamingSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("%s"), *Streaming->DescribeStatus());
		}
		else
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("No chunk streaming in this world (editor worlds do not stream)."));
		}
	}));

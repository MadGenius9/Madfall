// Copyright MadFall. All Rights Reserved.

#include "MadVoxelWorldSubsystem.h"

#include "MadFrameBudget.h"
#include "Algo/AllOf.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/FileManager.h"
#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadPrefabRegistry.h"
#include "MadChunkSampleGrid.h"
#include "MadChunkSerializer.h"
#include "MadDefinitionPatches.h"
#include "MadDefinitionSources.h"
#include "MadModManifest.h"
#include "MadFallCore.h"
#include "MadFallStats.h"
#include "MadRegionFile.h"
#include "MadSession.h"
#include "MadWorldBackup.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/StringBuilder.h"
#include "Tasks/Task.h"

DECLARE_CYCLE_STAT(TEXT("World LoadChunk"), STAT_MadWorldLoadChunk, STATGROUP_MadFallVoxel);
DECLARE_CYCLE_STAT(TEXT("World SaveAll"), STAT_MadWorldSaveAll, STATGROUP_MadFallVoxel);
DECLARE_CYCLE_STAT(TEXT("World PublishLoads"), STAT_MadWorldPublishLoads, STATGROUP_MadFallVoxel);

namespace
{
	/**
	 * The process-wide registry.
	 *
	 * Built lazily rather than in FMadFallCoreModule::StartupModule because
	 * staging UMadBlockDefinition data assets needs the asset registry, which
	 * does not exist at PreDefault. Building it once on first use - rather than
	 * building a JSON-only registry early and rebuilding later - avoids ever
	 * renumbering runtime ids while chunks are loaded.
	 */
	TUniquePtr<FMadBlockRegistry> GBlockRegistry;

	/** Same lifecycle and the same reasons as the block registry. */
	TUniquePtr<FMadBiomeRegistry> GBiomeRegistry;

	TUniquePtr<FMadPrefabRegistry> GPrefabRegistry;

	void LogErrors(const TArray<FMadDefinitionError>& Errors)
	{
		for (const FMadDefinitionError& Error : Errors)
		{
			UE_LOG(LogMadFallRegistry, Warning, TEXT("%s"), *Error.ToString());
		}
	}

	void BuildBlockRegistry()
	{
		GBlockRegistry = MakeUnique<FMadBlockRegistry>();
		GBlockRegistry->BeginLoad();

		TArray<FMadDefinitionError> Errors;
		int32 CoreCount = 0;
		int32 ModCount = 0;

		// First-party blocks load through exactly the same JSON path a mod
		// uses, first in the resolved load order. If the shipped content could
		// do something a mod cannot, the mod API would be a lie.
		MadFall::Definitions::ForEachSource(TEXT("blocks"), [&](const FString& Directory, FName ModId)
		{
			const int32 Staged = GBlockRegistry->AddFromDirectory(Directory, ModId, Errors);
			(ModId == FName(MadFall::CoreModId) ? CoreCount : ModCount) += Staged;
		});

		const int32 AssetCount = GBlockRegistry->AddFromAssetRegistry(Errors);

		GBlockRegistry->ApplyPatches(MadFall::GetPatchSet(), Errors);
		GBlockRegistry->FinishLoad(Errors);

		UE_LOG(LogMadFallRegistry, Log,
			TEXT("Block definitions: %d core, %d from mods, %d data assets. %d validation message(s)."),
			CoreCount, ModCount, AssetCount, Errors.Num());

		MadFall::SetBlockRegistry(GBlockRegistry.Get());
	}
}

namespace
{
	void BuildBiomeRegistry()
	{
		GBiomeRegistry = MakeUnique<FMadBiomeRegistry>();
		GBiomeRegistry->BeginLoad();

		TArray<FMadDefinitionError> Errors;
		int32 CoreCount = 0;
		int32 ModCount = 0;

		// Biomes load through the same path, the same validation, the same
		// inheritance and the same patches as blocks.
		MadFall::Definitions::ForEachSource(TEXT("biomes"), [&](const FString& Directory, FName ModId)
		{
			const int32 Staged = GBiomeRegistry->AddFromDirectory(Directory, ModId, Errors);
			(ModId == FName(MadFall::CoreModId) ? CoreCount : ModCount) += Staged;
		});

		GBiomeRegistry->ApplyPatches(MadFall::GetPatchSet(), Errors);
		GBiomeRegistry->FinishLoad(Errors);

		UE_LOG(LogMadFallRegistry, Log, TEXT("Biome definitions: %d core, %d from mods. %d validation message(s)."),
			CoreCount, ModCount, Errors.Num());
	}
}

FMadBlockRegistry& UMadVoxelWorldSubsystem::GetBlockRegistry()
{
	if (!GBlockRegistry.IsValid())
	{
		BuildBlockRegistry();
	}
	return *GBlockRegistry;
}

FMadBiomeRegistry& UMadVoxelWorldSubsystem::GetBiomeRegistry()
{
	if (!GBiomeRegistry.IsValid())
	{
		BuildBiomeRegistry();
	}
	return *GBiomeRegistry;
}

FMadPrefabRegistry& UMadVoxelWorldSubsystem::GetPrefabRegistry()
{
	if (!GPrefabRegistry.IsValid())
	{
		GPrefabRegistry = MakeUnique<FMadPrefabRegistry>();

		TArray<FMadDefinitionError> Errors;
		int32 CoreCount = 0;
		int32 ModCount = 0;

		// Same sources and load order as every other definition. Prefabs are
		// voxels rather than fields, so they are replaced, never patched.
		MadFall::Definitions::ForEachSource(TEXT("prefabs"), [&](const FString& Directory, FName ModId)
		{
			const int32 Staged = GPrefabRegistry->AddFromDirectory(Directory, ModId, Errors);
			(ModId == FName(MadFall::CoreModId) ? CoreCount : ModCount) += Staged;
		});

		GPrefabRegistry->Finalize();

		UE_LOG(LogMadFallRegistry, Log, TEXT("Prefab definitions: %d core, %d from mods. %d validation message(s)."),
			CoreCount, ModCount, Errors.Num());
		LogErrors(Errors);
	}
	return *GPrefabRegistry;
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void UMadVoxelWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Which world: the title screen's backdrop, the world the menu selected,
	// -MadWorld= (so a probe or experiment never writes into the world CI and
	// day-to-day play use), or DevWorld. -MadSeed= seeds a world being created.
	FString WorldName = TEXT("DevWorld");
	TOptional<int64> SeedIfNew;
	FString Requested;
	FString SeedText;
	if (MadFall::Session::IsTitleScreen())
	{
		WorldName = MadFall::Session::TitleWorldName;
		SeedIfNew = MadFall::Session::TitleWorldSeed;
		// The title's backdrop is scenery: regenerated every launch, never written.
		bReadOnly = true;
	}
	else if (MadFall::Session::GetSelectedWorld(Requested, SeedIfNew))
	{
		WorldName = Requested;
	}
	else if (FParse::Value(FCommandLine::Get(), TEXT("MadWorld="), Requested))
	{
		if (MadFall::Session::IsValidWorldName(Requested))
		{
			WorldName = Requested;
		}
		else
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("-MadWorld=%s is not a valid world name (letters, digits, - and _, not starting with _); using %s."), *Requested, *WorldName);
		}
	}
	if (!SeedIfNew.IsSet() && FParse::Value(FCommandLine::Get(), TEXT("MadSeed="), SeedText))
	{
		SeedIfNew = MadFall::Session::ParseSeed(SeedText);
	}

	WorldDirectory = FPaths::Combine(MadFall::Session::GetWorldsRoot(), WorldName);
	if (bReadOnly)
	{
		// Region files are opened (and so created) even when nothing is saved into
		// them. Left over, they made the next launch read the backdrop as a world
		// from before world.json - seed 0, a different landscape each time the
		// title's seed changes. It is scenery: start it clean.
		IFileManager::Get().DeleteDirectory(*WorldDirectory, /*RequireExists*/ false, /*Tree*/ true);
	}
	else if (GetWorld() != nullptr && GetWorld()->IsGameWorld())
	{
		// Before a single region file opens: the one moment the folder is sure to
		// be quiescent, so a file copy is a consistent snapshot (MadWorldBackup.h).
		FString Backup;
		FString BackupError;
		const double BackupStart = FPlatformTime::Seconds();
		if (!MadFall::WorldBackup::BackUp(WorldDirectory, Backup, BackupError))
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("World backup of %s failed: %s"), *WorldName, *BackupError);
		}
		else if (!Backup.IsEmpty())
		{
			UE_LOG(LogMadFallVoxel, Display, TEXT("World backup %s of %s made in %.0f ms."), *Backup, *WorldName, (FPlatformTime::Seconds() - BackupStart) * 1000.0);
		}
	}

	GetBlockRegistry();
	GetBiomeRegistry();
	GetPrefabRegistry();

	// The seed belongs to the world folder. A folder with regions but no
	// world.json predates world.json and was always generated with seed 0.
	FMadWorldInfo Info;
	if (MadFall::Session::ReadWorldInfo(WorldDirectory, Info))
	{
		WorldSeed = static_cast<uint64>(Info.Seed);
		Difficulty = Info.Difficulty;
	}
	else
	{
		const bool bExisting = IFileManager::Get().DirectoryExists(*FPaths::Combine(WorldDirectory, TEXT("regions")));
		WorldSeed = static_cast<uint64>(bExisting ? 0 : SeedIfNew.Get(0));
		if (!bReadOnly)
		{
			Info.Name = WorldName;
			Info.DisplayName = WorldName;
			Info.Seed = static_cast<int64>(WorldSeed);
			Info.Created = FDateTime::UtcNow();
			Info.LastPlayed = Info.Created;
			Info.Directory = WorldDirectory;
			FString Error;
			if (!MadFall::Session::WriteWorldInfo(Info, Error))
			{
				UE_LOG(LogMadFallVoxel, Error, TEXT("World %s: %s"), *WorldName, *Error);
			}
		}
	}

	SetWorldGenSettings(WorldGenSettings);

	UE_LOG(LogMadFallVoxel, Log,
		TEXT("Voxel world ready. World directory: %s, seed %llu, %d biomes, worldgen version 0x%llX."),
		*WorldDirectory, WorldSeed, GetBiomeRegistry().Num(), WorldGenVersion);
}

void UMadVoxelWorldSubsystem::Deinitialize()
{
	WaitForAllPendingSaves();

	FString Error;
	if (!SaveAll(Error))
	{
		UE_LOG(LogMadFallVoxel, Error, TEXT("Failed to save the world on shutdown: %s"), *Error);
	}

	if (!CloseAllRegions(Error))
	{
		UE_LOG(LogMadFallVoxel, Error, TEXT("Failed to close region files: %s"), *Error);
	}

	LoadedChunks.Reset();

	Super::Deinitialize();
}

TStatId UMadVoxelWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadVoxelWorldSubsystem, STATGROUP_Tickables);
}

void UMadVoxelWorldSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(WorldLoads);

	SCOPE_CYCLE_COUNTER(STAT_MadWorldPublishLoads);

	TArray<FPendingLoad> Ready;
	{
		FScopeLock Lock(&PendingLock);
		// bDirty is the game thread's; a worker that failed to write reports here.
		for (const FMadChunkPtr& Failed : FailedSaves)
		{
			Failed->bDirty = true;
		}
		FailedSaves.Reset();
		if (CompletedLoads.Num() == 0)
		{
			return;
		}
		Ready = MoveTemp(CompletedLoads);
		CompletedLoads.Reset();
	}

	// The only game-thread work per finished load is a map insert and a move of
	// already-built storage. Everything expensive happened on the worker.
	for (FPendingLoad& Load : Ready)
	{
		InFlightLoads.Remove(Load.Coord);

		if (!Load.bSucceeded)
		{
			UE_LOG(LogMadFallVoxel, Warning, TEXT("Chunk %s failed to load: %s"),
				*Load.Coord.ToString(), *Load.Error);
			continue;
		}

		if (LoadedChunks.Contains(Load.Coord))
		{
			// Something loaded it synchronously while the task was in flight.
			// Dropping the async result is correct: the resident copy may
			// already have edits on it.
			continue;
		}

		FMadChunkPtr Chunk = MakeShared<FMadChunk, ESPMode::ThreadSafe>();
		Chunk->Coord = Load.Coord;
		Chunk->Storage = MoveTemp(Load.Storage);
		LoadedChunks.Add(Load.Coord, Chunk);
		ChunkChangedDelegate.Broadcast(Load.Coord);
		ChunkLoadedDelegate.Broadcast(Load.Coord);
	}
}

// ===========================================================================
// Configuration
// ===========================================================================

void UMadVoxelWorldSubsystem::SetWorldDirectory(const FString& InDirectory)
{
	FString Error;
	if (!SaveAll(Error))
	{
		UE_LOG(LogMadFallVoxel, Error, TEXT("Could not save before switching worlds: %s"), *Error);
	}
	CloseAllRegions(Error);

	LoadedChunks.Reset();
	WorldDirectory = FPaths::ConvertRelativePathToFull(InDirectory);

	UE_LOG(LogMadFallVoxel, Log, TEXT("World directory is now %s"), *WorldDirectory);
}

void UMadVoxelWorldSubsystem::SetSeed(int64 InSeed)
{
	WorldSeed = static_cast<uint64>(InSeed);

	// The generator caches derived seeds, so it has to be rebuilt - otherwise
	// the seed would change and the world would not.
	WorldGenSettings.Seed = static_cast<uint32>(WorldSeed);
	SetWorldGenSettings(WorldGenSettings);
}

void UMadVoxelWorldSubsystem::SetWorldGenSettings(const FMadWorldGenSettings& InSettings)
{
	WorldGenSettings = InSettings;
	WorldGenSettings.Seed = static_cast<uint32>(WorldSeed);

	WorldGenerator = MakeUnique<FMadWorldGenerator>(
		WorldGenSettings, GetBiomeRegistry(), GetBlockRegistry(), &GetPrefabRegistry());

	// Stamped into every region header. A chunk generated under different
	// settings would not meet its neighbours cleanly, and the region loader
	// refuses to mix them rather than producing a seam nobody can explain.
	WorldGenVersion = WorldGenSettings.GetGenerationVersion();
}

// ===========================================================================
// Regions
// ===========================================================================

FMadRegionFile* UMadVoxelWorldSubsystem::GetOrOpenRegion(const FMadRegionCoord& Coord, FString& OutError)
{
	if (TUniquePtr<FMadRegionFile>* Existing = OpenRegions.Find(Coord))
	{
		return Existing->Get();
	}

	const FString Path = FPaths::Combine(WorldDirectory, TEXT("regions"), FMadRegionFile::MakeFileName(Coord));

	TUniquePtr<FMadRegionFile> Region = MakeUnique<FMadRegionFile>();
	if (!Region->Open(Path, Coord, WorldSeed, WorldGenVersion, /*bCreateIfMissing*/ true, OutError))
	{
		return nullptr;
	}

	FMadRegionFile* Raw = Region.Get();
	OpenRegions.Add(Coord, MoveTemp(Region));
	return Raw;
}

bool UMadVoxelWorldSubsystem::CloseAllRegions(FString& OutError)
{
	WaitForAllPendingSaves();

	FScopeLock Lock(&RegionLock);

	bool bAllOk = true;
	for (TPair<FMadRegionCoord, TUniquePtr<FMadRegionFile>>& Pair : OpenRegions)
	{
		FString Error;
		if (!Pair.Value->Close(Error))
		{
			OutError = Error;
			bAllOk = false;
		}
	}

	OpenRegions.Reset();
	return bAllOk;
}

// ===========================================================================
// Chunk load / save
// ===========================================================================

bool UMadVoxelWorldSubsystem::LoadChunkStorage(const FMadChunkCoord& Coord, FMadChunkStorage& OutStorage, FString& OutError)
{
	SCOPE_CYCLE_COUNTER(STAT_MadWorldLoadChunk);

	if (!Coord.IsValidZ())
	{
		OutError = FString::Printf(TEXT("chunk %s is outside the world's vertical range (%d..%d)"),
			*Coord.ToString(), MadFall::WorldMinChunkZ, MadFall::WorldMaxChunkZ);
		return false;
	}

	const FMadRegionCoord RegionCoord = MadFall::ChunkToRegion(Coord);
	const int32 Slot = MadFall::ChunkToRegionSlot(Coord);
	check(Slot != INDEX_NONE);

	FMadSerializedChunk Serialized;
	bool bHadStoredChunk = false;

	{
		FScopeLock Lock(&RegionLock);

		FMadRegionFile* Region = GetOrOpenRegion(RegionCoord, OutError);
		if (Region == nullptr)
		{
			return false;
		}

		if (Region->HasChunk(Slot))
		{
			if (!Region->ReadChunk(Slot, Serialized, OutError))
			{
				return false;
			}
			bHadStoredChunk = true;
		}
	}

	if (!bHadStoredChunk)
	{
		// Nothing on disk, so generate it. This is the only place generation is
		// triggered from: a chunk is either loaded or generated, never both, and
		// never generated over something a player has edited.
		if (bGenerationEnabled && WorldGenerator.IsValid())
		{
			WorldGenerator->GenerateChunk(Coord, OutStorage);
		}
		else
		{
			OutStorage = FMadChunkStorage();
		}
		return true;
	}

	// Resolving strings needs the region's table, so take the lock again for
	// the lookup closure. Deserialization itself is pure CPU work.
	FMadRegionFile* Region = nullptr;
	{
		FScopeLock Lock(&RegionLock);
		if (TUniquePtr<FMadRegionFile>* Found = OpenRegions.Find(RegionCoord))
		{
			Region = Found->Get();
		}
	}

	if (Region == nullptr)
	{
		OutError = FString::Printf(TEXT("region %s was closed while chunk %s was loading"),
			*RegionCoord.ToString(), *Coord.ToString());
		return false;
	}

	auto ResolveString = [this, Region](uint32 Index) -> FName
	{
		FScopeLock Lock(&RegionLock);
		return Region->ResolveString(Index);
	};

	return MadFall::ChunkSerializer::Deserialize(
		Serialized, GetBlockRegistry(), ResolveString, OutStorage, OutError);
}

bool UMadVoxelWorldSubsystem::LoadChunkSync(const FMadChunkCoord& Coord, FString& OutError)
{
	if (LoadedChunks.Contains(Coord))
	{
		return true;
	}

	WaitForPendingSave(Coord);

	FMadChunkStorage Storage;
	if (!LoadChunkStorage(Coord, Storage, OutError))
	{
		return false;
	}

	FMadChunkPtr Chunk = MakeShared<FMadChunk, ESPMode::ThreadSafe>();
	Chunk->Coord = Coord;
	Chunk->Storage = MoveTemp(Storage);
	LoadedChunks.Add(Coord, Chunk);
	ChunkChangedDelegate.Broadcast(Coord);
	ChunkLoadedDelegate.Broadcast(Coord);

	return true;
}

void UMadVoxelWorldSubsystem::RequestLoadChunk(const FMadChunkCoord& Coord)
{
	if (LoadedChunks.Contains(Coord) || InFlightLoads.Contains(Coord))
	{
		return;
	}

	// Just unloaded and still being written: ask again next frame rather than
	// read the slot before the write lands.
	if (HasPendingSave(Coord))
	{
		return;
	}

	InFlightLoads.Add(Coord);

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Coord]
	{
		FPendingLoad Result;
		Result.Coord = Coord;
		Result.bSucceeded = LoadChunkStorage(Coord, Result.Storage, Result.Error);

		FScopeLock Lock(&PendingLock);
		CompletedLoads.Add(MoveTemp(Result));
	});
}

bool UMadVoxelWorldSubsystem::WriteChunkStorage(const FMadChunkCoord& Coord, const FMadChunkStorage& Storage,
	bool bPlayerModified, FString& OutError)
{
	const FMadRegionCoord RegionCoord = MadFall::ChunkToRegion(Coord);
	const int32 Slot = MadFall::ChunkToRegionSlot(Coord);

	if (Slot == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("chunk %s is outside the world's vertical range"), *Coord.ToString());
		return false;
	}

	FScopeLock Lock(&RegionLock);

	FMadRegionFile* Region = GetOrOpenRegion(RegionCoord, OutError);
	if (Region == nullptr)
	{
		return false;
	}

	auto InternString = [Region](FName Value) -> uint32
	{
		return Region->InternString(Value);
	};

	FMadSerializedChunk Serialized;
	MadFall::ChunkSerializer::Serialize(Storage, GetBlockRegistry(), InternString, Serialized);

	return Region->WriteChunk(Slot, Serialized, bPlayerModified, OutError);
}

bool UMadVoxelWorldSubsystem::SaveChunk(const FMadChunkCoord& Coord, FString& OutError)
{
	const FMadChunkPtr* Found = LoadedChunks.Find(Coord);
	if (Found == nullptr || !Found->IsValid())
	{
		OutError = FString::Printf(TEXT("chunk %s is not loaded"), *Coord.ToString());
		return false;
	}

	FMadChunk& Chunk = **Found;

	{
		FRWScopeLock ChunkLock(Chunk.Lock, SLT_Write);

		// Compacting on the save path only. Doing it during an edit would
		// renumber palette slots underneath any worker mid-read.
		Chunk.Storage.Compact();

		if (!WriteChunkStorage(Coord, Chunk.Storage, Chunk.bPlayerModified, OutError))
		{
			return false;
		}

		Chunk.bDirty = false;
	}

	FScopeLock Lock(&RegionLock);
	if (TUniquePtr<FMadRegionFile>* Region = OpenRegions.Find(MadFall::ChunkToRegion(Coord)))
	{
		return (*Region)->Flush(OutError);
	}

	return true;
}

bool UMadVoxelWorldSubsystem::SaveAll(FString& OutError)
{
	SCOPE_CYCLE_COUNTER(STAT_MadWorldSaveAll);

	if (bReadOnly)
	{
		return true;
	}

	WaitForAllPendingSaves();

	bool bAllOk = true;
	int32 Saved = 0;

	for (TPair<FMadChunkCoord, FMadChunkPtr>& Pair : LoadedChunks)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}

		FMadChunk& Chunk = *Pair.Value;

		FRWScopeLock ChunkLock(Chunk.Lock, SLT_Write);
		if (!Chunk.bDirty)
		{
			continue;
		}

		Chunk.Storage.Compact();

		FString Error;
		if (!WriteChunkStorage(Pair.Key, Chunk.Storage, Chunk.bPlayerModified, Error))
		{
			UE_LOG(LogMadFallVoxel, Error, TEXT("Failed to save chunk %s: %s"), *Pair.Key.ToString(), *Error);
			OutError = Error;
			bAllOk = false;
			continue;
		}

		Chunk.bDirty = false;
		++Saved;
	}

	{
		FScopeLock Lock(&RegionLock);
		for (TPair<FMadRegionCoord, TUniquePtr<FMadRegionFile>>& Pair : OpenRegions)
		{
			FString Error;
			if (!Pair.Value->Flush(Error))
			{
				OutError = Error;
				bAllOk = false;
			}
		}
	}

	if (Saved > 0)
	{
		UE_LOG(LogMadFallVoxel, Log, TEXT("Saved %d chunk(s)."), Saved);
	}

	return bAllOk;
}

int32 UMadVoxelWorldSubsystem::SaveAllAsync()
{
	SCOPE_CYCLE_COUNTER(STAT_MadWorldSaveAll);

	if (bReadOnly)
	{
		return 0;
	}

	struct FSnapshot
	{
		FMadChunkCoord Coord;
		FMadChunkPtr Chunk;
		FMadChunkStorage Storage;
		bool bPlayerModified = false;
	};

	const double Start = FPlatformTime::Seconds();
	const TSharedRef<TArray<FSnapshot>, ESPMode::ThreadSafe> Snapshots = MakeShared<TArray<FSnapshot>, ESPMode::ThreadSafe>();
	TArray<UE::Tasks::FTask> After;
	for (TPair<FMadChunkCoord, FMadChunkPtr>& Pair : LoadedChunks)
	{
		if (!Pair.Value.IsValid() || !Pair.Value->bDirty)
		{
			continue;
		}
		FSnapshot& Snapshot = Snapshots->AddDefaulted_GetRef();
		Snapshot.Coord = Pair.Key;
		Snapshot.Chunk = Pair.Value;
		{
			// Only the game thread writes a loaded chunk, so this waits for nothing
			// but a mesher's read; the copy is of packed storage, not 32^3 voxels.
			FRWScopeLock ChunkLock(Pair.Value->Lock, SLT_ReadOnly);
			Snapshot.Storage = Pair.Value->Storage;
		}
		Snapshot.bPlayerModified = Pair.Value->bPlayerModified;
		Pair.Value->bDirty = false;

		// A previous autosave still writing this chunk goes first.
		if (const UE::Tasks::FTask* Earlier = PendingSaves.Find(Pair.Key); Earlier != nullptr && !Earlier->IsCompleted())
		{
			After.Add(*Earlier);   // duplicates are harmless as prerequisites
		}
	}
	if (Snapshots->Num() == 0)
	{
		return 0;
	}
	const double CopyMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	const UE::Tasks::FTask Save = UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Snapshots, CopyMs]
	{
		const double WriteStart = FPlatformTime::Seconds();
		TSet<FMadRegionCoord> Regions;
		int32 Written = 0;
		for (FSnapshot& Snapshot : *Snapshots)
		{
			// Compacting the copy, not the live chunk, so no palette slot moves under a mesher.
			Snapshot.Storage.Compact();
			FString Error;
			if (!WriteChunkStorage(Snapshot.Coord, Snapshot.Storage, Snapshot.bPlayerModified, Error))
			{
				UE_LOG(LogMadFallVoxel, Error, TEXT("Autosave could not write chunk %s: %s"), *Snapshot.Coord.ToString(), *Error);
				FScopeLock Lock(&PendingLock);
				FailedSaves.Add(Snapshot.Chunk);
				continue;
			}
			Regions.Add(MadFall::ChunkToRegion(Snapshot.Coord));
			++Written;
		}

		// Off the game thread, so each region reaches the disk step by step.
		for (const FMadRegionCoord& RegionCoord : Regions)
		{
			FScopeLock Lock(&RegionLock);
			FString Error;
			TUniquePtr<FMadRegionFile>* Region = OpenRegions.Find(RegionCoord);
			if (Region != nullptr && !(*Region)->Flush(Error))
			{
				UE_LOG(LogMadFallVoxel, Error, TEXT("Autosave could not flush region %s: %s"), *RegionCoord.ToString(), *Error);
			}
		}
		UE_LOG(LogMadFallVoxel, Display,
			TEXT("World autosave: %d chunk(s) copied in %.2f ms on the game thread; %d written to %d region(s) and flushed to disk in %.0f ms on a worker."),
			Snapshots->Num(), CopyMs, Written, Regions.Num(), (FPlatformTime::Seconds() - WriteStart) * 1000.0);
	}, After);

	for (const FSnapshot& Snapshot : *Snapshots)
	{
		PendingSaves.Add(Snapshot.Coord, Save);
	}
	return Snapshots->Num();
}

bool UMadVoxelWorldSubsystem::UnloadChunk(const FMadChunkCoord& Coord, bool bSave, FString& OutError)
{
	if (!LoadedChunks.Contains(Coord))
	{
		OutError = FString::Printf(TEXT("chunk %s is not loaded"), *Coord.ToString());
		return false;
	}

	const FMadChunkPtr Chunk = LoadedChunks.FindRef(Coord);

	// Only chunks that differ from disk are written. Streaming used to save
	// every chunk it unloaded - untouched generated terrain included - and
	// flush the region after each, on the game thread: 33 ms and 112 ms frames
	// measured while walking. A clean chunk regenerates identically.
	if (bSave && !bReadOnly && Chunk.IsValid() && Chunk->bDirty)
	{
		// On a worker. The chunk has left LoadedChunks, so no edit can reach it;
		// the shared pointer keeps it alive; a load of the same coordinate waits
		// for this task (see HasPendingSave). An autosave still writing an older
		// copy of it runs first - chained, not waited for, since waiting here
		// would put the whole autosave's write on this frame.
		TArray<UE::Tasks::FTask> After;
		if (const UE::Tasks::FTask* Earlier = PendingSaves.Find(Coord); Earlier != nullptr && !Earlier->IsCompleted())
		{
			After.Add(*Earlier);
		}
		const UE::Tasks::FTask Save = UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Coord, Chunk]
		{
			FRWScopeLock ChunkLock(Chunk->Lock, SLT_Write);
			Chunk->Storage.Compact();

			FString Error;
			if (!WriteChunkStorage(Coord, Chunk->Storage, Chunk->bPlayerModified, Error))
			{
				UE_LOG(LogMadFallVoxel, Error, TEXT("Failed to save unloaded chunk %s: %s"), *Coord.ToString(), *Error);
				return;
			}
			Chunk->bDirty = false;

			FScopeLock Lock(&RegionLock);
			if (TUniquePtr<FMadRegionFile>* Region = OpenRegions.Find(MadFall::ChunkToRegion(Coord)))
			{
				if (!(*Region)->Flush(Error))
				{
					UE_LOG(LogMadFallVoxel, Error, TEXT("Failed to flush region after saving %s: %s"), *Coord.ToString(), *Error);
				}
			}
		}, After);
		PendingSaves.Add(Coord, Save);
	}

	LoadedChunks.Remove(Coord);
	ChunkUnloadedDelegate.Broadcast(Coord);
	return true;
}

bool UMadVoxelWorldSubsystem::HasPendingSave(const FMadChunkCoord& Coord)
{
	if (const UE::Tasks::FTask* Save = PendingSaves.Find(Coord))
	{
		if (!Save->IsCompleted())
		{
			return true;
		}
		PendingSaves.Remove(Coord);
	}
	return false;
}

void UMadVoxelWorldSubsystem::WaitForPendingSave(const FMadChunkCoord& Coord)
{
	if (const UE::Tasks::FTask* Save = PendingSaves.Find(Coord))
	{
		Save->Wait();
		PendingSaves.Remove(Coord);
	}
}

void UMadVoxelWorldSubsystem::WaitForAllPendingSaves()
{
	for (const TPair<FMadChunkCoord, UE::Tasks::FTask>& Pair : PendingSaves)
	{
		Pair.Value.Wait();
	}
	PendingSaves.Reset();
}

// ===========================================================================
// Voxel access
// ===========================================================================

FMadChunkPtr UMadVoxelWorldSubsystem::FindChunk(const FMadChunkCoord& Coord) const
{
	const FMadChunkPtr* Found = LoadedChunks.Find(Coord);
	return Found ? *Found : FMadChunkPtr();
}

int32 UMadVoxelWorldSubsystem::NumLoadedChunks() const
{
	return LoadedChunks.Num();
}

void UMadVoxelWorldSubsystem::GetLoadedChunkCoords(TArray<FMadChunkCoord>& OutCoords) const
{
	OutCoords.Reset(LoadedChunks.Num());
	LoadedChunks.GenerateKeyArray(OutCoords);
}

int32 UMadVoxelWorldSubsystem::LoadArea(const FMadChunkCoord& Centre, int32 RadiusInChunks,
	int32 VerticalRadius, FString& OutError)
{
	const double Start = FPlatformTime::Seconds();

	int32 Loaded = 0;
	int32 Failed = 0;

	for (int32 dz = -VerticalRadius; dz <= VerticalRadius; ++dz)
	{
		for (int32 dy = -RadiusInChunks; dy <= RadiusInChunks; ++dy)
		{
			for (int32 dx = -RadiusInChunks; dx <= RadiusInChunks; ++dx)
			{
				const FMadChunkCoord Coord(Centre.X + dx, Centre.Y + dy, Centre.Z + dz);
				if (!Coord.IsValidZ())
				{
					continue;
				}

				FString Error;
				if (LoadChunkSync(Coord, Error))
				{
					++Loaded;
				}
				else
				{
					++Failed;
					OutError = Error;
				}
			}
		}
	}

	const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;

	UE_LOG(LogMadFallVoxel, Log,
		TEXT("Loaded %d chunk(s) around %s in %.1f ms (%.2f ms each), %d failed."),
		Loaded, *Centre.ToString(), Milliseconds, Milliseconds / FMath::Max(Loaded, 1), Failed);

	return Loaded;
}

int32 UMadVoxelWorldSubsystem::NumPendingLoads() const
{
	return InFlightLoads.Num();
}

int64 UMadVoxelWorldSubsystem::GetLoadedVoxelBytes() const
{
	int64 Total = 0;
	for (const TPair<FMadChunkCoord, FMadChunkPtr>& Pair : LoadedChunks)
	{
		if (Pair.Value.IsValid())
		{
			FRWScopeLock Lock(Pair.Value->Lock, SLT_ReadOnly);
			Total += Pair.Value->Storage.GetAllocatedSize();
		}
	}
	return Total;
}

FMadVoxel UMadVoxelWorldSubsystem::GetVoxel(int32 WorldX, int32 WorldY, int32 WorldZ) const
{
	if (!MadFall::IsValidWorldZ(WorldZ))
	{
		return FMadVoxel::Air();
	}

	const FMadChunkCoord Coord = MadFall::WorldToChunk(WorldX, WorldY, WorldZ);
	const FMadChunkPtr Chunk = FindChunk(Coord);
	if (!Chunk.IsValid())
	{
		return FMadVoxel::Air();
	}

	int32 LocalX, LocalY, LocalZ;
	MadFall::WorldToLocal(WorldX, WorldY, WorldZ, LocalX, LocalY, LocalZ);

	FRWScopeLock Lock(Chunk->Lock, SLT_ReadOnly);
	return Chunk->Storage.GetVoxel(LocalX, LocalY, LocalZ);
}

bool UMadVoxelWorldSubsystem::IsVoxelLoaded(int32 WorldX, int32 WorldY, int32 WorldZ) const
{
	return MadFall::IsValidWorldZ(WorldZ) && LoadedChunks.Contains(MadFall::WorldToChunk(WorldX, WorldY, WorldZ));
}

bool UMadVoxelWorldSubsystem::SetVoxel(int32 WorldX, int32 WorldY, int32 WorldZ, const FMadVoxel& Voxel)
{
	if (!MadFall::IsValidWorldZ(WorldZ))
	{
		return false;
	}

	const FMadChunkCoord Coord = MadFall::WorldToChunk(WorldX, WorldY, WorldZ);
	const FMadChunkPtr Chunk = FindChunk(Coord);
	if (!Chunk.IsValid())
	{
		return false;
	}

	int32 LocalX, LocalY, LocalZ;
	MadFall::WorldToLocal(WorldX, WorldY, WorldZ, LocalX, LocalY, LocalZ);

	FMadVoxel Written = Voxel;
	Written.SetFlag(EMadVoxelFlags::PlayerModified, true);

	FMadVoxel Before;
	{
		FRWScopeLock Lock(Chunk->Lock, SLT_Write);
		Before = Chunk->Storage.GetVoxel(LocalX, LocalY, LocalZ);
		Chunk->Storage.SetVoxel(LocalX, LocalY, LocalZ, Written);
		Chunk->bDirty = true;
		Chunk->bPlayerModified = true;
	}

	// Every downstream system - meshing, the structural solver, replication -
	// hangs off this one broadcast rather than polling for changes.
	VoxelChangedDelegate.Broadcast(FIntVector(WorldX, WorldY, WorldZ), Before, Written);
	ChunkChangedDelegate.Broadcast(Coord);
	return true;
}

FMadVoxelState UMadVoxelWorldSubsystem::GetVoxelState(int32 WorldX, int32 WorldY, int32 WorldZ) const
{
	const FMadVoxel Voxel = GetVoxel(WorldX, WorldY, WorldZ);

	FMadVoxelState State;
	State.BlockId = GetBlockRegistry().GetStringId(Voxel.BlockTypeID);
	State.Density = Voxel.Density;
	State.Damage = Voxel.Damage;
	State.Orientation = Voxel.GetOrientation();
	State.ShapeVariant = Voxel.GetShapeVariant();
	State.bCubic = Voxel.HasFlag(EMadVoxelFlags::Cubic);
	State.bSolid = Voxel.IsSolid();

	return State;
}

bool UMadVoxelWorldSubsystem::SetVoxelState(int32 WorldX, int32 WorldY, int32 WorldZ, const FMadVoxelState& State)
{
	FMadVoxel Voxel;
	Voxel.BlockTypeID = GetBlockRegistry().ResolveRuntimeId(State.BlockId);
	Voxel.Density = static_cast<uint8>(FMath::Clamp(State.Density, 0, 255));
	Voxel.Damage = static_cast<uint8>(FMath::Clamp(State.Damage, 0, 255));
	Voxel.Rotation = 0;
	Voxel.SetOrientation(static_cast<uint8>(FMath::Clamp(State.Orientation, 0, 23)));
	Voxel.SetShapeVariant(static_cast<uint8>(FMath::Clamp(State.ShapeVariant, 0, 7)));
	Voxel.Flags = 0;
	Voxel.SetFlag(EMadVoxelFlags::Cubic, State.bCubic);

	return SetVoxel(WorldX, WorldY, WorldZ, Voxel);
}

bool UMadVoxelWorldSubsystem::FillChunk(const FMadChunkCoord& Coord, const FMadVoxel& Voxel)
{
	const FMadChunkPtr Chunk = FindChunk(Coord);
	if (!Chunk.IsValid())
	{
		return false;
	}

	FRWScopeLock Lock(Chunk->Lock, SLT_Write);
	Chunk->Storage.Fill(Voxel);
	Chunk->bDirty = true;
	Chunk->bPlayerModified = true;

	return true;
}

// ===========================================================================
// Diagnostics
// ===========================================================================

bool UMadVoxelWorldSubsystem::SnapshotChunkWithMargin(const FMadChunkCoord& Coord, FMadChunkSampleGrid& OutGrid) const
{
	FSnapshotSources Sources;
	if (!GatherSnapshotSources(Coord, Sources))
	{
		return false;
	}
	SnapshotFromSources(Sources, OutGrid);
	return true;
}

bool UMadVoxelWorldSubsystem::GatherSnapshotSources(const FMadChunkCoord& Coord, FSnapshotSources& OutSources) const
{
	OutSources.Coord = Coord;

	// Cache the chunks the margin can come from, so the 34^3 walk does a map
	// lookup 27 times instead of 39304 times.
	for (int32 dz = -1; dz <= 1; ++dz)
	{
		for (int32 dy = -1; dy <= 1; ++dy)
		{
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				OutSources.Chunks[dx + 1][dy + 1][dz + 1] =
					FindChunk(FMadChunkCoord(Coord.X + dx, Coord.Y + dy, Coord.Z + dz));
			}
		}
	}
	return OutSources.Chunks[1][1][1].IsValid();
}

void UMadVoxelWorldSubsystem::SnapshotFromSources(const FSnapshotSources& Sources, FMadChunkSampleGrid& OutGrid)
{
	OutGrid.Coord = Sources.Coord;
	const auto& Neighbours = Sources.Chunks;

	// One read lock per chunk for the whole snapshot, rather than per sample.
	TArray<FMadChunkPtr, TInlineAllocator<27>> Locked;
	for (int32 dz = 0; dz < 3; ++dz)
	{
		for (int32 dy = 0; dy < 3; ++dy)
		{
			for (int32 dx = 0; dx < 3; ++dx)
			{
				if (Neighbours[dx][dy][dz].IsValid())
				{
					Neighbours[dx][dy][dz]->Lock.ReadLock();
					Locked.Add(Neighbours[dx][dy][dz]);
				}
			}
		}
	}

	ON_SCOPE_EXIT
	{
		for (const FMadChunkPtr& Chunk : Locked)
		{
			Chunk->Lock.ReadUnlock();
		}
	};

	// The centre chunk's damage, so the mesher can crack a damaged face. Copied
	// whole rather than sampled: it is a sparse map, usually empty, and a face is
	// drawn by the chunk that owns the voxel, so margins never need theirs.
	OutGrid.Damage.Reset();
	if (const FMadChunkPtr& Centre = Neighbours[1][1][1]; Centre.IsValid())
	{
		OutGrid.Damage = Centre->Storage.GetDamageMap();
	}

	using MadFall::ChunkSize;

	for (int32 Z = -1; Z <= ChunkSize; ++Z)
	{
		const int32 nz = (Z < 0) ? 0 : (Z >= ChunkSize ? 2 : 1);
		const int32 lz = MadFall::FloorMod(Z, ChunkSize);

		for (int32 Y = -1; Y <= ChunkSize; ++Y)
		{
			const int32 ny = (Y < 0) ? 0 : (Y >= ChunkSize ? 2 : 1);
			const int32 ly = MadFall::FloorMod(Y, ChunkSize);

			for (int32 X = -1; X <= ChunkSize; ++X)
			{
				const int32 nx = (X < 0) ? 0 : (X >= ChunkSize ? 2 : 1);
				const int32 lx = MadFall::FloorMod(X, ChunkSize);

				const FMadChunkPtr& Source = Neighbours[nx][ny][nz];
				const int32 GridIndex = FMadChunkSampleGrid::Index(X, Y, Z);

				if (!Source.IsValid())
				{
					// An unloaded neighbour reads as air. That produces a
					// surface at the edge of the loaded world rather than a
					// hole, which is the less alarming of the two failures.
					OutGrid.Density[GridIndex] = 0;
					OutGrid.BlockId[GridIndex] = MadFall::BlockTypeAir;
					OutGrid.Flags[GridIndex] = 0;
					continue;
				}

				Source->Storage.GetMeshSample(MadFall::VoxelIndex(lx, ly, lz),
					OutGrid.BlockId[GridIndex], OutGrid.Density[GridIndex], OutGrid.Flags[GridIndex]);
			}
		}
	}
}

void UMadVoxelWorldSubsystem::SnapshotLodFromSources(const FSnapshotSources& Sources, int32 Stride, FMadLodSampleGrid& OutGrid)
{
	OutGrid.Coord = Sources.Coord;
	OutGrid.Init(Stride);
	const auto& Neighbours = Sources.Chunks;

	TArray<FMadChunkPtr, TInlineAllocator<27>> Locked;
	for (int32 dz = 0; dz < 3; ++dz)
	{
		for (int32 dy = 0; dy < 3; ++dy)
		{
			for (int32 dx = 0; dx < 3; ++dx)
			{
				if (Neighbours[dx][dy][dz].IsValid())
				{
					Neighbours[dx][dy][dz]->Lock.ReadLock();
					Locked.Add(Neighbours[dx][dy][dz]);
				}
			}
		}
	}
	ON_SCOPE_EXIT
	{
		for (const FMadChunkPtr& Chunk : Locked)
		{
			Chunk->Lock.ReadUnlock();
		}
	};

	using MadFall::ChunkSize;
	OutGrid.Fill([&Neighbours](int32 X, int32 Y, int32 Z, uint16& OutBlock, uint8& OutDensity, uint8& OutFlags)
	{
		const int32 nx = (X < 0) ? 0 : (X >= ChunkSize ? 2 : 1);
		const int32 ny = (Y < 0) ? 0 : (Y >= ChunkSize ? 2 : 1);
		const int32 nz = (Z < 0) ? 0 : (Z >= ChunkSize ? 2 : 1);
		const FMadChunkPtr& Source = Neighbours[nx][ny][nz];
		if (!Source.IsValid())
		{
			// Unloaded reads as air, as in the full snapshot.
			OutBlock = MadFall::BlockTypeAir;
			OutDensity = 0;
			OutFlags = 0;
			return;
		}
		Source->Storage.GetMeshSample(
			MadFall::VoxelIndex(MadFall::FloorMod(X, ChunkSize), MadFall::FloorMod(Y, ChunkSize), MadFall::FloorMod(Z, ChunkSize)),
			OutBlock, OutDensity, OutFlags);
	});
}

FString UMadVoxelWorldSubsystem::DescribeWorld() const
{
	TStringBuilder<1024> Builder;

	Builder.Appendf(TEXT("MadFall world\n"));
	Builder.Appendf(TEXT("  directory:      %s\n"), *WorldDirectory);
	Builder.Appendf(TEXT("  seed:           %llu\n"), WorldSeed);
	Builder.Appendf(TEXT("  loaded chunks:  %d (%d loading)\n"), LoadedChunks.Num(), InFlightLoads.Num());
	Builder.Appendf(TEXT("  voxel memory:   %.2f MiB\n"), GetLoadedVoxelBytes() / (1024.0 * 1024.0));

	int32 Dirty = 0;
	for (const TPair<FMadChunkCoord, FMadChunkPtr>& Pair : LoadedChunks)
	{
		if (Pair.Value.IsValid() && Pair.Value->bDirty)
		{
			++Dirty;
		}
	}
	Builder.Appendf(TEXT("  unsaved chunks: %d\n"), Dirty);

	{
		FScopeLock Lock(&RegionLock);
		Builder.Appendf(TEXT("  open regions:   %d\n"), OpenRegions.Num());
	}

	Builder.Appendf(TEXT("  blocks:         %d definitions, %d unresolved\n"),
		GetBlockRegistry().Num(), GetBlockRegistry().NumUnresolved());

	return Builder.ToString();
}

FString UMadVoxelWorldSubsystem::DescribeChunk(const FMadChunkCoord& Coord) const
{
	const FMadChunkPtr Chunk = FindChunk(Coord);
	if (!Chunk.IsValid())
	{
		return FString::Printf(TEXT("Chunk %s is not loaded."), *Coord.ToString());
	}

	FRWScopeLock Lock(Chunk->Lock, SLT_ReadOnly);
	const FMadChunkStorage& Storage = Chunk->Storage;

	TStringBuilder<1024> Builder;
	Builder.Appendf(TEXT("Chunk %s  region=%s slot=%d\n"),
		*Coord.ToString(),
		*MadFall::ChunkToRegion(Coord).ToString(),
		MadFall::ChunkToRegionSlot(Coord));
	Builder.Appendf(TEXT("  uniform:        %s\n"), Storage.IsUniform() ? TEXT("yes") : TEXT("no"));
	Builder.Appendf(TEXT("  block types:    %d (palette %d, %d bits/voxel)\n"),
		Storage.GetDistinctBlockCount(),
		Storage.GetPalette().Num(),
		Storage.GetIndices().IsEmpty() ? 0 : Storage.GetIndices().GetBitsPerValue());
	Builder.Appendf(TEXT("  damaged voxels: %d\n"), Storage.GetDamageMap().Num());
	Builder.Appendf(TEXT("  memory:         %lld bytes\n"), Storage.GetAllocatedSize());
	Builder.Appendf(TEXT("  dirty:          %s\n"), Chunk->bDirty ? TEXT("yes") : TEXT("no"));

	Builder.Appendf(TEXT("  palette:\n"));
	for (const FMadBlockPaletteEntry& Entry : Storage.GetPalette())
	{
		Builder.Appendf(TEXT("    %-32s x%u\n"),
			*GetBlockRegistry().GetStringId(Entry.RuntimeId).ToString(), Entry.RefCount);
	}

	return Builder.ToString();
}

FString UMadVoxelWorldSubsystem::DescribeRegion(const FMadRegionCoord& Coord)
{
	FScopeLock Lock(&RegionLock);

	FString Error;
	FMadRegionFile* Region = GetOrOpenRegion(Coord, Error);
	if (Region == nullptr)
	{
		return FString::Printf(TEXT("Could not open region %s: %s"), *Coord.ToString(), *Error);
	}

	return Region->Describe();
}

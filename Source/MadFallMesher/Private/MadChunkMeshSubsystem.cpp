// Copyright MadFall. All Rights Reserved.

#include "MadChunkMeshSubsystem.h"

#include "MadFrameBudget.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "MadBlockRegistry.h"
#include "MadChunkMeshComponent.h"
#include "MadChunkSampleGrid.h"
#include "MadFallMesher.h"
#include "MadFallStats.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Misc/StringBuilder.h"
#include "Tasks/Task.h"
#include "UObject/ConstructorHelpers.h"

DECLARE_CYCLE_STAT(TEXT("Mesh Snapshot (game thread)"), STAT_MadMeshSnapshot, STATGROUP_MadFallMesher);
DECLARE_CYCLE_STAT(TEXT("Mesh Publish (game thread)"), STAT_MadMeshPublish, STATGROUP_MadFallMesher);

namespace
{
	/**
	 * A cap only. Applying a rebuilt chunk costs 0.2 ms for a quiet chunk and
	 * over 2 ms for a busy one, so the time budget below decides how many fit;
	 * this bounds the count when many trivial meshes land at once.
	 */
	TAutoConsoleVariable<int32> CVarMaxAppliesPerFrame(
		TEXT("mad.mesh.MaxAppliesPerFrame"),
		8,
		TEXT("Upper bound on finished chunk meshes pushed to components in one frame; mad.mesh.PublishBudgetMs usually stops sooner."),
		ECVF_Default);

	/** Snapshotting is a ~157 KB copy under read locks; four is ~0.2 ms. */
	TAutoConsoleVariable<int32> CVarMaxJobLaunchesPerFrame(
		TEXT("mad.mesh.MaxJobLaunchesPerFrame"),
		4,
		TEXT("How many chunk sample-grid snapshots may be taken in one frame."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarMaxJobsInFlight(
		TEXT("mad.mesh.MaxJobsInFlight"),
		16,
		TEXT("Upper bound on concurrent chunk meshing tasks."),
		ECVF_Default);

	TAutoConsoleVariable<FString> CVarSectionMaterial(
		TEXT("mad.mesh.SectionMaterial"),
		TEXT("/Game/Materials/M_MadVoxel.M_MadVoxel"),
		TEXT("Material applied to every generated chunk section."),
		ECVF_Default);

	/**
	 * Game-thread milliseconds for publishing meshes and for launching jobs.
	 * Together with the rest of the frame's MadFall work they have to fit the
	 * 2 ms rule, so each gets about half of it.
	 */
	TAutoConsoleVariable<float> CVarPublishBudgetMs(
		TEXT("mad.mesh.PublishBudgetMs"),
		1.0f,
		TEXT("Game-thread milliseconds per frame for applying finished chunk meshes (at least one is applied)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarLaunchBudgetMs(
		TEXT("mad.mesh.LaunchBudgetMs"),
		0.5f,
		TEXT("Game-thread milliseconds per frame for snapshotting chunks into meshing jobs (at least one is launched)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarComponentPoolSize(
		TEXT("mad.mesh.ComponentPoolSize"),
		256,
		TEXT("Released chunk mesh components kept registered for reuse."),
		ECVF_Default);

	const TCHAR* MeshActorName = TEXT("MadFallChunkMeshes");
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void UMadChunkMeshSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Declaring the dependency makes the voxel world exist before this
	// subsystem's Initialize runs, so the delegate hookup below cannot race it.
	Collection.InitializeDependency<UMadVoxelWorldSubsystem>();

	// Load surfaces here, on the game thread, so no meshing worker is ever the
	// first caller (it would take the load lock and read patch files mid-job).
	MadFall::GetSurfaces();

	// Load every surface's material now, while the world is initialising,
	// rather than inside the first chunk apply that needs it - that first apply
	// was measured at 5.5 ms, the worst game-thread frame of a whole session.
	GetSectionMaterial();
	for (const FMadSurfaceDefinition& Surface : MadFall::GetSurfaces().GetAll())
	{
		GetMaterialForClass(Surface.Id);
	}

	if (UMadVoxelWorldSubsystem* VoxelWorld = GetVoxelWorld())
	{
		VoxelWorld->OnChunkChanged().AddUObject(this, &UMadChunkMeshSubsystem::HandleChunkChanged);
		VoxelWorld->OnChunkUnloaded().AddUObject(this, &UMadChunkMeshSubsystem::HandleChunkUnloaded);
	}

	UE_LOG(LogMadFallMesher, Log, TEXT("Chunk mesh subsystem ready."));
}

void UMadChunkMeshSubsystem::Deinitialize()
{
	if (UMadVoxelWorldSubsystem* VoxelWorld = GetVoxelWorld())
	{
		VoxelWorld->OnChunkChanged().RemoveAll(this);
		VoxelWorld->OnChunkUnloaded().RemoveAll(this);
	}

	Components.Reset();
	ComponentPool.Reset();
	DirtyChunks.Reset();
	InFlight.Reset();

	{
		FScopeLock Lock(&CompletedLock);
		Completed.Reset();
	}

	Super::Deinitialize();
}

TStatId UMadChunkMeshSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadChunkMeshSubsystem, STATGROUP_Tickables);
}

UMadVoxelWorldSubsystem* UMadChunkMeshSubsystem::GetVoxelWorld() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
}

// ===========================================================================
// Change handling
// ===========================================================================

void UMadChunkMeshSubsystem::HandleChunkChanged(const FMadChunkCoord& Coord)
{
	MarkChunkDirty(Coord);

	// Editing a voxel on a chunk boundary changes what its neighbour's margin
	// samples say, so the neighbour's surface moves too. Skipping this is how
	// voxel games end up with cracks along chunk seams that only appear after
	// the player digs there.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (int32 Step = -1; Step <= 1; Step += 2)
		{
			FMadChunkCoord Neighbour = Coord;
			if (Axis == 0) { Neighbour.X += Step; }
			else if (Axis == 1) { Neighbour.Y += Step; }
			else { Neighbour.Z += Step; }

			if (Neighbour.IsValidZ())
			{
				MarkChunkDirty(Neighbour);
			}
		}
	}
}

void UMadChunkMeshSubsystem::HandleChunkUnloaded(const FMadChunkCoord& Coord)
{
	ReleaseChunk(Coord);
}

void UMadChunkMeshSubsystem::MarkChunkDirty(const FMadChunkCoord& Coord)
{
	if (!Coord.IsValidZ())
	{
		return;
	}

	DirtyChunks.Add(Coord);
}

void UMadChunkMeshSubsystem::ReleaseChunk(const FMadChunkCoord& Coord)
{
	DirtyChunks.Remove(Coord);

	if (TObjectPtr<UMadChunkMeshComponent>* Found = Components.Find(Coord))
	{
		if (UMadChunkMeshComponent* Component = Found->Get())
		{
			// Keep it registered and empty for the next chunk that needs one:
			// an empty procedural mesh renders and collides with nothing.
			if (ComponentPool.Num() < CVarComponentPoolSize.GetValueOnGameThread())
			{
				Component->ClearAllMeshSections();
				ComponentPool.Add(Component);
			}
			else
			{
				Component->DestroyComponent();
			}
		}
		Components.Remove(Coord);
	}
}

int32 UMadChunkMeshSubsystem::RebuildAll()
{
	UMadVoxelWorldSubsystem* VoxelWorld = GetVoxelWorld();
	if (VoxelWorld == nullptr)
	{
		return 0;
	}

	// Every LOADED chunk, not just the ones that already have a component: a
	// chunk generated while the mesher was throttled may never have been meshed
	// at all, and "rebuild all" that skips those is a confusing tool.
	TArray<FMadChunkCoord> Loaded;
	VoxelWorld->GetLoadedChunkCoords(Loaded);

	for (const FMadChunkCoord& Coord : Loaded)
	{
		MarkChunkDirty(Coord);
	}

	return Loaded.Num();
}

int32 UMadChunkMeshSubsystem::RebuildAllNow()
{
	UMadVoxelWorldSubsystem* VoxelWorld = GetVoxelWorld();
	if (VoxelWorld == nullptr)
	{
		return 0;
	}

	TArray<FMadChunkCoord> Loaded;
	VoxelWorld->GetLoadedChunkCoords(Loaded);

	// Deterministic order so two captures of the same world look identical.
	Loaded.Sort([](const FMadChunkCoord& A, const FMadChunkCoord& B)
	{
		if (A.Z != B.Z) { return A.Z < B.Z; }
		if (A.Y != B.Y) { return A.Y < B.Y; }
		return A.X < B.X;
	});

	const double Start = FPlatformTime::Seconds();
	int32 Built = 0;

	for (const FMadChunkCoord& Coord : Loaded)
	{
		FString Error;
		if (RebuildChunkNow(Coord, Error))
		{
			++Built;
			DirtyChunks.Remove(Coord);
		}
		else
		{
			UE_LOG(LogMadFallMesher, Warning, TEXT("Could not mesh chunk %s: %s"), *Coord.ToString(), *Error);
		}
	}

	const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
	UE_LOG(LogMadFallMesher, Display,
		TEXT("Meshed %d chunk(s) synchronously in %.0f ms (%.2f ms each)."),
		Built, Milliseconds, Milliseconds / FMath::Max(Built, 1));

	return Built;
}

// ===========================================================================
// Tick
// ===========================================================================

void UMadChunkMeshSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Meshing);

	const double FrameStart = FPlatformTime::Seconds();
	const int32 RebuildsBefore = TotalRebuilds;
	PublishCompletedMeshes();
	const double PublishMs = (FPlatformTime::Seconds() - FrameStart) * 1000.0;
	WorstPublishMilliseconds = FMath::Max(WorstPublishMilliseconds, PublishMs);

	ON_SCOPE_EXIT
	{
		const double FrameMs = (FPlatformTime::Seconds() - FrameStart) * 1000.0;
		WorstLaunchMilliseconds = FMath::Max(WorstLaunchMilliseconds, FrameMs - PublishMs);
		WorstFrameMilliseconds = FMath::Max(WorstFrameMilliseconds, FrameMs);
		if (TotalRebuilds != RebuildsBefore || FrameMs > 0.05)
		{
			++FramesWithWork;
			FramesOverBudget += FrameMs > 2.0 ? 1 : 0;
		}
	};

	const int32 MaxLaunches = FMath::Max(CVarMaxJobLaunchesPerFrame.GetValueOnGameThread(), 1);
	const int32 MaxInFlight = FMath::Max(CVarMaxJobsInFlight.GetValueOnGameThread(), 1);

	int32 Launched = 0;

	// Iterate a copy of the keys: LaunchMeshJob removes from DirtyChunks.
	TArray<FMadChunkCoord> Candidates = DirtyChunks.Array();

	for (const FMadChunkCoord& Coord : Candidates)
	{
		if (Launched >= MaxLaunches || InFlight.Num() >= MaxInFlight)
		{
			break;
		}
		if (Launched > 0 && (FPlatformTime::Seconds() - FrameStart) * 1000.0 - PublishMs >= CVarLaunchBudgetMs.GetValueOnGameThread())
		{
			break;
		}

		if (InFlight.Contains(Coord))
		{
			// Already meshing. Leave it dirty so it is rebuilt again once the
			// current job lands - the edit that dirtied it is newer than the
			// snapshot the job is working from.
			continue;
		}

		DirtyChunks.Remove(Coord);
		LaunchMeshJob(Coord);
		++Launched;
	}
}

void UMadChunkMeshSubsystem::LaunchMeshJob(const FMadChunkCoord& Coord)
{
	UMadVoxelWorldSubsystem* VoxelWorld = GetVoxelWorld();
	if (VoxelWorld == nullptr)
	{
		return;
	}

	// Game thread: only the 27 chunk pointers. The copy and the grid allocation
	// (156 KiB) happen on the worker; measured on the game thread they were
	// up to 1.1 ms per launch.
	TSharedPtr<UMadVoxelWorldSubsystem::FSnapshotSources, ESPMode::ThreadSafe> Sources =
		MakeShared<UMadVoxelWorldSubsystem::FSnapshotSources, ESPMode::ThreadSafe>();

	const double SnapshotStart = FPlatformTime::Seconds();
	{
		SCOPE_CYCLE_COUNTER(STAT_MadMeshSnapshot);
		if (!VoxelWorld->GatherSnapshotSources(Coord, *Sources))
		{
			// The chunk unloaded between being marked dirty and being picked up.
			return;
		}
	}
	WorstSnapshotMilliseconds = FMath::Max(WorstSnapshotMilliseconds,
		(FPlatformTime::Seconds() - SnapshotStart) * 1000.0);

	InFlight.Add(Coord);

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Sources]
	{
		TSharedPtr<FMadChunkSampleGrid, ESPMode::ThreadSafe> Grid = MakeShared<FMadChunkSampleGrid, ESPMode::ThreadSafe>();
		UMadVoxelWorldSubsystem::SnapshotFromSources(*Sources, *Grid);

		FMadChunkMeshPtr Mesh = MakeShared<FMadChunkMesh, ESPMode::ThreadSafe>();

		// The registry is immutable after load, so reading it from a worker
		// needs no lock. The sample grid is this job's private copy.
		MadFall::ChunkMesher::FMeshSettings Settings;
		MadFall::ChunkMesher::BuildChunkMesh(
			*Grid, UMadVoxelWorldSubsystem::GetBlockRegistry(), Settings, *Mesh);

		FScopeLock Lock(&CompletedLock);
		Completed.Add(Mesh);
	});
}

void UMadChunkMeshSubsystem::PublishCompletedMeshes()
{
	SCOPE_CYCLE_COUNTER(STAT_MadMeshPublish);

	// Time-budgeted rather than a fixed count: a quiet chunk applies in 0.2 ms
	// and a busy one in over 2, so "two per frame" was either wasteful or over
	// budget. At least one mesh is handled per frame so meshing always advances.
	const int32 MaxApplies = FMath::Max(CVarMaxAppliesPerFrame.GetValueOnGameThread(), 1);
	const double Budget = FMath::Max(0.1, static_cast<double>(CVarPublishBudgetMs.GetValueOnGameThread())) / 1000.0;
	const double Start = FPlatformTime::Seconds();
	UMadVoxelWorldSubsystem* VoxelWorld = GetVoxelWorld();

	for (int32 Handled = 0; Handled < MaxApplies; ++Handled)
	{
		if (Handled > 0 && FPlatformTime::Seconds() - Start >= Budget)
		{
			break;
		}

		FMadChunkMeshPtr Mesh;
		{
			FScopeLock Lock(&CompletedLock);
			if (Completed.Num() == 0)
			{
				break;
			}
			Mesh = Completed[0];
			Completed.RemoveAt(0, 1, EAllowShrinking::No);
		}
		if (!Mesh.IsValid())
		{
			continue;
		}

		// The chunk unloaded while its mesh was being built: nothing to show.
		if (VoxelWorld != nullptr && !VoxelWorld->FindChunk(Mesh->Coord).IsValid())
		{
			InFlight.Remove(Mesh->Coord);
			continue;
		}

		if (Mesh->IsEmpty())
		{
			InFlight.Remove(Mesh->Coord);
			++TotalRebuilds;
			// An all-air or all-interior chunk has no surface. Releasing the
			// component rather than leaving an empty one keeps the component
			// count proportional to visible geometry.
			if (Components.Contains(Mesh->Coord))
			{
				ReleaseChunk(Mesh->Coord);
			}
			continue;
		}

		// The first apply of a frame used to go ahead regardless, which is right for
		// meshing's own budget and wrong for the frame's: a 1.7 ms busy chunk on a
		// frame where the solver, the horde and the far terrain already ran broke
		// 2 ms. Estimate the apply from its vertex count and wait for a quieter
		// frame if it will not fit - but never more than a few frames, so a
		// steadily busy frame cannot starve meshing.
		int32 Vertices = 0;
		for (const FMadMeshSection& Section : Mesh->Sections)
		{
			Vertices += Section.NumVertices();
		}
		const double EstimateMs = 0.1 + Vertices * ApplyMsPerVertex;
		const double LeftMs = MadFall::FrameBudget::GetRemainingMs(Budget * 1000.0, 0.0) - (FPlatformTime::Seconds() - Start) * 1000.0;
		if (EstimateMs > LeftMs && StarvedFrames < MaxStarvedFrames)
		{
			FScopeLock Lock(&CompletedLock);
			Completed.Insert(Mesh, 0);
			++DeferredApplies;
			++StarvedFrames;
			break;
		}

		const bool bNeedsComponent = !Components.Contains(Mesh->Coord);
		UMadChunkMeshComponent* Component = GetOrCreateComponent(Mesh->Coord);
		if (Component == nullptr)
		{
			InFlight.Remove(Mesh->Coord);
			continue;
		}

		// Creating the component used this frame's budget: apply next frame
		// rather than stacking a multi-millisecond apply on top of it.
		if (bNeedsComponent && FPlatformTime::Seconds() - Start >= Budget)
		{
			FScopeLock Lock(&CompletedLock);
			Completed.Insert(Mesh, 0);
			++DeferredApplies;
			break;
		}

		InFlight.Remove(Mesh->Coord);
		++TotalRebuilds;
		TotalBuildMilliseconds += Mesh->BuildMilliseconds;
		WorstBuildMilliseconds = FMath::Max(WorstBuildMilliseconds, Mesh->BuildMilliseconds);

		const double ApplyStart = FPlatformTime::Seconds();
		Component->ApplyChunkMesh(*Mesh, [this](FName MaterialClass) { return GetMaterialForClass(MaterialClass); });
		const double ApplyMs = (FPlatformTime::Seconds() - ApplyStart) * 1000.0;
		WorstApplyMilliseconds = FMath::Max(WorstApplyMilliseconds, ApplyMs);
		if (Vertices > 0)
		{
			ApplyMsPerVertex = FMath::Lerp(ApplyMsPerVertex, FMath::Max(0.0, ApplyMs - 0.1) / Vertices, 0.1);
		}
		StarvedFrames = 0;
	}
}

// ===========================================================================
// Components
// ===========================================================================

AActor* UMadChunkMeshSubsystem::GetOrCreateMeshActor()
{
	if (MeshActor != nullptr)
	{
		return MeshActor;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	// One actor for the whole voxel world rather than one per chunk: an actor
	// per chunk would put thousands of them in the scene, each with its own
	// transform update and registration cost, for no benefit - chunk components
	// never move independently.
	FActorSpawnParameters Params;
	Params.Name = FName(MeshActorName);
	Params.ObjectFlags |= RF_Transient;

	MeshActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (MeshActor != nullptr)
	{
#if WITH_EDITOR
		// Editor-only: the outliner label does not exist in a game build, and
		// calling it unguarded is how runtime code quietly stops compiling for
		// anything but the editor.
		MeshActor->SetActorLabel(MeshActorName);
#endif
		USceneComponent* Root = NewObject<USceneComponent>(MeshActor, TEXT("Root"));
		Root->RegisterComponent();
		MeshActor->SetRootComponent(Root);
	}

	return MeshActor;
}

UMadChunkMeshComponent* UMadChunkMeshSubsystem::GetOrCreateComponent(const FMadChunkCoord& Coord)
{
	if (TObjectPtr<UMadChunkMeshComponent>* Found = Components.Find(Coord))
	{
		if (UMadChunkMeshComponent* Existing = Found->Get())
		{
			return Existing;
		}
		Components.Remove(Coord);
	}

	AActor* Actor = GetOrCreateMeshActor();
	if (Actor == nullptr)
	{
		return nullptr;
	}

	const double RegisterStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT { WorstRegisterMilliseconds = FMath::Max(WorstRegisterMilliseconds, (FPlatformTime::Seconds() - RegisterStart) * 1000.0); };

	UMadChunkMeshComponent* Component = nullptr;
	while (Component == nullptr && ComponentPool.Num() > 0)
	{
		Component = ComponentPool.Pop(EAllowShrinking::No);
		if (Component != nullptr && !Component->IsRegistered())
		{
			Component = nullptr;
		}
	}

	if (Component != nullptr)
	{
		++PoolReuses;
		Component->ChunkCoord = Coord;
	}
	else
	{
		// Pooled components keep their first chunk's name; names are for the
		// outliner, not lookup, so a pooled "Chunk_3_1_0" showing another chunk
		// is cosmetic.
		const FString ComponentName = FString::Printf(TEXT("Chunk_%d_%d_%d"), Coord.X, Coord.Y, Coord.Z);
		Component = NewObject<UMadChunkMeshComponent>(Actor, UMadChunkMeshComponent::StaticClass(), MakeUniqueObjectName(Actor, UMadChunkMeshComponent::StaticClass(), *ComponentName));
		Component->ChunkCoord = Coord;
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->RegisterComponent();
	}

	// The mesher emits chunk-local positions; the component carries the offset.
	const FVector ChunkOrigin(
		static_cast<double>(Coord.X) * MadFall::ChunkSize * MadFall::VoxelSizeUU,
		static_cast<double>(Coord.Y) * MadFall::ChunkSize * MadFall::VoxelSizeUU,
		static_cast<double>(Coord.Z) * MadFall::ChunkSize * MadFall::VoxelSizeUU);

	Component->SetWorldLocation(ChunkOrigin);

	Components.Add(Coord, Component);
	return Component;
}

UMaterialInterface* UMadChunkMeshSubsystem::GetSectionMaterial()
{
	if (SectionMaterial != nullptr)
	{
		return SectionMaterial;
	}

	if (!bTriedLoadingMaterial)
	{
		bTriedLoadingMaterial = true;

		const FString Path = CVarSectionMaterial.GetValueOnGameThread();
		SectionMaterial = LoadObject<UMaterialInterface>(nullptr, *Path);

		if (SectionMaterial == nullptr)
		{
			// Falling back keeps the geometry visible rather than invisible,
			// which matters because "no material" and "no geometry" look
			// identical in a screenshot and are very different bugs.
			UE_LOG(LogMadFallMesher, Warning,
				TEXT("Could not load section material '%s'; falling back to the engine default. ")
				TEXT("Run Scripts/make_voxel_material.py to generate it."), *Path);
			SectionMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		}
	}

	return SectionMaterial;
}

UMaterialInterface* UMadChunkMeshSubsystem::GetMaterialForClass(FName MaterialClass)
{
	if (const TObjectPtr<UMaterialInterface>* Cached = ClassMaterials.Find(MaterialClass))
	{
		return *Cached;
	}

	UMaterialInterface* Material = nullptr;
	const FMadSurfaceDefinition* Surface = MadFall::GetSurfaces().Find(MaterialClass);
	if (Surface != nullptr && !Surface->Material.IsNull())
	{
		// Synchronous, once per class for the session. A surface is a handful of
		// materials, and loading one on first sight costs a frame once rather
		// than showing the wrong material until an async load lands.
		Material = Cast<UMaterialInterface>(Surface->Material.TryLoad());
		if (Material == nullptr)
		{
			UE_LOG(LogMadFallMesher, Warning, TEXT("%s: surface %s names material '%s', which did not load; using the default voxel material."),
				*Surface->SourcePath, *MaterialClass.ToString(), *Surface->Material.ToString());
		}
		else
		{
			UE_LOG(LogMadFallMesher, Display, TEXT("Surface %s renders with %s."), *MaterialClass.ToString(), *Material->GetPathName());
		}
	}
	if (Material == nullptr)
	{
		Material = GetSectionMaterial();
	}

	ClassMaterials.Add(MaterialClass, Material);
	return Material;
}

// ===========================================================================
// Diagnostics
// ===========================================================================

bool UMadChunkMeshSubsystem::RebuildChunkNow(const FMadChunkCoord& Coord, FString& OutError)
{
	UMadVoxelWorldSubsystem* VoxelWorld = GetVoxelWorld();
	if (VoxelWorld == nullptr)
	{
		OutError = TEXT("no voxel world subsystem");
		return false;
	}

	const double SnapshotStart = FPlatformTime::Seconds();
	FMadChunkSampleGrid Grid;
	if (!VoxelWorld->SnapshotChunkWithMargin(Coord, Grid))
	{
		OutError = FString::Printf(TEXT("chunk %s is not loaded"), *Coord.ToString());
		return false;
	}
	const double SnapshotMs = (FPlatformTime::Seconds() - SnapshotStart) * 1000.0;

	FMadChunkMesh Mesh;
	MadFall::ChunkMesher::FMeshSettings Settings;
	MadFall::ChunkMesher::BuildChunkMesh(Grid, UMadVoxelWorldSubsystem::GetBlockRegistry(), Settings, Mesh);

	// The synchronous path has to feed the same counters as the async one, or
	// `mad.mesh.stats` reports 0.000 ms of game-thread cost for work that
	// definitely happened on the game thread.
	WorstSnapshotMilliseconds = FMath::Max(WorstSnapshotMilliseconds, SnapshotMs);

	++TotalRebuilds;
	TotalBuildMilliseconds += Mesh.BuildMilliseconds;
	WorstBuildMilliseconds = FMath::Max(WorstBuildMilliseconds, Mesh.BuildMilliseconds);

	if (Mesh.IsEmpty())
	{
		ReleaseChunk(Coord);
		return true;
	}

	UMadChunkMeshComponent* Component = GetOrCreateComponent(Coord);
	if (Component == nullptr)
	{
		OutError = TEXT("could not create a mesh component");
		return false;
	}

	const double ApplyStart = FPlatformTime::Seconds();
	Component->ApplyChunkMesh(Mesh, [this](FName MaterialClass) { return GetMaterialForClass(MaterialClass); });
	WorstApplyMilliseconds = FMath::Max(WorstApplyMilliseconds,
		(FPlatformTime::Seconds() - ApplyStart) * 1000.0);

	return true;
}

FString UMadChunkMeshSubsystem::InspectChunkMesh(const FMadChunkCoord& Coord, FString& OutError)
{
	UMadVoxelWorldSubsystem* VoxelWorld = GetVoxelWorld();
	if (VoxelWorld == nullptr)
	{
		OutError = TEXT("no voxel world subsystem");
		return FString();
	}

	FMadChunkSampleGrid Grid;
	if (!VoxelWorld->SnapshotChunkWithMargin(Coord, Grid))
	{
		OutError = FString::Printf(TEXT("chunk %s is not loaded"), *Coord.ToString());
		return FString();
	}

	// Count what each mesher will actually see, so "the cubic path produced
	// nothing" can be distinguished from "there were no cubic voxels".
	int32 SolidVoxels = 0;
	int32 CubicVoxels = 0;
	TMap<uint16, int32> BlockCounts;

	for (int32 Z = 0; Z < MadFall::ChunkSize; ++Z)
	{
		for (int32 Y = 0; Y < MadFall::ChunkSize; ++Y)
		{
			for (int32 X = 0; X < MadFall::ChunkSize; ++X)
			{
				if (!Grid.IsSolid(X, Y, Z))
				{
					continue;
				}

				++SolidVoxels;
				if (Grid.IsCubic(X, Y, Z))
				{
					++CubicVoxels;
				}

				++BlockCounts.FindOrAdd(Grid.GetBlockId(X, Y, Z));
			}
		}
	}

	FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();

	TStringBuilder<2048> Builder;
	Builder.Appendf(TEXT("Chunk %s mesh inspection\n"), *Coord.ToString());
	Builder.Appendf(TEXT("  solid voxels: %d  (of which cubic/construction: %d)\n"), SolidVoxels, CubicVoxels);

	Builder.Appendf(TEXT("  block types present:\n"));
	for (const TPair<uint16, int32>& Pair : BlockCounts)
	{
		const FMadBlockDefView View = Registry.GetBlockView(Pair.Key);
		Builder.Appendf(TEXT("    %-28s x%-6d material=%s\n"),
			*Registry.GetStringId(Pair.Key).ToString(), Pair.Value, *View.MaterialClass.ToString());
	}

	MadFall::ChunkMesher::FMeshSettings Settings;

	FMadChunkMesh Both;
	MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, Both);

	Settings.bCubic = false;
	FMadChunkMesh IsoOnly;
	MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, IsoOnly);

	Settings.bCubic = true;
	Settings.bIsosurface = false;
	FMadChunkMesh CubicOnly;
	MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, Settings, CubicOnly);

	Builder.Appendf(TEXT("  isosurface path: %d verts, %d tris, %.2f ms\n"),
		IsoOnly.TotalVertices(), IsoOnly.TotalTriangles(), IsoOnly.BuildMilliseconds);
	Builder.Appendf(TEXT("  cubic path:      %d verts, %d tris, %.2f ms\n"),
		CubicOnly.TotalVertices(), CubicOnly.TotalTriangles(), CubicOnly.BuildMilliseconds);
	Builder.Appendf(TEXT("  combined:        %d verts, %d tris, %d sections, %.2f ms\n"),
		Both.TotalVertices(), Both.TotalTriangles(), Both.Sections.Num(), Both.BuildMilliseconds);

	for (const FMadMeshSection& Section : Both.Sections)
	{
		Builder.Appendf(TEXT("    section %-28s %6d verts %6d tris\n"),
			*Section.MaterialClass.ToString(), Section.NumVertices(), Section.NumTriangles());
	}

	Builder.Appendf(TEXT("  material asset:  %s\n"),
		GetSectionMaterial() ? *GetSectionMaterial()->GetPathName() : TEXT("(none)"));

	return Builder.ToString();
}

FString UMadChunkMeshSubsystem::DescribeStats() const
{
	TStringBuilder<1024> Builder;

	Builder.Appendf(TEXT("MadFall chunk meshing\n"));
	Builder.Appendf(TEXT("  components:        %d\n"), Components.Num());
	Builder.Appendf(TEXT("  dirty:             %d\n"), DirtyChunks.Num());
	Builder.Appendf(TEXT("  jobs in flight:    %d\n"), InFlight.Num());
	Builder.Appendf(TEXT("  rebuilds:          %d\n"), TotalRebuilds);

	if (TotalRebuilds > 0)
	{
		Builder.Appendf(TEXT("  mean build:        %.3f ms (worker thread)\n"),
			TotalBuildMilliseconds / TotalRebuilds);
	}

	Builder.Appendf(TEXT("  worst build:       %.3f ms (worker thread)\n"), WorstBuildMilliseconds);
	Builder.Appendf(TEXT("  worst snapshot:    %.3f ms (GAME THREAD, budget 2.0)\n"), WorstSnapshotMilliseconds);
	Builder.Appendf(TEXT("  worst apply:       %.3f ms (GAME THREAD, budget 2.0)\n"), WorstApplyMilliseconds);
	Builder.Appendf(TEXT("  worst register:    %.3f ms (GAME THREAD, new component)\n"), WorstRegisterMilliseconds);
	Builder.Appendf(TEXT("  component pool:    %d idle, %d reuses, %d applies deferred a frame\n"), ComponentPool.Num(), PoolReuses, DeferredApplies);
	Builder.Appendf(TEXT("  worst publish:     %.3f ms, worst launch %.3f ms\n"), WorstPublishMilliseconds, WorstLaunchMilliseconds);
	Builder.Appendf(TEXT("  worst frame:       %.3f ms (GAME THREAD, budget 2.0); %d of %d frames with work over budget\n"),
		WorstFrameMilliseconds, FramesOverBudget, FramesWithWork);
	Builder.Appendf(TEXT("  section material:  %s\n"),
		SectionMaterial ? *SectionMaterial->GetPathName() : TEXT("(not loaded yet)"));

	int64 GeometryBytes = 0;
	int32 Vertices = 0;
	int32 Triangles = 0;
	for (const TPair<FMadChunkCoord, TObjectPtr<UMadChunkMeshComponent>>& Pair : Components)
	{
		if (const UMadChunkMeshComponent* Component = Pair.Value.Get())
		{
			Vertices += Component->LastVertexCount;
			Triangles += Component->LastTriangleCount;
		}
	}
	(void)GeometryBytes;

	Builder.Appendf(TEXT("  resident geometry: %d verts, %d tris\n"), Vertices, Triangles);

	return Builder.ToString();
}

void UMadChunkMeshSubsystem::ResetStats()
{
	WorstBuildMilliseconds = 0.0;
	WorstApplyMilliseconds = 0.0;
	WorstSnapshotMilliseconds = 0.0;
	WorstFrameMilliseconds = 0.0;
	WorstPublishMilliseconds = 0.0;
	WorstLaunchMilliseconds = 0.0;
	WorstRegisterMilliseconds = 0.0;
	FramesWithWork = 0;
	FramesOverBudget = 0;
	PoolReuses = 0;
	DeferredApplies = 0;
}

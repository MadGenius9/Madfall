// Copyright MadFall. All Rights Reserved.

#include "MadFarTerrain.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadChunkMeshSubsystem.h"
#include "MadFallMesher.h"
#include "MadFrameBudget.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldGenerator.h"
#include "ProceduralMeshComponent.h"

namespace
{
	TAutoConsoleVariable<int32> CVarFarEnabled(
		TEXT("mad.far.Enabled"), 1,
		TEXT("Draw the far terrain beyond the streamed chunks."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarFarRange(
		TEXT("mad.far.Range"), 100,
		TEXT("Percent of each far-terrain level's reach. 100 reaches about 6 km."),
		ECVF_Default);

	constexpr int32 MaxBuildsInFlight = 4;

	/** Frame-budget share for creating tile components. */
	constexpr double CreateShareMs = 0.5;

	FVector2D ViewerVoxels(const UWorld& World)
	{
		const APlayerController* Controller = World.GetFirstPlayerController();
		if (Controller != nullptr && Controller->PlayerCameraManager != nullptr)
		{
			const FVector At = Controller->PlayerCameraManager->GetCameraLocation();
			return FVector2D(At.X, At.Y) / MadFall::VoxelSizeUU;
		}
		return FVector2D::ZeroVector;
	}

	FIntPoint TileOrigin(const FMadFarLevel& Level, int32 X, int32 Y)
	{
		return FIntPoint(X * Level.TileVoxels, Y * Level.TileVoxels);
	}
}

const TArray<FMadFarLevel>& MadFall::FarTerrain::GetLevels()
{
	static const TArray<FMadFarLevel> Levels = {
		{ 128, 8, 640, 2.0f },
		{ 512, 32, 2048, 6.0f },
		{ 2048, 128, 6144, 20.0f },
	};
	return Levels;
}

void MadFall::FarTerrain::GatherWanted(const FVector2D& Viewer, int32 HiddenHalfExtent, int32 RangeScalePercent, TArray<FMadFarTileKey>& Out)
{
	Out.Reset();
	const TArray<FMadFarLevel>& Levels = GetLevels();
	int32 Hidden = HiddenHalfExtent;
	for (int32 LevelIndex = 0; LevelIndex < Levels.Num(); ++LevelIndex)
	{
		const FMadFarLevel& Level = Levels[LevelIndex];
		const int32 Range = FMath::Max(Level.TileVoxels, Level.RangeVoxels * FMath::Clamp(RangeScalePercent, 10, 400) / 100);
		const int32 MinX = FMath::FloorToInt32((Viewer.X - Range) / Level.TileVoxels);
		const int32 MaxX = FMath::FloorToInt32((Viewer.X + Range) / Level.TileVoxels);
		const int32 MinY = FMath::FloorToInt32((Viewer.Y - Range) / Level.TileVoxels);
		const int32 MaxY = FMath::FloorToInt32((Viewer.Y + Range) / Level.TileVoxels);
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FIntPoint Min = TileOrigin(Level, X, Y);
				const FVector2D Centre(Min.X + Level.TileVoxels * 0.5, Min.Y + Level.TileVoxels * 0.5);
				if (FMath::Max(FMath::Abs(Centre.X - Viewer.X), FMath::Abs(Centre.Y - Viewer.Y)) > Range)
				{
					continue;
				}
				// Entirely inside what is already drawn better.
				const bool bInside = Min.X >= Viewer.X - Hidden && Min.X + Level.TileVoxels <= Viewer.X + Hidden
					&& Min.Y >= Viewer.Y - Hidden && Min.Y + Level.TileVoxels <= Viewer.Y + Hidden;
				if (!bInside)
				{
					Out.Add({ LevelIndex, X, Y });
				}
			}
		}
		// What this level is guaranteed to cover, whichever way its grid falls.
		Hidden = FMath::Max(Hidden, Range - Level.TileVoxels);
	}
}

void MadFall::FarTerrain::BuildTile(const FMadWorldGenerator& Generator, const FMadFarTileKey& Key, const TArray<FColor>& BiomeColours,
	FColor WaterColour, FMadFarTileMesh& Out)
{
	const FMadFarLevel& Level = GetLevels()[Key.Level];
	const int32 Cells = Level.TileVoxels / Level.SpacingVoxels;
	const int32 Side = Cells + 1;
	const FIntPoint Origin = TileOrigin(Level, Key.X, Key.Y);
	const float SeaTop = static_cast<float>(Generator.GetSettings().SeaLevel + 1);
	const float Spacing = static_cast<float>(Level.SpacingVoxels);

	Out = FMadFarTileMesh();
	Out.Key = Key;

	// Heights on a grid one sample wider than the tile on every side, for normals.
	const int32 Wide = Side + 2;
	TArray<float> Heights;
	Heights.SetNumUninitialized(Wide * Wide);
	for (int32 J = 0; J < Wide; ++J)
	{
		for (int32 I = 0; I < Wide; ++I)
		{
			const float WX = Origin.X + (I - 1) * Spacing;
			const float WY = Origin.Y + (J - 1) * Spacing;
			// Water is flat at its surface; the land under it is not what shows.
			Heights[I + J * Wide] = FMath::Max(Generator.GetSurfaceHeight(WX, WY), SeaTop);
		}
	}
	auto H = [&](int32 I, int32 J) { return Heights[(I + 1) + (J + 1) * Wide]; };

	const int32 SurfaceCount = Side * Side;
	Out.Positions.Reserve(SurfaceCount + Side * 4);
	Out.Normals.Reserve(SurfaceCount + Side * 4);
	Out.Colors.Reserve(SurfaceCount + Side * 4);
	for (int32 J = 0; J < Side; ++J)
	{
		for (int32 I = 0; I < Side; ++I)
		{
			const float WX = Origin.X + I * Spacing;
			const float WY = Origin.Y + J * Spacing;
			const float Height = H(I, J);
			Out.Positions.Add(FVector(I * Spacing, J * Spacing, Height - Level.DropVoxels) * MadFall::VoxelSizeUU);
			const FVector Normal(H(I - 1, J) - H(I + 1, J), H(I, J - 1) - H(I, J + 1), 2.0f * Spacing);
			Out.Normals.Add(Normal.GetSafeNormal(KINDA_SMALL_NUMBER, FVector::UpVector));
			const bool bWater = Generator.GetSurfaceHeight(WX, WY) < SeaTop - 0.5f;
			const int32 Biome = bWater ? INDEX_NONE : Generator.GetDominantBiome(WX, WY);
			Out.Colors.Add(BiomeColours.IsValidIndex(Biome) ? BiomeColours[Biome] : (bWater ? WaterColour : FColor(90, 110, 60)));
		}
	}
	for (int32 J = 0; J < Cells; ++J)
	{
		for (int32 I = 0; I < Cells; ++I)
		{
			const int32 A = I + J * Side;
			const int32 B = A + 1;
			const int32 C = A + Side;
			const int32 D = C + 1;
			Out.Triangles.Append({ A, C, B, B, C, D });
		}
	}

	// Skirts: each edge copied a few voxels down and joined to the surface edge,
	// facing outwards, so a coarser neighbour's gap is never see-through.
	const float Skirt = FMath::Max(8.0f, Spacing * 1.5f) * MadFall::VoxelSizeUU;
	auto AddSkirt = [&](TFunctionRef<int32(int32)> EdgeIndex, bool bFlip)
	{
		const int32 Base = Out.Positions.Num();
		for (int32 K = 0; K < Side; ++K)
		{
			// Copies: adding an array's own element by reference is refused (it may reallocate).
			const int32 Top = EdgeIndex(K);
			const FVector Position = Out.Positions[Top] - FVector(0.0, 0.0, Skirt);
			const FVector Normal = Out.Normals[Top];
			const FColor Colour = Out.Colors[Top];
			Out.Positions.Add(Position);
			Out.Normals.Add(Normal);
			Out.Colors.Add(Colour);
		}
		for (int32 K = 0; K < Cells; ++K)
		{
			const int32 T0 = EdgeIndex(K);
			const int32 T1 = EdgeIndex(K + 1);
			const int32 B0 = Base + K;
			const int32 B1 = Base + K + 1;
			if (bFlip)
			{
				Out.Triangles.Append({ T0, T1, B0, B0, T1, B1 });
			}
			else
			{
				Out.Triangles.Append({ T0, B0, T1, T1, B0, B1 });
			}
		}
	};
	AddSkirt([Side](int32 K) { return K; }, false);                              // south edge (J = 0)
	AddSkirt([Side](int32 K) { return K + (Side - 1) * Side; }, true);            // north edge
	AddSkirt([Side](int32 K) { return K * Side; }, true);                          // west edge (I = 0)
	AddSkirt([Side](int32 K) { return (Side - 1) + K * Side; }, false);            // east edge
}

// --- subsystem ----------------------------------------------------------------------

bool UMadFarTerrainSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UMadFarTerrainSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Initialised before and torn down after this, so its generator outlives every build.
	Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
}

void UMadFarTerrainSubsystem::Deinitialize()
{
	// Builds read the voxel world's generator; let them finish before it goes.
	// Their results arrive through a weak pointer and are dropped.
	UE::Tasks::Wait(Tasks);
	Tasks.Reset();
	Tiles.Reset();
	Super::Deinitialize();
}

TStatId UMadFarTerrainSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadFarTerrainSubsystem, STATGROUP_Tickables);
}

int32 UMadFarTerrainSubsystem::NumBuilt() const
{
	int32 Built = 0;
	for (const TPair<FMadFarTileKey, FTile>& Pair : Tiles)
	{
		Built += Pair.Value.Component.IsValid() ? 1 : 0;
	}
	return Built;
}

void UMadFarTerrainSubsystem::EnsureActor()
{
	if (TerrainActor != nullptr)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	TerrainActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (TerrainActor != nullptr)
	{
		USceneComponent* Root = NewObject<USceneComponent>(TerrainActor, TEXT("FarTerrainRoot"));
		TerrainActor->SetRootComponent(Root);
		Root->RegisterComponent();
	}

	// Biome index -> the colour of its surface block, looked up once.
	const FMadBiomeRegistry& Biomes = UMadVoxelWorldSubsystem::GetBiomeRegistry();
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	BiomeColours.SetNum(Biomes.Num());
	for (int32 Index = 0; Index < Biomes.Num(); ++Index)
	{
		const FMadBlockDefView Surface = Blocks.GetBlockViewById(Biomes.Get(Index).SurfaceBlock);
		BiomeColours[Index] = MadFall::GetSurfaces().GetVertexColor(Surface.MaterialClass);
	}
	WaterColour = MadFall::GetSurfaces().GetVertexColor(FName(TEXT("madfall:water")));
}

void UMadFarTerrainSubsystem::RefreshWanted(const FVector2D& Viewer)
{
	const IConsoleVariable* StreamRadius = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.stream.Radius"));
	// Real chunks cover out to the stream radius; hide level-0 tiles a chunk inside that.
	const int32 Hidden = FMath::Max(0, ((StreamRadius ? StreamRadius->GetInt() : 8) - 1) * MadFall::ChunkSize);
	const int32 Range = CVarFarRange.GetValueOnGameThread();

	TArray<FMadFarTileKey> Wanted;
	MadFall::FarTerrain::GatherWanted(Viewer, Hidden, Range, Wanted);

	for (TPair<FMadFarTileKey, FTile>& Pair : Tiles)
	{
		Pair.Value.bWanted = false;
	}
	for (const FMadFarTileKey& Key : Wanted)
	{
		FTile& Tile = Tiles.FindOrAdd(Key);
		Tile.bWanted = true;
	}

	// Drop what fell out of range. A tile still building is dropped when it lands.
	for (auto It = Tiles.CreateIterator(); It; ++It)
	{
		if (!It->Value.bWanted && !It->Value.bBuilding)
		{
			if (UProceduralMeshComponent* Component = It->Value.Component.Get())
			{
				Component->DestroyComponent();
			}
			It.RemoveCurrent();
		}
	}

	BuildQueue.Reset();
	for (const TPair<FMadFarTileKey, FTile>& Pair : Tiles)
	{
		if (Pair.Value.bWanted && !Pair.Value.bBuilding && !Pair.Value.Component.IsValid())
		{
			BuildQueue.Add(Pair.Key);
		}
	}
	// Nearest and finest first: the land just past the chunks matters most.
	const TArray<FMadFarLevel>& Levels = MadFall::FarTerrain::GetLevels();
	BuildQueue.Sort([&Levels, Viewer](const FMadFarTileKey& A, const FMadFarTileKey& B)
	{
		auto Distance = [&Levels, Viewer](const FMadFarTileKey& K)
		{
			const FMadFarLevel& L = Levels[K.Level];
			return FVector2D::DistSquared(FVector2D((K.X + 0.5) * L.TileVoxels, (K.Y + 0.5) * L.TileVoxels), Viewer);
		};
		return A.Level != B.Level ? A.Level < B.Level : Distance(A) < Distance(B);
	});

	LastViewer = Viewer;
	LastHidden = Hidden;
	LastRange = Range;
}

void UMadFarTerrainSubsystem::LaunchBuilds()
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	if (Generator == nullptr)
	{
		return;
	}

	while (BuildsInFlight < MaxBuildsInFlight && BuildQueue.Num() > 0)
	{
		const FMadFarTileKey Key = BuildQueue[0];
		BuildQueue.RemoveAt(0, EAllowShrinking::No);
		FTile* Tile = Tiles.Find(Key);
		if (Tile == nullptr || !Tile->bWanted || Tile->Component.IsValid() || Tile->bBuilding)
		{
			continue;
		}
		Tile->bBuilding = true;
		++BuildsInFlight;

		// The generator is owned by the voxel world and outlives this tick's
		// tasks only while the world does; the weak pointer below checks that.
		TWeakObjectPtr<UMadFarTerrainSubsystem> WeakThis(this);
		TArray<FColor> Colours = BiomeColours;
		const FColor Water = WaterColour;
		Tasks.RemoveAll([](const UE::Tasks::FTask& Task) { return Task.IsCompleted(); });
		Tasks.Add(UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, Generator, Key, Colours = MoveTemp(Colours), Water]()
		{
			TSharedPtr<FMadFarTileMesh> Mesh = MakeShared<FMadFarTileMesh>();
			MadFall::FarTerrain::BuildTile(*Generator, Key, Colours, Water, *Mesh);
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Mesh]()
			{
				if (UMadFarTerrainSubsystem* This = WeakThis.Get())
				{
					FScopeLock Lock(&This->FinishedLock);
					This->Finished.Add(Mesh);
				}
			});
		}));
	}
}

void UMadFarTerrainSubsystem::ApplyFinished()
{
	TArray<TSharedPtr<FMadFarTileMesh>> Ready;
	{
		FScopeLock Lock(&FinishedLock);
		Ready = MoveTemp(Finished);
		Finished.Reset();
	}
	if (Ready.Num() == 0)
	{
		return;
	}

	UMaterialInterface* Material = nullptr;
	if (UMadChunkMeshSubsystem* Meshes = GetWorld()->GetSubsystem<UMadChunkMeshSubsystem>())
	{
		Material = Meshes->GetMaterialForClass(NAME_None);
	}

	const double Start = FPlatformTime::Seconds();
	const double BudgetMs = MadFall::FrameBudget::GetRemainingMs(CreateShareMs, 0.0);
	int32 Index = 0;
	for (; Index < Ready.Num(); ++Index)
	{
		// Only start a tile that fits in what is left of the frame, judged by what
		// tiles have been costing: the far terrain is never urgent enough to be
		// the frame that breaks the budget.
		if ((FPlatformTime::Seconds() - Start) * 1000.0 + TileCostMs > BudgetMs)
		{
			break;
		}
		const double TileStart = FPlatformTime::Seconds();
		const FMadFarTileMesh& Mesh = *Ready[Index];
		--BuildsInFlight;
		FTile* Tile = Tiles.Find(Mesh.Key);
		if (Tile == nullptr)
		{
			continue;
		}
		Tile->bBuilding = false;
		if (!Tile->bWanted || TerrainActor == nullptr)
		{
			Tiles.Remove(Mesh.Key);
			continue;
		}

		const FMadFarLevel& Level = MadFall::FarTerrain::GetLevels()[Mesh.Key.Level];
		UProceduralMeshComponent* Component = NewObject<UProceduralMeshComponent>(TerrainActor, NAME_None, RF_Transient);
		Component->SetupAttachment(TerrainActor->GetRootComponent());
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		// Shadows from 500 m away cost shadow cascades for nothing a survivor can see.
		Component->SetCastShadow(false);
		Component->bAffectDistanceFieldLighting = false;
		Component->SetRelativeLocation(FVector(Mesh.Key.X * Level.TileVoxels, Mesh.Key.Y * Level.TileVoxels, 0.0) * MadFall::VoxelSizeUU);
		if (Material != nullptr)
		{
			Component->SetMaterial(0, Material);
		}
		Component->CreateMeshSection(0, Mesh.Positions, Mesh.Triangles, Mesh.Normals, TArray<FVector2D>(), Mesh.Colors,
			TArray<FProcMeshTangent>(), /*bCreateCollision*/ false);
		Component->RegisterComponent();
		Tile->Component = Component;
		++TotalCreated;
		TileCostMs = FMath::Lerp(TileCostMs, (FPlatformTime::Seconds() - TileStart) * 1000.0, 0.2);
	}
	LastCreateMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	// The estimate only learns from tiles that are created, so one slow creation
	// (a component's first ray tracing geometry, a hitch elsewhere in the frame)
	// could lift it above the share and no tile would ever fit again: builds in
	// flight never came back, and CI saw anywhere from 4 to 195 of 195 tiles for
	// the same walk. While nothing fits, the old cost is forgotten over about
	// half a second, so a spike delays the far terrain instead of stopping it.
	if (Index == 0)
	{
		TileCostMs = FMath::Max(0.1, TileCostMs * 0.95);
	}

	// What did not fit this frame goes back to wait for the next.
	if (Index < Ready.Num())
	{
		FScopeLock Lock(&FinishedLock);
		for (; Index < Ready.Num(); ++Index)
		{
			Finished.Add(Ready[Index]);
		}
	}
}

void UMadFarTerrainSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	MAD_FRAME_SCOPE(FarTerrain);

	UWorld* World = GetWorld();
	const bool bEnabled = CVarFarEnabled.GetValueOnGameThread() != 0;
	if (bEnabled != bLastEnabled)
	{
		bLastEnabled = bEnabled;
		if (TerrainActor != nullptr)
		{
			TerrainActor->SetActorHiddenInGame(!bEnabled);
		}
	}
	if (!bEnabled || World->GetSubsystem<UMadVoxelWorldSubsystem>() == nullptr)
	{
		return;
	}

	EnsureActor();
	const FVector2D Viewer = ViewerVoxels(*World);
	const IConsoleVariable* StreamRadius = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.stream.Radius"));
	const int32 Hidden = FMath::Max(0, ((StreamRadius ? StreamRadius->GetInt() : 8) - 1) * MadFall::ChunkSize);
	if (FVector2D::Distance(Viewer, LastViewer) > 16.0f || Hidden != LastHidden || CVarFarRange.GetValueOnGameThread() != LastRange)
	{
		RefreshWanted(Viewer);
	}
	LaunchBuilds();
	ApplyFinished();
}

FString UMadFarTerrainSubsystem::DescribeStatus() const
{
	int32 PerLevel[3] = {};
	for (const TPair<FMadFarTileKey, FTile>& Pair : Tiles)
	{
		if (Pair.Value.Component.IsValid() && Pair.Key.Level >= 0 && Pair.Key.Level < 3)
		{
			++PerLevel[Pair.Key.Level];
		}
	}
	return FString::Printf(TEXT("Far terrain: %d tile(s) wanted, %d built (%d / %d / %d by level), %d queued, %d building, %d created, last create %.2f ms."),
		Tiles.Num(), NumBuilt(), PerLevel[0], PerLevel[1], PerLevel[2], BuildQueue.Num(), BuildsInFlight, TotalCreated, LastCreateMs);
}

static FAutoConsoleCommandWithWorld GMadFarStatusCommand(
	TEXT("mad.far.status"),
	TEXT("Far terrain tiles: how many are wanted, built and building."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadFarTerrainSubsystem* Far = World ? World->GetSubsystem<UMadFarTerrainSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallMesher, Display, TEXT("%s"), *Far->DescribeStatus());
		}
	}));

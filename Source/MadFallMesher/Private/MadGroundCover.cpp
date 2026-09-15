// Copyright MadFall. All Rights Reserved.

#include "MadGroundCover.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "IMadBlockRegistry.h"
#include "MadBlockRegistry.h"
#include "MadChunkStorage.h"
#include "MadFallMesher.h"
#include "MadFrameBudget.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"

namespace
{
	TAutoConsoleVariable<int32> CVarCoverRadius(
		TEXT("mad.cover.Radius"), 2,
		TEXT("Chunks around the camera, horizontally, that draw ground cover. 0 turns it off."));

	TAutoConsoleVariable<float> CVarCoverDistance(
		TEXT("mad.cover.Distance"), 3600.0f,
		TEXT("Distance in cm past which ground cover is culled; it fades out over the last quarter."));

	TAutoConsoleVariable<float> CVarCoverBudgetMs(
		TEXT("mad.cover.BudgetMs"), 0.4f,
		TEXT("Game-thread milliseconds per frame for building chunks' ground cover."));

	const TCHAR* PlaneMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");
	const TCHAR* CoverMaterialPath = TEXT("/Game/Materials/M_MadGroundCover.M_MadGroundCover");

	/** A stable hash of a world column and a salt, split into 0..1 floats. */
	struct FColumnRandom
	{
		uint32 State;

		FColumnRandom(int32 WorldX, int32 WorldY, uint32 Salt)
		{
			State = HashCombine(HashCombine(GetTypeHash(WorldX * 73856093), GetTypeHash(WorldY * 19349663)), Salt) | 1u;
		}

		float Next()
		{
			// xorshift32
			State ^= State << 13;
			State ^= State >> 17;
			State ^= State << 5;
			return (State & 0xFFFFFF) / static_cast<float>(0x1000000);
		}
	};
}

void MadFall::GroundCover::Collect(const FMadChunkStorage& Storage, const FMadChunkCoord& Coord, const IMadBlockRegistry& Registry, TArray<FTuft>& OutTufts)
{
	OutTufts.Reset();

	// Palette first: most chunks are underground or in the air.
	TMap<uint16, const FMadSurfaceDefinition*> Covered;
	for (const FMadBlockPaletteEntry& Entry : Storage.GetPalette())
	{
		if (Entry.RefCount == 0 && !Storage.IsUniform())
		{
			continue;
		}
		const FMadBlockDefView View = Registry.GetBlockView(Entry.RuntimeId);
		if (View.ShapeKind == EMadBlockShapeKind::Model)
		{
			continue;
		}
		if (const FMadSurfaceDefinition* Surface = MadFall::GetSurfaces().Find(View.MaterialClass); Surface && Surface->HasCover())
		{
			Covered.Add(Entry.RuntimeId, Surface);
		}
	}
	if (Covered.Num() == 0)
	{
		return;
	}

	const uint8* Density = Storage.GetDensityArray();
	const uint8* Flags = Storage.GetFlagsArray();
	auto DensityAt = [&Storage, Density](int32 Index) { return Density ? Density[Index] : Storage.GetDefaultDensity(); };
	auto FlagsAt = [&Storage, Flags](int32 Index) { return Flags ? Flags[Index] : Storage.GetDefaultFlags(); };

	for (int32 Y = 0; Y < MadFall::ChunkSize; ++Y)
	{
		for (int32 X = 0; X < MadFall::ChunkSize; ++X)
		{
			// The top solid voxel with this chunk's air above it. A column whose top
			// voxel is solid continues in the chunk above, which grows its own cover.
			for (int32 Z = MadFall::ChunkSize - 2; Z >= 0; --Z)
			{
				const int32 Index = MadFall::VoxelIndex(X, Y, Z);
				const uint8 Solid = DensityAt(Index);
				if (Solid < 128)
				{
					continue;
				}
				const int32 AboveIndex = MadFall::VoxelIndex(X, Y, Z + 1);
				const uint8 Above = DensityAt(AboveIndex);
				if (Z == MadFall::ChunkSize - 2 && Above >= 128)
				{
					break;
				}
				const FMadSurfaceDefinition* const* Surface = Covered.Find(Storage.GetBlockId(Index));
				if (Surface == nullptr || Above >= 128 || Storage.GetBlockId(AboveIndex) != MadFall::BlockTypeAir)
				{
					break;
				}

				// Where the surface is: the top of a placed block, or where smooth
				// terrain's density crosses the iso level between the two samples.
				const bool bCubic = (FlagsAt(Index) & static_cast<uint8>(EMadVoxelFlags::Cubic)) != 0;
				const float Top = bCubic
					? Z + 1.0f
					: Z + 0.5f + FMath::Clamp((Solid - 128.0f) / FMath::Max(1.0f, static_cast<float>(Solid) - Above), 0.0f, 1.0f);

				int32 WorldX, WorldY, WorldZ;
				MadFall::ChunkToWorld(Coord, X, Y, Z, WorldX, WorldY, WorldZ);
				const float BaseZ = (WorldZ - Z + Top) * MadFall::VoxelSizeUU;

				auto Grow = [&](uint32 Salt, float Chance, bool bFlower)
				{
					FColumnRandom Random(WorldX, WorldY, Salt);
					if (Random.Next() >= Chance)
					{
						return;
					}
					FTuft Tuft;
					Tuft.Location = FVector((WorldX + 0.15f + 0.7f * Random.Next()) * MadFall::VoxelSizeUU,
						(WorldY + 0.15f + 0.7f * Random.Next()) * MadFall::VoxelSizeUU, BaseZ - 3.0f);
					Tuft.Yaw = Random.Next() * 180.0f;
					const float Height = FMath::Lerp((*Surface)->CoverHeightMin, (*Surface)->CoverHeightMax, Random.Next());
					Tuft.Height = (bFlower ? Height * 1.1f : Height) * MadFall::VoxelSizeUU;
					Tuft.Width = (bFlower ? 0.45f : 0.55f + 0.25f * Random.Next()) * MadFall::VoxelSizeUU;
					Tuft.Surface = (*Surface)->Id;
					Tuft.bFlower = bFlower;
					OutTufts.Add(Tuft);
				};
				Grow(0x6A09E667, (*Surface)->CoverDensity, false);
				Grow(0xBB67AE85, (*Surface)->CoverFlowers, true);
				break;
			}
		}
	}
}

void MadFall::GroundCover::MakeTransforms(const FTuft& Tuft, FTransform& OutA, FTransform& OutB)
{
	// The plane lies in XY facing +Z; a roll of -90 stands it up with its +Y
	// pointing at the sky, which the material reads as the blades' height (+90
	// planted every tuft upside down).
	const FVector Scale(Tuft.Width / 100.0f, Tuft.Height / 100.0f, 1.0f);
	const FVector Centre = Tuft.Location + FVector(0.0, 0.0, Tuft.Height * 0.5f);
	OutA = FTransform(FRotator(0.0f, Tuft.Yaw, -90.0f).Quaternion(), Centre, Scale);
	OutB = FTransform(FRotator(0.0f, Tuft.Yaw + 90.0f, -90.0f).Quaternion(), Centre, Scale);
}

// ===========================================================================
// Subsystem
// ===========================================================================

bool UMadGroundCoverSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && FApp::CanEverRender() && Super::ShouldCreateSubsystem(Outer);
}

void UMadGroundCoverSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	VoxelWorld = Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
	// Loaded now, not in the first build: a synchronous load inside a chunk
	// build is a hitch the moment the first grass streams in.
	PlaneMesh = LoadObject<UStaticMesh>(nullptr, PlaneMeshPath);
	BaseMaterial = LoadObject<UMaterialInterface>(nullptr, CoverMaterialPath);
	if (VoxelWorld != nullptr)
	{
		VoxelWorld->OnChunkChanged().AddUObject(this, &UMadGroundCoverSubsystem::HandleChunkChanged);
		VoxelWorld->OnChunkUnloaded().AddUObject(this, &UMadGroundCoverSubsystem::HandleChunkUnloaded);
	}
}

void UMadGroundCoverSubsystem::Deinitialize()
{
	if (VoxelWorld != nullptr)
	{
		VoxelWorld->OnChunkChanged().RemoveAll(this);
		VoxelWorld->OnChunkUnloaded().RemoveAll(this);
	}
	Built.Reset();
	Wanted.Reset();
	Pending.Reset();
	AllComponents.Reset();
	Super::Deinitialize();
}

TStatId UMadGroundCoverSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadGroundCoverSubsystem, STATGROUP_Tickables);
}

void UMadGroundCoverSubsystem::HandleChunkChanged(const FMadChunkCoord& Coord)
{
	if (Wanted.Contains(Coord))
	{
		Pending.Add(Coord);
	}
}

void UMadGroundCoverSubsystem::HandleChunkUnloaded(const FMadChunkCoord& Coord)
{
	Pending.Remove(Coord);
	ReleaseChunk(Coord);
}

void UMadGroundCoverSubsystem::UpdateWanted()
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (Controller == nullptr || Controller->PlayerCameraManager == nullptr)
	{
		return;
	}
	const FVector Camera = Controller->PlayerCameraManager->GetCameraLocation();
	int32 VoxelX, VoxelY, VoxelZ;
	MadFall::UnrealToWorldVoxel(Camera, VoxelX, VoxelY, VoxelZ);
	const FMadChunkCoord Centre = MadFall::WorldToChunk(VoxelX, VoxelY, VoxelZ);
	const int32 Radius = FMath::Clamp(CVarCoverRadius.GetValueOnGameThread(), 0, 6);
	CentreChunk = Centre;

	TSet<FMadChunkCoord> Next;
	if (Radius > 0)
	{
		// A column's surface can be a chunk above or below the camera's.
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		{
			for (int32 DY = -Radius; DY <= Radius; ++DY)
			{
				for (int32 DX = -Radius; DX <= Radius; ++DX)
				{
					Next.Add(FMadChunkCoord(Centre.X + DX, Centre.Y + DY, Centre.Z + DZ));
				}
			}
		}
	}
	for (const FMadChunkCoord& Coord : Next)
	{
		if (!Built.Contains(Coord) && VoxelWorld->FindChunk(Coord).IsValid())
		{
			Pending.Add(Coord);
		}
	}
	TArray<FMadChunkCoord> Leaving;
	for (const TPair<FMadChunkCoord, TArray<TWeakObjectPtr<UInstancedStaticMeshComponent>>>& Pair : Built)
	{
		if (!Next.Contains(Pair.Key))
		{
			Leaving.Add(Pair.Key);
		}
	}
	for (const FMadChunkCoord& Coord : Leaving)
	{
		ReleaseChunk(Coord);
		Pending.Remove(Coord);
	}
	Wanted = MoveTemp(Next);
}

void UMadGroundCoverSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (VoxelWorld == nullptr)
	{
		return;
	}
	MAD_FRAME_SCOPE(Meshing);

	WantedTimer -= DeltaTime;
	if (WantedTimer <= 0.0f)
	{
		// Chunks stream in behind the camera's move, so the wanted set is refreshed
		// twice a second rather than only when the centre chunk changes.
		WantedTimer = 0.5f;
		UpdateWanted();
	}
	if (Pending.Num() == 0 || MadFall::FrameBudget::GetSpentThisFrameMs() > MadFall::FrameBudget::BudgetMs * 0.5)
	{
		return;
	}

	// Nearest first: the ground under the survivor matters more than the ground a chunk away.
	FMadChunkCoord Next = *Pending.CreateConstIterator();
	int32 Best = TNumericLimits<int32>::Max();
	for (const FMadChunkCoord& Coord : Pending)
	{
		const int32 Distance = FMath::Abs(Coord.X - CentreChunk.X) + FMath::Abs(Coord.Y - CentreChunk.Y) + FMath::Abs(Coord.Z - CentreChunk.Z);
		if (Distance < Best)
		{
			Best = Distance;
			Next = Coord;
		}
	}

	const double Start = FPlatformTime::Seconds();
	const double BudgetSeconds = MadFall::FrameBudget::GetRemainingMs(FMath::Max(0.0f, CVarCoverBudgetMs.GetValueOnGameThread()), 0.0) / 1000.0;
	do
	{
		Pending.Remove(Next);
		BuildChunk(Next);
		if (Pending.Num() == 0)
		{
			break;
		}
		Next = *Pending.CreateConstIterator();
	}
	while (FPlatformTime::Seconds() - Start < BudgetSeconds);
}

void UMadGroundCoverSubsystem::BuildChunk(const FMadChunkCoord& Coord)
{
	const double Start = FPlatformTime::Seconds();
	ReleaseChunk(Coord);
	const FMadChunkPtr Chunk = VoxelWorld->FindChunk(Coord);
	if (!Chunk.IsValid())
	{
		return;
	}

	TArray<MadFall::GroundCover::FTuft> Tufts;
	{
		FRWScopeLock Lock(Chunk->Lock, SLT_ReadOnly);
		MadFall::GroundCover::Collect(Chunk->Storage, Coord, UMadVoxelWorldSubsystem::GetBlockRegistry(), Tufts);
	}
	++TotalBuilds;
	TArray<TWeakObjectPtr<UInstancedStaticMeshComponent>>& Components = Built.Add(Coord);
	if (Tufts.Num() == 0)
	{
		return;
	}

	// One component per surface and kind: each has its own colour.
	TMap<TPair<FName, bool>, TArray<FTransform>> Groups;
	for (const MadFall::GroundCover::FTuft& Tuft : Tufts)
	{
		TArray<FTransform>& Transforms = Groups.FindOrAdd(TPair<FName, bool>(Tuft.Surface, Tuft.bFlower));
		FTransform A, B;
		MadFall::GroundCover::MakeTransforms(Tuft, A, B);
		Transforms.Add(A);
		Transforms.Add(B);
	}
	for (TPair<TPair<FName, bool>, TArray<FTransform>>& Group : Groups)
	{
		if (UInstancedStaticMeshComponent* Component = MakeComponent(Group.Key.Key, Group.Key.Value))
		{
			Component->AddInstances(Group.Value, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
			Components.Add(Component);
		}
	}
	const double BuildMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	WorstBuildMs = FMath::Max(WorstBuildMs, BuildMs);
	if (BuildMs > 1.0)
	{
		UE_LOG(LogMadFallMesher, Log, TEXT("Slow ground cover build %s: %.2f ms, %d tufts."), *Coord.ToString(), BuildMs, Tufts.Num());
	}
}

UInstancedStaticMeshComponent* UMadGroundCoverSubsystem::MakeComponent(FName Surface, bool bFlower)
{
	if (PlaneMesh == nullptr || BaseMaterial == nullptr)
	{
		return nullptr;
	}
	if (CoverActor == nullptr)
	{
		FActorSpawnParameters Params;
		Params.Name = MakeUniqueObjectName(GetWorld()->PersistentLevel, AActor::StaticClass(), TEXT("MadFallGroundCover"));
		Params.ObjectFlags |= RF_Transient;
		CoverActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		if (CoverActor == nullptr)
		{
			return nullptr;
		}
		CoverActor->SetActorEnableCollision(false);
		USceneComponent* Root = NewObject<USceneComponent>(CoverActor, TEXT("Root"));
		Root->SetMobility(EComponentMobility::Static);
		Root->RegisterComponent();
		CoverActor->SetRootComponent(Root);
	}

	// One material instance per surface and kind, shared by every chunk.
	const FName Key(*FString::Printf(TEXT("%s|%d"), *Surface.ToString(), bFlower ? 1 : 0));
	TObjectPtr<UMaterialInstanceDynamic>& Material = Materials.FindOrAdd(Key);
	if (Material == nullptr)
	{
		Material = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		FLinearColor Colour(0.1f, 0.25f, 0.05f);
		if (const FMadSurfaceDefinition* Found = MadFall::GetSurfaces().Find(Surface))
		{
			Colour = Found->CoverColor;
		}
		Material->SetVectorParameterValue(TEXT("Color"), Colour);
		Material->SetScalarParameterValue(TEXT("Flower"), bFlower ? 1.0f : 0.0f);
	}

	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(CoverActor, NAME_None, RF_Transient);
	Component->SetStaticMesh(PlaneMesh);
	Component->SetMaterial(0, Material);
	Component->SetMobility(EComponentMobility::Static);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCanEverAffectNavigation(false);
	// Thousands of swaying cards casting shadows would redraw shadow maps every
	// frame for little gain: the ground under them is already shaded.
	Component->SetCastShadow(false);
	Component->bAffectDistanceFieldLighting = false;
	const float Distance = FMath::Max(500.0f, CVarCoverDistance.GetValueOnGameThread());
	Component->SetCullDistances(static_cast<int32>(Distance * 0.75f), static_cast<int32>(Distance));
	Component->SetupAttachment(CoverActor->GetRootComponent());
	Component->RegisterComponent();
	AllComponents.Add(Component);
	return Component;
}

void UMadGroundCoverSubsystem::ReleaseChunk(const FMadChunkCoord& Coord)
{
	TArray<TWeakObjectPtr<UInstancedStaticMeshComponent>> Removed;
	if (!Built.RemoveAndCopyValue(Coord, Removed))
	{
		return;
	}
	for (const TWeakObjectPtr<UInstancedStaticMeshComponent>& Weak : Removed)
	{
		if (UInstancedStaticMeshComponent* Component = Weak.Get())
		{
			AllComponents.Remove(Component);
			Component->DestroyComponent();
		}
	}
}

FString UMadGroundCoverSubsystem::DescribeStatus() const
{
	int32 Instances = 0;
	for (const TObjectPtr<UInstancedStaticMeshComponent>& Component : AllComponents)
	{
		Instances += Component != nullptr ? Component->GetInstanceCount() : 0;
	}
	return FString::Printf(TEXT("Ground cover: %d chunks built (%d wanted, %d pending), %d components, %d instances; %lld builds, worst %.2f ms"),
		Built.Num(), Wanted.Num(), Pending.Num(), AllComponents.Num(), Instances, TotalBuilds, WorstBuildMs);
}

static FAutoConsoleCommandWithWorld GMadCoverStatusCommand(
	TEXT("mad.cover.status"),
	TEXT("Ground cover chunks, components and instances."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadGroundCoverSubsystem* Cover = World ? World->GetSubsystem<UMadGroundCoverSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallMesher, Display, TEXT("%s"), *Cover->DescribeStatus());
		}
		else
		{
			UE_LOG(LogMadFallMesher, Display, TEXT("Ground cover: off (no renderer)."));
		}
	}));

// Copyright MadFall. All Rights Reserved.

#include "MadModelInstances.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "MadBlockRegistry.h"
#include "MadChunkStorage.h"
#include "MadFallMesher.h"
#include "MadFrameBudget.h"
#include "MadOrientation.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	TAutoConsoleVariable<int32> CVarMaxLightsPerChunk(
		TEXT("mad.models.MaxLightsPerChunk"),
		16,
		TEXT("Most light-emitting blocks (torches) given a light in one chunk; the rest still render, unlit."));

	TAutoConsoleVariable<bool> CVarLightShadows(
		TEXT("mad.models.LightShadows"),
		true,
		TEXT("Whether block lights (torches, campfires) cast shadows. Without them a torch lights the room behind a wall as if the wall were not there; with them each light renders virtual shadow maps, a GPU cost per light that MaxLightsPerChunk bounds."));

	TAutoConsoleVariable<float> CVarModelBudgetMs(
		TEXT("mad.models.BudgetMs"),
		0.5f,
		TEXT("Game-thread milliseconds per frame for rebuilding chunks' model instances. At least one chunk is rebuilt per frame."));

	/** Every shipped basic shape; also the fallback when a block's mesh does not load. */
	const TCHAR* FallbackMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/**
	 * Engine material with a "Color" vector parameter. Used for a model block
	 * with no render.material, tinted by its surface colour - the static meshes
	 * a mod ships have no MadFall vertex colours for M_MadVoxel to read.
	 */
	const TCHAR* TintMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	/** The voxel patterns drawn from a mesh's local position (Scripts/make_voxel_material.py). */
	const TCHAR* PatternMaterialPath = TEXT("/Game/Materials/M_MadVoxelHeld.M_MadVoxelHeld");
}

// ===========================================================================
// Pure helpers
// ===========================================================================

FTransform MadFall::Models::MakeTransform(const FIntVector& WorldVoxel, uint8 Rotation, const FVector& OffsetVoxels, const FVector& Scale)
{
	const uint8 Orientation = static_cast<uint8>(Rotation & 0x1F) < MadFall::Orientation::Count ? static_cast<uint8>(Rotation & 0x1F) : 0;
	const MadFall::Orientation::FIntMatrix3& M = MadFall::Orientation::GetMatrix(Orientation);

	// The matrix's columns are where the block's local X, Y and Z axes end up;
	// FMatrix takes those axes as rows.
	const FVector AxisX(M.M[0][0], M.M[1][0], M.M[2][0]);
	const FVector AxisY(M.M[0][1], M.M[1][1], M.M[2][1]);
	const FVector AxisZ(M.M[0][2], M.M[1][2], M.M[2][2]);
	const FQuat Quat(FMatrix(AxisX, AxisY, AxisZ, FVector::ZeroVector).ToQuat());

	const FVector Centre = (FVector(WorldVoxel) + FVector(0.5)) * MadFall::VoxelSizeUU;
	const FVector Location = Centre + Quat.RotateVector(OffsetVoxels * MadFall::VoxelSizeUU);
	return FTransform(Quat, Location, Scale);
}

void MadFall::Models::CollectLights(const FMadChunkStorage& Storage, const FMadChunkCoord& Coord,
	const FMadBlockRegistry& Registry, TArray<FBlockLight>& OutLights)
{
	OutLights.Reset();

	TArray<const FMadBlockDefinitionData*, TInlineAllocator<4>> LightDefs;
	TArray<uint16, TInlineAllocator<4>> LightIds;
	for (const FMadBlockPaletteEntry& Entry : Storage.GetPalette())
	{
		if ((Entry.RefCount == 0 && !Storage.IsUniform()) || LightIds.Contains(Entry.RuntimeId))
		{
			continue;
		}
		const FMadBlockDefinitionData* Def = Registry.FindDefinition(Entry.RuntimeId);
		if (Def != nullptr && Def->HasLight())
		{
			LightIds.Add(Entry.RuntimeId);
			LightDefs.Add(Def);
		}
	}
	if (LightIds.Num() == 0)
	{
		return;
	}

	const uint8* Rotations = Storage.GetRotationArray();
	for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
	{
		const int32 LightIndex = LightIds.IndexOfByKey(Storage.GetBlockId(Index));
		if (LightIndex == INDEX_NONE)
		{
			continue;
		}
		int32 X, Y, Z;
		MadFall::VoxelCoords(Index, X, Y, Z);
		int32 WorldX, WorldY, WorldZ;
		MadFall::ChunkToWorld(Coord, X, Y, Z, WorldX, WorldY, WorldZ);
		// The light offset turns with the block, exactly like a model's mesh offset.
		const FTransform Transform = MakeTransform(FIntVector(WorldX, WorldY, WorldZ),
			Rotations ? Rotations[Index] : Storage.GetDefaultRotation(), LightDefs[LightIndex]->LightOffset, FVector::OneVector);
		OutLights.Add(FBlockLight{ LightIds[LightIndex], Transform.GetLocation() });
	}
}

void MadFall::Models::CollectInstances(const FMadChunkStorage& Storage, const FMadChunkCoord& Coord,
	const FMadBlockRegistry& Registry, TMap<uint16, TArray<FTransform>>& OutByBlock)
{
	OutByBlock.Reset();

	// Palette first: nearly every chunk holds no model block at all, and this
	// answers that without touching 32768 voxels.
	TArray<const FMadBlockDefinitionData*, TInlineAllocator<4>> ModelDefs;
	TArray<uint16, TInlineAllocator<4>> ModelIds;
	for (const FMadBlockPaletteEntry& Entry : Storage.GetPalette())
	{
		if (Entry.RefCount == 0 && !Storage.IsUniform())
		{
			continue;
		}
		if (ModelIds.Contains(Entry.RuntimeId))
		{
			continue;
		}
		const FMadBlockDefinitionData* Def = Registry.FindDefinition(Entry.RuntimeId);
		if (Def != nullptr && Def->ShapeKind == EMadBlockShapeKind::Model)
		{
			ModelIds.Add(Entry.RuntimeId);
			ModelDefs.Add(Def);
		}
	}
	if (ModelIds.Num() == 0)
	{
		return;
	}

	const uint8* Rotations = Storage.GetRotationArray();
	const uint8 DefaultRotation = Storage.GetDefaultRotation();
	for (int32 Z = 0; Z < MadFall::ChunkSize; ++Z)
	{
		for (int32 Y = 0; Y < MadFall::ChunkSize; ++Y)
		{
			for (int32 X = 0; X < MadFall::ChunkSize; ++X)
			{
				const int32 Index = MadFall::VoxelIndex(X, Y, Z);
				const int32 ModelIndex = ModelIds.IndexOfByKey(Storage.GetBlockId(Index));
				if (ModelIndex == INDEX_NONE)
				{
					continue;
				}

				int32 WorldX, WorldY, WorldZ;
				MadFall::ChunkToWorld(Coord, X, Y, Z, WorldX, WorldY, WorldZ);
				const FMadBlockDefinitionData& Def = *ModelDefs[ModelIndex];
				OutByBlock.FindOrAdd(ModelIds[ModelIndex]).Add(MakeTransform(FIntVector(WorldX, WorldY, WorldZ),
					Rotations ? Rotations[Index] : DefaultRotation, Def.MeshOffset, Def.MeshScale));
			}
		}
	}
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void UMadModelInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	VoxelWorld = Collection.InitializeDependency<UMadVoxelWorldSubsystem>();

	// Load every model block's mesh and material while the world initialises.
	// Loading them in the first rebuild that needs one measured 12.9 ms - a
	// hitch the moment the first barrel streams in.
	for (const FMadBlockEntry& Entry : UMadVoxelWorldSubsystem::GetBlockRegistry().GetEntries())
	{
		if (Entry.Definition.ShapeKind == EMadBlockShapeKind::Model && !Entry.bUnresolved)
		{
			GetMesh(Entry.RuntimeId);
			GetMaterial(Entry.RuntimeId);
		}
	}

	if (VoxelWorld != nullptr)
	{
		VoxelWorld->OnChunkChanged().AddUObject(this, &UMadModelInstanceSubsystem::HandleChunkChanged);
		VoxelWorld->OnChunkUnloaded().AddUObject(this, &UMadModelInstanceSubsystem::HandleChunkUnloaded);
	}
}

void UMadModelInstanceSubsystem::Deinitialize()
{
	if (VoxelWorld != nullptr)
	{
		VoxelWorld->OnChunkChanged().RemoveAll(this);
		VoxelWorld->OnChunkUnloaded().RemoveAll(this);
	}
	Chunks.Reset();
	AllComponents.Reset();
	AllLights.Reset();
	AllFlames.Reset();
	Dirty.Reset();
	Super::Deinitialize();
}

TStatId UMadModelInstanceSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadModelInstanceSubsystem, STATGROUP_Tickables);
}

void UMadModelInstanceSubsystem::HandleChunkChanged(const FMadChunkCoord& Coord)
{
	Dirty.Add(Coord);
}

void UMadModelInstanceSubsystem::HandleChunkUnloaded(const FMadChunkCoord& Coord)
{
	Dirty.Remove(Coord);
	ReleaseChunk(Coord);
}

void UMadModelInstanceSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Dirty.Num() == 0)
	{
		return;
	}

	MAD_FRAME_SCOPE(Meshing);

	const double Start = FPlatformTime::Seconds();
	// Model rebuilds can wait a frame: skip entirely when the frame is already
	// mostly spent, rather than adding a chunk's rebuild on top of it.
	if (MadFall::FrameBudget::GetSpentThisFrameMs() > MadFall::FrameBudget::BudgetMs * 0.5)
	{
		return;
	}
	const double BudgetSeconds = MadFall::FrameBudget::GetRemainingMs(FMath::Max(0.0f, CVarModelBudgetMs.GetValueOnGameThread()), 0.0) / 1000.0;
	do
	{
		auto It = Dirty.CreateIterator();
		const FMadChunkCoord Coord = *It;
		It.RemoveCurrent();
		RebuildChunk(Coord);
	}
	while (Dirty.Num() > 0 && FPlatformTime::Seconds() - Start < BudgetSeconds);
}

void UMadModelInstanceSubsystem::FlushNow()
{
	while (Dirty.Num() > 0)
	{
		auto It = Dirty.CreateIterator();
		const FMadChunkCoord Coord = *It;
		It.RemoveCurrent();
		RebuildChunk(Coord);
	}
}

// ===========================================================================
// Rebuild
// ===========================================================================

void UMadModelInstanceSubsystem::RebuildChunk(const FMadChunkCoord& Coord)
{
	if (VoxelWorld == nullptr)
	{
		return;
	}
	const FMadChunkPtr Chunk = VoxelWorld->FindChunk(Coord);
	if (!Chunk.IsValid())
	{
		ReleaseChunk(Coord);
		return;
	}

	const double Start = FPlatformTime::Seconds();

	TMap<uint16, TArray<FTransform>> ByBlock;
	TArray<MadFall::Models::FBlockLight> Lights;
	{
		FRWScopeLock Lock(Chunk->Lock, SLT_ReadOnly);
		MadFall::Models::CollectInstances(Chunk->Storage, Coord, UMadVoxelWorldSubsystem::GetBlockRegistry(), ByBlock);
		MadFall::Models::CollectLights(Chunk->Storage, Coord, UMadVoxelWorldSubsystem::GetBlockRegistry(), Lights);
	}

	FChunkModels* Existing = Chunks.Find(Coord);
	if (ByBlock.Num() == 0 && Lights.Num() == 0 && Existing == nullptr)
	{
		// The common case - no models before or after - costs a palette scan.
		return;
	}

	FChunkModels& Models = Existing ? *Existing : Chunks.Add(Coord);
	double InstanceSeconds = 0.0;

	// Drop components for block types this chunk no longer holds.
	for (auto It = Models.ByBlock.CreateIterator(); It; ++It)
	{
		if (!ByBlock.Contains(It.Key()))
		{
			if (UInstancedStaticMeshComponent* Component = It.Value().Get())
			{
				AllComponents.Remove(Component);
				Component->DestroyComponent();
			}
			It.RemoveCurrent();
		}
	}

	for (TPair<uint16, TArray<FTransform>>& Pair : ByBlock)
	{
		UInstancedStaticMeshComponent* Component = Models.ByBlock.FindRef(Pair.Key).Get();
		if (Component == nullptr)
		{
			AActor* Actor = GetOrCreateActor();
			if (Actor == nullptr)
			{
				return;
			}

			const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(Pair.Key);
			Component = NewObject<UInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient);
			Component->SetStaticMesh(GetMesh(Pair.Key));
			Component->SetMaterial(0, GetMaterial(Pair.Key));
			Component->SetMobility(EComponentMobility::Static);
			Component->SetCastShadow(Def == nullptr || Def->bCastShadow);
			// The instance is the model's collision: the chunk mesh has no faces
			// for it, so without this the player walks through barrels. Blocks
			// with "collision": "none" (an open door, a ladder) are walked through.
			const bool bNoCollision = Def != nullptr && Def->Collision == EMadBlockCollisionKind::None;
			Component->SetCollisionProfileName(bNoCollision ? UCollisionProfile::NoCollision_ProfileName : UCollisionProfile::BlockAll_ProfileName);
			Component->SetupAttachment(Actor->GetRootComponent());
			Component->RegisterComponent();
			Models.ByBlock.Add(Pair.Key, Component);
			AllComponents.Add(Component);
		}

		const double InstancesStart = FPlatformTime::Seconds();
		Component->ClearInstances();
		Component->AddInstances(Pair.Value, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
		InstanceSeconds += FPlatformTime::Seconds() - InstancesStart;
	}

	UpdateLights(Models, Lights);

	if (Models.ByBlock.Num() == 0 && Models.Lights.Num() == 0 && !Models.Flames.IsValid())
	{
		Chunks.Remove(Coord);
	}

	++TotalRebuilds;
	const double RebuildMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	WorstRebuildMs = FMath::Max(WorstRebuildMs, RebuildMs);
	if (RebuildMs > 2.0)
	{
		UE_LOG(LogMadFallMesher, Log, TEXT("Slow model rebuild %s: %.2f ms (%.2f ms setting instances)."), *Coord.ToString(), RebuildMs, InstanceSeconds * 1000.0);
	}
}

void UMadModelInstanceSubsystem::ReleaseChunk(const FMadChunkCoord& Coord)
{
	FChunkModels Removed;
	if (!Chunks.RemoveAndCopyValue(Coord, Removed))
	{
		return;
	}
	for (const TPair<uint16, TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : Removed.ByBlock)
	{
		if (UInstancedStaticMeshComponent* Component = Pair.Value.Get())
		{
			AllComponents.Remove(Component);
			Component->DestroyComponent();
		}
	}
	for (const TWeakObjectPtr<UPointLightComponent>& Light : Removed.Lights)
	{
		if (UPointLightComponent* Component = Light.Get())
		{
			AllLights.Remove(Component);
			Component->DestroyComponent();
		}
	}
	if (UInstancedStaticMeshComponent* Flames = Removed.Flames.Get())
	{
		AllFlames.Remove(Flames);
		Flames->DestroyComponent();
	}
}

void UMadModelInstanceSubsystem::UpdateLights(FChunkModels& Models, const TArray<MadFall::Models::FBlockLight>& Lights)
{
	// Point lights are the expensive part of a lit base: each one is shaded in
	// every pixel it reaches. Capped per chunk, unshadowed, and reused in place
	// so an edit near a torch creates no light it does not have to.
	const int32 Wanted = FMath::Min(Lights.Num(), FMath::Max(0, CVarMaxLightsPerChunk.GetValueOnGameThread()));
	Models.Lights.RemoveAll([](const TWeakObjectPtr<UPointLightComponent>& Light) { return !Light.IsValid(); });
	while (Models.Lights.Num() > Wanted)
	{
		if (UPointLightComponent* Component = Models.Lights.Pop().Get())
		{
			AllLights.Remove(Component);
			Component->DestroyComponent();
		}
	}

	UpdateFlames(Models, Lights);

	AActor* Actor = Wanted > 0 ? GetOrCreateActor() : nullptr;
	for (int32 Index = 0; Index < Wanted && Actor != nullptr; ++Index)
	{
		const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(Lights[Index].BlockId);
		if (Def == nullptr)
		{
			continue;
		}
		UPointLightComponent* Component = Index < Models.Lights.Num() ? Models.Lights[Index].Get() : nullptr;
		if (Component == nullptr)
		{
			Component = NewObject<UPointLightComponent>(Actor, NAME_None, RF_Transient);
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetIntensityUnits(ELightUnits::Lumens);
			Component->SetupAttachment(Actor->GetRootComponent());
			Component->RegisterComponent();
			AllLights.Add(Component);
			Models.Lights.Add(Component);
		}
		Component->SetWorldLocation(Lights[Index].Location);
		Component->SetCastShadows(CVarLightShadows.GetValueOnGameThread());
		Component->SetIntensity(Def->LightLumens);
		Component->SetLightColor(Def->LightColor, /*bSRGB*/ false);
		Component->SetAttenuationRadius(Def->LightRadius * MadFall::VoxelSizeUU);
	}
}

// ===========================================================================
// Assets
// ===========================================================================

UStaticMesh* UMadModelInstanceSubsystem::GetMesh(uint16 BlockId)
{
	if (const TObjectPtr<UStaticMesh>* Cached = Meshes.Find(BlockId))
	{
		return *Cached;
	}

	UStaticMesh* Mesh = nullptr;
	const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(BlockId);
	if (Def != nullptr && !Def->Mesh.IsNull())
	{
		// Synchronous once per block type per session, like surface materials:
		// a frame's hitch the first time a barrel streams in, rather than an
		// invisible barrel until an async load lands.
		Mesh = Cast<UStaticMesh>(Def->Mesh.TryLoad());
		if (Mesh == nullptr)
		{
			UE_LOG(LogMadFallMesher, Warning, TEXT("%s: model block %s names mesh '%s', which did not load; drawing a cube."),
				*Def->SourcePath, *Def->Id.ToString(), *Def->Mesh.ToString());
		}
	}
	else if (Def != nullptr)
	{
		UE_LOG(LogMadFallMesher, Warning, TEXT("%s: model block %s has no render.mesh; drawing a cube."), *Def->SourcePath, *Def->Id.ToString());
	}
	if (Mesh == nullptr)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, FallbackMeshPath);
	}

	Meshes.Add(BlockId, Mesh);
	return Mesh;
}

UMaterialInterface* UMadModelInstanceSubsystem::GetMaterial(uint16 BlockId)
{
	if (const TObjectPtr<UMaterialInterface>* Cached = Materials.Find(BlockId))
	{
		return *Cached;
	}

	UMaterialInterface* Material = nullptr;
	const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(BlockId);
	if (Def != nullptr && !Def->Material.IsNull())
	{
		Material = Cast<UMaterialInterface>(Def->Material.TryLoad());
		if (Material == nullptr)
		{
			UE_LOG(LogMadFallMesher, Warning, TEXT("%s: model block %s names material '%s', which did not load; using its surface colour."),
				*Def->SourcePath, *Def->Id.ToString(), *Def->Material.ToString());
		}
	}

	if (Material == nullptr)
	{
		// No material of its own: the block's surface colour and pattern, drawn
		// from the mesh's local position like the block in the survivor's hand,
		// so a wooden barrel has planks and a bush has leaves. A flat tint read as
		// a placeholder next to patterned blocks. The engine's tinted material
		// remains the fallback without the asset.
		const FColor Color = MadFall::GetSurfaces().GetVertexColor(Def ? Def->MaterialClass : NAME_None);
		// GetVertexColor stores linear values in bytes; read them back as linear.
		const FLinearColor Linear(Color.R / 255.0f, Color.G / 255.0f, Color.B / 255.0f, 1.0f);
		if (UMaterialInterface* Pattern = LoadObject<UMaterialInterface>(nullptr, PatternMaterialPath))
		{
			UMaterialInstanceDynamic* Patterned = UMaterialInstanceDynamic::Create(Pattern, this);
			Patterned->SetVectorParameterValue(TEXT("Color"), Linear);
			Patterned->SetScalarParameterValue(TEXT("PatternAlpha"), Color.A / 255.0f);
			Material = Patterned;
		}
		else if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TintMaterialPath))
		{
			UMaterialInstanceDynamic* Tinted = UMaterialInstanceDynamic::Create(Base, this);
			Tinted->SetVectorParameterValue(TEXT("Color"), Linear);
			Material = Tinted;
		}
	}

	Materials.Add(BlockId, Material);
	return Material;
}

AActor* UMadModelInstanceSubsystem::GetOrCreateActor()
{
	if (ModelActor != nullptr)
	{
		return ModelActor;
	}
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.Name = MakeUniqueObjectName(World->PersistentLevel, AActor::StaticClass(), TEXT("MadFallModels"));
	Params.ObjectFlags |= RF_Transient;
	ModelActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (ModelActor != nullptr)
	{
		USceneComponent* Root = NewObject<USceneComponent>(ModelActor, TEXT("Root"));
		Root->SetMobility(EComponentMobility::Static);
		Root->RegisterComponent();
		ModelActor->SetRootComponent(Root);
	}
	return ModelActor;
}

// ===========================================================================
// Diagnostics
// ===========================================================================

int32 UMadModelInstanceSubsystem::NumComponents() const
{
	int32 Count = 0;
	for (const TObjectPtr<UInstancedStaticMeshComponent>& Component : AllComponents)
	{
		Count += Component != nullptr ? 1 : 0;
	}
	return Count;
}

int32 UMadModelInstanceSubsystem::NumLights() const
{
	int32 Count = 0;
	for (const TObjectPtr<UPointLightComponent>& Light : AllLights)
	{
		Count += Light != nullptr ? 1 : 0;
	}
	return Count;
}

int32 UMadModelInstanceSubsystem::NumFlames() const
{
	int32 Count = 0;
	for (const TObjectPtr<UInstancedStaticMeshComponent>& Flames : AllFlames)
	{
		Count += Flames != nullptr ? Flames->GetInstanceCount() : 0;
	}
	return Count;
}

void UMadModelInstanceSubsystem::UpdateFlames(FChunkModels& Models, const TArray<MadFall::Models::FBlockLight>& Lights)
{
	TArray<FTransform> Transforms;
	Transforms.Reserve(Lights.Num());
	for (const MadFall::Models::FBlockLight& Light : Lights)
	{
		const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(Light.BlockId);
		const float Size = Def != nullptr ? Def->LightFlame : 0.0f;
		if (Size > 0.0f)
		{
			// The engine cone is 100 uu tall and 100 across, pivot at its centre: the
			// flame is centred on the light, so a light placed above a torch's stick
			// is not half buried in it.
			Transforms.Emplace(FQuat::Identity, Light.Location, FVector(0.1 * Size, 0.1 * Size, 0.2 * Size));
		}
	}

	UInstancedStaticMeshComponent* Flames = Models.Flames.Get();
	if (Transforms.Num() == 0 || !FApp::CanEverRender())
	{
		if (Flames != nullptr)
		{
			AllFlames.Remove(Flames);
			Flames->DestroyComponent();
			Models.Flames.Reset();
		}
		return;
	}
	if (Flames == nullptr)
	{
		UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_MadFlame.M_MadFlame"));
		AActor* Actor = GetOrCreateActor();
		if (Cone == nullptr || Material == nullptr || Actor == nullptr)
		{
			return;
		}
		Flames = NewObject<UInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient);
		Flames->SetStaticMesh(Cone);
		Flames->SetMaterial(0, Material);
		Flames->SetMobility(EComponentMobility::Static);
		Flames->SetCastShadow(false);
		Flames->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Flames->bAffectDistanceFieldLighting = false;
		// Culled like small props: a torch flame a chunk away is a few pixels.
		Flames->SetCullDistances(0, 6000);
		Flames->SetupAttachment(Actor->GetRootComponent());
		Flames->RegisterComponent();
		Models.Flames = Flames;
		AllFlames.Add(Flames);
	}
	Flames->ClearInstances();
	Flames->AddInstances(Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
}

int32 UMadModelInstanceSubsystem::NumInstances() const
{
	int32 Count = 0;
	for (const TObjectPtr<UInstancedStaticMeshComponent>& Component : AllComponents)
	{
		Count += Component != nullptr ? Component->GetInstanceCount() : 0;
	}
	return Count;
}

FString UMadModelInstanceSubsystem::DescribeStats() const
{
	return FString::Printf(TEXT("Models: %d instances in %d components, %d lights, %d flames, over %d chunks; %d dirty; %lld rebuilds, worst %.2f ms"),
		NumInstances(), NumComponents(), NumLights(), NumFlames(), Chunks.Num(), Dirty.Num(), TotalRebuilds, WorstRebuildMs);
}

static FAutoConsoleCommandWithWorld GMadModelsStatsCommand(
	TEXT("mad.models.stats"),
	TEXT("Model block instances: counts and worst rebuild cost."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadModelInstanceSubsystem* Subsystem = World ? World->GetSubsystem<UMadModelInstanceSubsystem>() : nullptr)
		{
			Subsystem->FlushNow();
			UE_LOG(LogMadFallMesher, Display, TEXT("%s"), *Subsystem->DescribeStats());
		}
	}));

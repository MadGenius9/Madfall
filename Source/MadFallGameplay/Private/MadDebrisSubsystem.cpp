// Copyright MadFall. All Rights Reserved.

#include "MadDebrisSubsystem.h"

#include "MadAudioSubsystem.h"

#include "MadFrameBudget.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "HAL/IConsoleManager.h"
#include "MadBlockRegistry.h"
#include "MadDamageable.h"
#include "MadFallGameplay.h"
#include "MadFallStats.h"
#include "MadGameplayDefinitions.h"
#include "MadPickupSubsystem.h"
#include "MadPlayerCharacter.h"
#include "MadSurvivorComponents.h"
#include "MadZombie.h"
#include "MadStructuralSubsystem.h"
#include "MadVoxelWorldSubsystem.h"
#include "Materials/MaterialInterface.h"

DECLARE_CYCLE_STAT(TEXT("Debris Tick"), STAT_MadDebrisTick, STATGROUP_MadFallStructural);
DECLARE_CYCLE_STAT(TEXT("Debris Land"), STAT_MadDebrisLand, STATGROUP_MadFallStructural);

namespace
{
	/**
	 * Hit points of crush damage per kilojoule of impact energy.
	 *
	 * Calibration: a 1800 kg concrete block dropped one storey (4 m) carries
	 * ~70 kJ, so 10 HP/kJ is ~700 HP split over whatever it lands on - enough to
	 * break a concrete frame (520 HP) under a single block, not enough to punch
	 * through bedrock. A collapsing floor lands with many blocks' worth of
	 * energy spread over many contacts, so floors pancake onto floors roughly
	 * the way players expect.
	 */
	TAutoConsoleVariable<float> CVarDamagePerKJ(
		TEXT("mad.debris.DamagePerKJ"),
		10.0f,
		TEXT("Crush damage (hit points) dealt per kilojoule of debris impact energy."),
		ECVF_Default);

	/**
	 * Hit points per kilojoule when debris falls on a survivor or zombie.
	 * Lower than the block figure because a body is not a rigid support that
	 * absorbs the whole impact: a 250 kg wood frame dropping 3 m (~7 kJ) costs
	 * a survivor about 22 health, while an 1800 kg concrete block from a storey
	 * up (~70 kJ) is lethal, as it should be.
	 */
	TAutoConsoleVariable<float> CVarPawnDamagePerKJ(
		TEXT("mad.debris.PawnDamagePerKJ"),
		3.0f,
		TEXT("Damage dealt to survivors and zombies per kilojoule of falling debris."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarRubbleKeepOneIn(
		TEXT("mad.debris.RubbleKeepOneIn"),
		3,
		TEXT("Roughly one collapsed block in N is left behind as rubble. 0 disables rubble."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarMaxVisualClusters(
		TEXT("mad.debris.MaxVisualClusters"),
		64,
		TEXT("Falling clusters beyond this count fall without a visual actor."),
		ECVF_Default);

	const FName CrushDamageType(TEXT("madfall:crush"));
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void UMadDebrisSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	VoxelWorld = Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
	Structural = Collection.InitializeDependency<UMadStructuralSubsystem>();

	if (Structural != nullptr)
	{
		CollapseHandle = Structural->OnStructureCollapsed().AddUObject(this, &UMadDebrisSubsystem::HandleCollapse);
		StrainHandle = Structural->OnStructureStrained().AddUObject(this, &UMadDebrisSubsystem::HandleStrain);
	}
}

void UMadDebrisSubsystem::Deinitialize()
{
	if (Structural != nullptr)
	{
		Structural->OnStructureCollapsed().Remove(CollapseHandle);
		Structural->OnStructureStrained().Remove(StrainHandle);
	}

	for (FFalling& Item : Falling)
	{
		if (AActor* Actor = Item.Visual.Get())
		{
			Actor->Destroy();
		}
	}
	Falling.Reset();

	Structural = nullptr;
	VoxelWorld = nullptr;
	Super::Deinitialize();
}

TStatId UMadDebrisSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadDebrisSubsystem, STATGROUP_Tickables);
}

// ===========================================================================
// Collapse intake
// ===========================================================================

void UMadDebrisSubsystem::HandleCollapse(const TArray<FMadStructuralFailureRecord>& Failures)
{
	if (Structural == nullptr)
	{
		return;
	}

	TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters(Failures, Structural->GetMaterials());

	for (FMadDebrisCluster& Cluster : Clusters)
	{
		FFalling& Item = Falling.AddDefaulted_GetRef();
		Item.Cluster = MoveTemp(Cluster);

		++Stats.ClustersSpawned;
		Stats.BlocksFallen += Item.Cluster.Blocks.Num();

		if (Falling.Num() <= CVarMaxVisualClusters.GetValueOnGameThread())
		{
			Item.Visual = SpawnVisual(Item.Cluster);
		}
	}
}

void UMadDebrisSubsystem::HandleStrain(const FMadStressSample& Member, const FMadVoxel& Voxel)
{
	UMadAudioSubsystem* Audio = GetWorld() ? GetWorld()->GetSubsystem<UMadAudioSubsystem>() : nullptr;
	const FMadBlockDefinitionData* Block = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(Voxel.BlockTypeID);
	if (Audio == nullptr || Block == nullptr)
	{
		return;
	}
	// 85% of the limit is a quiet complaint, at the limit a loud one.
	const float Volume = FMath::GetMappedRangeValueClamped(FVector2f(0.85f, 1.0f), FVector2f(0.45f, 1.1f), Member.Stress);
	Audio->PlayForMaterial(EMadSound::Creak, Block->MaterialClass, (FVector(Member.Position) + FVector(0.5)) * MadFall::VoxelSizeUU, Volume);
}

void UMadDebrisSubsystem::KeepFalling(FMadDebrisCluster&& Cluster, TArray<TWeakObjectPtr<AActor>> HitPawns)
{
	FFalling& Item = Falling.AddDefaulted_GetRef();
	Item.Cluster = MoveTemp(Cluster);
	// A pawn the whole piece already hit is not hit again by the part that breaks off.
	Item.HitPawns = MoveTemp(HitPawns);
	++Stats.ClustersSheared;
	if (Falling.Num() <= CVarMaxVisualClusters.GetValueOnGameThread())
	{
		Item.Visual = SpawnVisual(Item.Cluster);
		if (AActor* Actor = Item.Visual.Get())
		{
			Actor->SetActorLocation(FVector(0.0, 0.0, -Item.Cluster.FallDistance * MadFall::VoxelSizeUU));
		}
	}
}

bool UMadDebrisSubsystem::IsFree(const FIntVector& Position) const
{
	if (VoxelWorld == nullptr || Position.Z < MadFall::WorldMinZ)
	{
		return false;
	}
	if (Position.Z > MadFall::WorldMaxZ)
	{
		return true;
	}

	// Debris lands on the edge of the loaded world rather than falling into it:
	// writing rubble into an unloaded chunk would silently load and dirty it.
	if (!VoxelWorld->IsVoxelLoaded(Position.X, Position.Y, Position.Z))
	{
		return false;
	}

	const FMadVoxel Voxel = VoxelWorld->GetVoxel(Position.X, Position.Y, Position.Z);
	return !Voxel.IsSolid() || Voxel.HasFlag(EMadVoxelFlags::Liquid);
}

AActor* UMadDebrisSubsystem::SpawnVisual(const FMadDebrisCluster& Cluster)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube == nullptr)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (Actor == nullptr)
	{
		return nullptr;
	}

	UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(Actor, TEXT("DebrisInstances"));
	Instances->SetMobility(EComponentMobility::Movable);
	Instances->SetStaticMesh(Cube);
	Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	if (IConsoleVariable* MaterialVar = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.mesh.SectionMaterial")))
	{
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialVar->GetString()))
		{
			Instances->SetMaterial(0, Material);
		}
	}

	Actor->SetRootComponent(Instances);
	Instances->RegisterComponent();

	// Engine cube is 100 uu with its pivot at the centre; a voxel spans
	// [x, x+1) * VoxelSizeUU, so instances sit at the voxel centre.
	TArray<FTransform> Transforms;
	Transforms.Reserve(Cluster.Blocks.Num());
	for (const FMadDebrisBlock& Block : Cluster.Blocks)
	{
		const FVector Centre = (FVector(Block.Position) + FVector(0.5)) * MadFall::VoxelSizeUU;
		Transforms.Add(FTransform(FQuat::Identity, Centre, FVector(MadFall::VoxelSizeUU / 100.0f)));
	}
	Instances->AddInstances(Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);

	return Actor;
}

// ===========================================================================
// Fall and land
// ===========================================================================

void UMadDebrisSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Debris);

	if (Falling.Num() == 0)
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_MadDebrisTick);

	// Clamp so a hitch does not teleport debris through a floor's worth of
	// voxels in one step - Advance checks every voxel it passes, but a huge dt
	// would still land things at a nonsensical speed.
	const float Dt = FMath::Min(DeltaTime, 1.0f / 20.0f);
	auto Free = [this](const FIntVector& P) { return IsFree(P); };

	for (int32 Index = 0; Index < Falling.Num();)
	{
		FFalling& Item = Falling[Index];
		const int32 DroppedBefore = Item.Cluster.Dropped;
		const bool bLanded = MadFall::Debris::Advance(Item.Cluster, Dt, Free);
		if (Item.Cluster.Dropped > DroppedBefore)
		{
			DamagePawns(Item, DroppedBefore);
		}

		if (AActor* Actor = Item.Visual.Get())
		{
			Actor->SetActorLocation(FVector(0.0, 0.0, -Item.Cluster.FallDistance * MadFall::VoxelSizeUU));
		}

		if (bLanded)
		{
			// Columns over open air shear off and fall on; the rest lands.
			FMadDebrisCluster Rest = MadFall::Debris::SplitUnsupported(Item.Cluster, Free);
			Land(Item);
			TArray<TWeakObjectPtr<AActor>> HitPawns = MoveTemp(Item.HitPawns);
			Falling.RemoveAtSwap(Index, EAllowShrinking::No);
			if (Rest.Blocks.Num() > 0)
			{
				KeepFalling(MoveTemp(Rest), MoveTemp(HitPawns));
			}
			continue;
		}
		++Index;
	}
}

void UMadDebrisSubsystem::FlushNow()
{
	auto Free = [this](const FIntVector& P) { return IsFree(P); };

	// Landing can cause another collapse, which appends more clusters; loop
	// until the whole cascade has settled. Bounded against a runaway.
	for (int32 Round = 0; Round < 64 && (Falling.Num() > 0 || (Structural && !Structural->IsIdle())); ++Round)
	{
		if (Structural != nullptr)
		{
			Structural->FlushNow();
		}

		TArray<FFalling> Batch = MoveTemp(Falling);
		Falling.Reset();

		for (FFalling& Item : Batch)
		{
			MadFall::Debris::AdvanceToLanding(Item.Cluster, Free);
			FMadDebrisCluster Rest = MadFall::Debris::SplitUnsupported(Item.Cluster, Free);
			Land(Item);
			if (Rest.Blocks.Num() > 0)
			{
				KeepFalling(MoveTemp(Rest), Item.HitPawns);   // picked up by the next round
			}
		}
	}
}

void UMadDebrisSubsystem::Land(FFalling& Item)
{
	SCOPE_CYCLE_COUNTER(STAT_MadDebrisLand);

	if (AActor* Actor = Item.Visual.Get())
	{
		Actor->Destroy();
	}
	Item.Visual.Reset();

	++Stats.ClustersLanded;

	if (VoxelWorld == nullptr || Structural == nullptr)
	{
		return;
	}

	const FMadDebrisCluster& Cluster = Item.Cluster;
	auto Free = [this](const FIntVector& P) { return IsFree(P); };

	TArray<FMadDebrisImpact> Impacts;
	MadFall::Debris::ComputeImpacts(Cluster, Free, Impacts);

	// The crash of a landing structure, where its first block hit.
	if (Impacts.Num() > 0)
	{
		if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
		{
			Audio->PlayAt(EMadSound::Collapse, (FVector(Impacts[0].Position) + FVector(0.5)) * MadFall::VoxelSizeUU,
				FMath::Clamp(0.4f + Cluster.Blocks.Num() / 20.0f, 0.4f, 1.5f));
		}
	}

	const float DamagePerKJ = CVarDamagePerKJ.GetValueOnGameThread();
	for (const FMadDebrisImpact& Impact : Impacts)
	{
		const float KJ = Impact.EnergyJ / 1000.0f;
		Stats.TotalEnergyKJ += KJ;
		Stats.LargestImpactKJ = FMath::Max(Stats.LargestImpactKJ, static_cast<double>(KJ));
		++Stats.Impacts;

		// Through the structural subsystem's damage path, so a crushed support
		// is re-solved and can bring down whatever it was holding.
		Structural->ApplyBlockDamage(Impact.Position, KJ * DamagePerKJ, CrushDamageType);
	}

	const int32 KeepOneIn = CVarRubbleKeepOneIn.GetValueOnGameThread();
	// Only rubble that was actually written counts as the block's remains; a
	// block whose rubble had nowhere to go (or no rubble block) drops loot instead.
	TArray<TPair<FIntVector, int32>> PlacedRubble;
	TArray<TPair<FIntVector, int32>> Rubble;
	if (KeepOneIn > 0)
	{
		const FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();

		// Where the living stand. Rubble used to settle there too, so a survivor
		// who lived through the hit woke up inside a pile and had to dig out -
		// or suffocated in a doorway. Their voxels stay open; that block's share
		// becomes salvage like rubble with nowhere to go.
		TSet<FIntVector> Occupied;
		for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
		{
			const IMadDamageable* Damageable = Cast<IMadDamageable>(*It);
			const UCapsuleComponent* Capsule = It->GetCapsuleComponent();
			if (Capsule == nullptr || (Damageable != nullptr && Damageable->IsDead()))
			{
				continue;
			}
			// Half a voxel of skin: rubble is isosurface terrain and bulges about that far
			// into the voxels beside it. Written right against the capsule, the pile
			// overlapped it, and resolving the overlap shoved a boxed-in survivor down
			// through the rock under them (measured: 22 voxels, into solid granite).
			TArray<FIntVector> Voxels;
			MadFall::Debris::GetPawnVoxels(It->GetActorLocation(), Capsule->GetScaledCapsuleHalfHeight(), Capsule->GetScaledCapsuleRadius(), Voxels,
				0.5f * MadFall::VoxelSizeUU);
			Occupied.Append(Voxels);
		}

		MadFall::Debris::ComputeRubble(Cluster, KeepOneIn, Rubble);

		for (const TPair<FIntVector, int32>& Entry : Rubble)
		{
			const FMadBlockDefinitionData* Def = Registry.FindDefinition(Cluster.Blocks[Entry.Value].Voxel.BlockTypeID);
			if (Def == nullptr || Def->DebrisOnCollapse.IsNone() || !Registry.IsRegistered(Def->DebrisOnCollapse))
			{
				continue;
			}

			// The impacts above may just have destroyed what the cluster landed
			// on, so let rubble settle further. Entries come bottom-up per
			// column, so each one settles onto the one written before it.
			FIntVector P = Entry.Key;
			if (!IsFree(P))
			{
				continue;
			}
			for (int32 Settle = 0; Settle < 64 && IsFree(P - FIntVector(0, 0, 1)); ++Settle)
			{
				P.Z -= 1;
			}
			if (Occupied.Contains(P))
			{
				++Stats.RubbleSparedPawns;
				continue;
			}

			// Rubble is natural (not Cubic): it never takes part in the
			// structure and meshes as a rough isosurface pile.
			FMadVoxel Voxel;
			Voxel.BlockTypeID = Registry.ResolveRuntimeId(Def->DebrisOnCollapse);
			Voxel.Density = 255;
			Voxel.Damage = 0;
			Voxel.Rotation = 0;
			Voxel.Flags = 0;

			if (VoxelWorld->SetVoxel(P.X, P.Y, P.Z, Voxel))
			{
				++Stats.RubblePlaced;
				PlacedRubble.Emplace(P, Entry.Value);
			}
		}
	}

	DropCollapseLoot(Cluster, PlacedRubble);

	UE_LOG(LogMadFallStructural, Log, TEXT("Debris landed: %d block(s), %.0f kg, fell %d m at %.1f m/s, %d impact(s)."),
		Cluster.Blocks.Num(), Cluster.MassKg, Cluster.Dropped, Cluster.Velocity, Impacts.Num());

	LandedDelegate.Broadcast(Cluster, Impacts);
}

void UMadDebrisSubsystem::DamagePawns(FFalling& Item, int32 DroppedBefore)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const float PawnDamagePerKJ = CVarPawnDamagePerKJ.GetValueOnGameThread();
	for (TActorIterator<ACharacter> It(World); It; ++It)
	{
		ACharacter* Character = *It;
		if (Item.HitPawns.Contains(Character))
		{
			continue;
		}

		const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
		const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 90.0f;
		const FVector Location = Character->GetActorLocation();
		const FIntVector Feet(
			FMath::FloorToInt32(Location.X / MadFall::VoxelSizeUU),
			FMath::FloorToInt32(Location.Y / MadFall::VoxelSizeUU),
			FMath::FloorToInt32((Location.Z - HalfHeight + 1.0) / MadFall::VoxelSizeUU));
		const int32 Height = FMath::Max(1, FMath::CeilToInt32(2.0f * HalfHeight / MadFall::VoxelSizeUU));

		const float EnergyJ = MadFall::Debris::ComputeSweptHitEnergy(Item.Cluster, DroppedBefore, Feet, Height);
		if (EnergyJ <= 0.0f)
		{
			continue;
		}

		Item.HitPawns.Add(Character);
		const float Damage = EnergyJ / 1000.0f * PawnDamagePerKJ;
		++Stats.PawnHits;

		if (AMadPlayerCharacter* Player = Cast<AMadPlayerCharacter>(Character))
		{
			if (Player->IsDown())
			{
				continue;
			}
			Player->GetSurvival()->ApplyDamage(Damage);
			Player->PushMessage(TEXT("Crushed by falling debris!"), 2.5f);
		}
		else if (AMadZombie* Zombie = Cast<AMadZombie>(Character))
		{
			Zombie->ReceiveHit(Damage, CrushDamageType, nullptr);
		}

		UE_LOG(LogMadFallGameplay, Display, TEXT("Debris hit %s for %.0f damage (%.1f kJ)."), *Character->GetName(), Damage, EnergyJ / 1000.0f);
	}
}

void UMadDebrisSubsystem::DropCollapseLoot(const FMadDebrisCluster& Cluster, const TArray<TPair<FIntVector, int32>>& Rubble)
{
	UMadPickupSubsystem* Pickups = GetWorld() ? GetWorld()->GetSubsystem<UMadPickupSubsystem>() : nullptr;
	if (Pickups == nullptr || Cluster.Blocks.Num() == 0)
	{
		return;
	}

	const FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	auto CollapseTableOf = [&Registry, &Definitions](uint16 BlockTypeId) -> const FMadLootTableDefinition*
	{
		const FMadBlockDefinitionData* Def = Registry.FindDefinition(BlockTypeId);
		return Def ? Definitions.FindLootTable(Def->DropTableOnCollapse) : nullptr;
	};

	// Seeded from where the cluster came from, so the same collapse always
	// leaves the same pile.
	const FIntVector& Origin = Cluster.Blocks[0].Position;
	FRandomStream Random(static_cast<int32>(HashCombineFast(HashCombineFast(GetTypeHash(Origin.X), GetTypeHash(Origin.Y)), GetTypeHash(Origin.Z))));

	TArray<FMadItemStack> Stacks;
	MadFall::Debris::RollCollapseDrops(Cluster, Rubble, CollapseTableOf, Definitions, Random, Stacks);
	if (Stacks.Num() == 0)
	{
		return;
	}

	// One bag on top of the pile, over the middle of the footprint: a dozen
	// bags scattered through rubble would be buried or lost.
	FVector Sum = FVector::ZeroVector;
	int32 LowestZ = MAX_int32;
	for (int32 Index = 0; Index < Cluster.Blocks.Num(); ++Index)
	{
		const FIntVector Landed = Cluster.GetLandedPosition(Index);
		Sum += FVector(Landed);
		LowestZ = FMath::Min(LowestZ, Landed.Z);
	}
	const FVector Mean = Sum / Cluster.Blocks.Num();
	FIntVector Spot(FMath::FloorToInt32(Mean.X), FMath::FloorToInt32(Mean.Y), LowestZ);
	for (int32 Up = 0; Up < 64 && !IsFree(Spot); ++Up)
	{
		Spot.Z += 1;
	}

	const FVector Location = (FVector(Spot) + FVector(0.5, 0.5, 0.1)) * MadFall::VoxelSizeUU;
	if (Pickups->Drop(Location, Stacks) != nullptr)
	{
		for (const FMadItemStack& Stack : Stacks)
		{
			Stats.ItemsDropped += Stack.Count;
		}
	}
}

FString UMadDebrisSubsystem::DescribeStatus() const
{
	return FString::Printf(
		TEXT("Debris: %d falling; %lld clusters spawned, %lld sheared off, %lld landed, %lld blocks fallen\n")
		TEXT("  %lld impacts, %.1f kJ total, largest %.1f kJ; %lld rubble placed, %lld kept out of a pawn's space; %lld pawn hit(s), %lld item(s) dropped"),
		Falling.Num(), Stats.ClustersSpawned, Stats.ClustersSheared, Stats.ClustersLanded, Stats.BlocksFallen,
		Stats.Impacts, Stats.TotalEnergyKJ, Stats.LargestImpactKJ, Stats.RubblePlaced, Stats.RubbleSparedPawns, Stats.PawnHits, Stats.ItemsDropped);
}

// ===========================================================================
// Console
// ===========================================================================

namespace
{
	UMadDebrisSubsystem* GetDebris(UWorld* World)
	{
		UMadDebrisSubsystem* Subsystem = World ? World->GetSubsystem<UMadDebrisSubsystem>() : nullptr;
		if (Subsystem == nullptr)
		{
			UE_LOG(LogMadFallStructural, Error, TEXT("No debris subsystem in this world."));
		}
		return Subsystem;
	}

	FAutoConsoleCommandWithWorld CmdDebrisStatus(
		TEXT("mad.debris.status"),
		TEXT("Falling debris counters."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadDebrisSubsystem* D = GetDebris(World))
			{
				UE_LOG(LogMadFallStructural, Display, TEXT("%s"), *D->DescribeStatus());
			}
		}));

	FAutoConsoleCommandWithWorld CmdDebrisFlush(
		TEXT("mad.debris.flush"),
		TEXT("Settles every pending collapse and falling cluster now, including cascades."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadDebrisSubsystem* D = GetDebris(World))
			{
				D->FlushNow();
				UE_LOG(LogMadFallStructural, Display, TEXT("%s"), *D->DescribeStatus());
			}
		}));
}

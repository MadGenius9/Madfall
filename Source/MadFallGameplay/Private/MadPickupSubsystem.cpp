// Copyright MadFall. All Rights Reserved.

#include "MadPickupSubsystem.h"

#include "MadAudioSubsystem.h"

#include "MadFrameBudget.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "MadSurfaceMaterials.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadFallGameplay.h"
#include "MadGameplayDefinitions.h"
#include "MadPlayerCharacter.h"
#include "MadSurvivorComponents.h"
#include "MadVoxelWorldSubsystem.h"
#include "Materials/MaterialInterface.h"

namespace
{
	constexpr float CollectRadiusCm = 150.0f;
	constexpr float LooseLifetime = 900.0f;
	constexpr float BackpackLifetime = 3600.0f;
}

// ===========================================================================
// Actor
// ===========================================================================

AMadItemPickup::AMadItemPickup()
{
	PrimaryActorTick.bCanEverTick = true;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetRelativeScale3D(FVector(0.3));
	SetRootComponent(Mesh);
}

bool AMadItemPickup::IsEmpty() const
{
	return !Stacks.ContainsByPredicate([](const FMadItemStack& Stack) { return !Stack.IsEmpty(); });
}

void AMadItemPickup::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (Mesh->GetStaticMesh() == nullptr)
	{
		// A tied cloth bundle, in the surface materials the world is made of; a
		// white engine cube read as a missing asset, which is what it was.
		UStaticMesh* Bundle = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Models/Props/SM_Pickup.SM_Pickup"));
		Mesh->SetStaticMesh(Bundle != nullptr ? Bundle : LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		if (Bundle != nullptr)
		{
			const TArray<FStaticMaterial>& Slots = Bundle->GetStaticMaterials();
			for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
			{
				if (UMaterialInterface* Surface = MadFall::SurfaceMaterials::MakeHeld(this, Slots[Slot].MaterialSlotName, 1.0f))
				{
					Mesh->SetMaterial(Slot, Surface);
				}
			}
			// A backpack is the same bundle, bigger.
			Mesh->SetRelativeScale3D(FVector(bIsBackpack ? 1.35 : 0.85));
		}
		else
		{
			if (IConsoleVariable* MaterialVar = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.mesh.SectionMaterial")))
			{
				if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialVar->GetString()))
				{
					Mesh->SetMaterial(0, Material);
				}
			}
			Mesh->SetRelativeScale3D(FVector(bIsBackpack ? 0.5 : 0.3));
		}
	}

	Lifetime -= DeltaSeconds;
	if (Lifetime <= 0.0f || IsEmpty())
	{
		Destroy();
		return;
	}

	Spin = FMath::Fmod(Spin + DeltaSeconds * 90.0f, 360.0f);
	SetActorRotation(FRotator(0.0, Spin, 0.0));

	// Settle onto the voxel below, and fall again if it is dug out. Twice a
	// second is plenty for something that is not moving on its own.
	SettleTimer -= DeltaSeconds;
	if (SettleTimer > 0.0f)
	{
		return;
	}
	SettleTimer = 0.5f;

	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return;
	}

	FVector Location = GetActorLocation();
	const int32 X = FMath::FloorToInt32(Location.X / MadFall::VoxelSizeUU);
	const int32 Y = FMath::FloorToInt32(Location.Y / MadFall::VoxelSizeUU);
	int32 Z = FMath::FloorToInt32((Location.Z - 20.0) / MadFall::VoxelSizeUU);

	for (int32 Fall = 0; Fall < 64 && Z > MadFall::WorldMinZ; ++Fall)
	{
		if (!VoxelWorld->IsVoxelLoaded(X, Y, Z - 1) || VoxelWorld->GetVoxel(X, Y, Z - 1).IsSolid())
		{
			break;
		}
		--Z;
	}
	// Climb out of a block placed on top of it.
	for (int32 Rise = 0; Rise < 8 && VoxelWorld->GetVoxel(X, Y, Z).IsSolid(); ++Rise)
	{
		++Z;
	}

	Location.Z = Z * MadFall::VoxelSizeUU + 20.0;

	// The voxel grid is where the ground is cut, not where it is drawn: the
	// smooth surface of a hill sits below the voxel's floor, and a bundle left
	// on the grid hung a hand's width to a metre over it (a playtest photo
	// showed two hanging over a stag). Rest it on the surface itself when there
	// is one under it; the grid answer stands in while collision is still cooking.
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MadPickupSettle), false, this);
	if (GetWorld()->LineTraceSingleByChannel(Hit, Location + FVector(0.0, 0.0, MadFall::VoxelSizeUU),
			Location - FVector(0.0, 0.0, MadFall::VoxelSizeUU * 1.5), ECC_WorldStatic, Params))
	{
		Location.Z = Hit.ImpactPoint.Z + 14.0;
	}
	SetActorLocation(Location);
}

// ===========================================================================
// Subsystem
// ===========================================================================

bool UMadPickupSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadPickupSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadPickupSubsystem, STATGROUP_Tickables);
}

AMadItemPickup* UMadPickupSubsystem::Drop(const FVector& Location, const TArray<FMadItemStack>& Stacks, bool bBackpack)
{
	TArray<FMadItemStack> NonEmpty = Stacks.FilterByPredicate([](const FMadItemStack& Stack) { return !Stack.IsEmpty(); });
	if (NonEmpty.Num() == 0)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AMadItemPickup* Pickup = GetWorld()->SpawnActor<AMadItemPickup>(AMadItemPickup::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (Pickup == nullptr)
	{
		return nullptr;
	}

	Pickup->Stacks = MoveTemp(NonEmpty);
	Pickup->bIsBackpack = bBackpack;
	Pickup->Lifetime = bBackpack ? BackpackLifetime : LooseLifetime;
	Pickups.Add(Pickup);
	return Pickup;
}

int32 UMadPickupSubsystem::CollectNear(AMadPlayerCharacter& Player, float RadiusCm)
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FVector PlayerLocation = Player.GetActorLocation();
	const float RadiusSq = RadiusCm * RadiusCm;
	int32 Collected = 0;

	for (const TWeakObjectPtr<AMadItemPickup>& Weak : Pickups)
	{
		AMadItemPickup* Pickup = Weak.Get();
		// Pickups are measured from the player's feet: they lie on the ground.
		if (Pickup == nullptr || FVector::DistSquared(Pickup->GetActorLocation(), PlayerLocation - FVector(0.0, 0.0, 70.0)) > RadiusSq)
		{
			continue;
		}

		FMadInventory& Inventory = Player.GetInventory()->GetInventory();
		for (FMadItemStack& Stack : Pickup->Stacks)
		{
			if (Stack.IsEmpty())
			{
				continue;
			}
			const int32 Left = Inventory.Add(Stack, Definitions);
			Collected += Stack.Count - Left;
			Stack.Count = Left;
		}
		Player.GetInventory()->NotifyChanged();
	}

	if (Collected > 0)
	{
		Player.PushMessage(FString::Printf(TEXT("Picked up %d item(s)."), Collected), 1.5f);
		if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
		{
			Audio->Play2D(EMadSound::Pickup, 0.6f);
		}
	}
	return Collected;
}

void UMadPickupSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Other);

	Pickups.RemoveAll([](const TWeakObjectPtr<AMadItemPickup>& Pickup) { return !Pickup.IsValid() || Pickup->IsEmpty(); });

	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	AMadPlayerCharacter* Player = Controller ? Cast<AMadPlayerCharacter>(Controller->GetPawn()) : nullptr;
	if (Player != nullptr && !Player->IsDown() && Pickups.Num() > 0)
	{
		CollectNear(*Player, CollectRadiusCm);
	}
}

int32 UMadPickupSubsystem::NumPickups() const
{
	return Pickups.FilterByPredicate([](const TWeakObjectPtr<AMadItemPickup>& Pickup) { return Pickup.IsValid() && !Pickup->IsEmpty(); }).Num();
}

void UMadPickupSubsystem::ExportState(TArray<FMadPickupSaveData>& Out) const
{
	Out.Reset();
	for (const TWeakObjectPtr<AMadItemPickup>& Weak : Pickups)
	{
		if (const AMadItemPickup* Pickup = Weak.Get(); Pickup && !Pickup->IsEmpty())
		{
			FMadPickupSaveData& Data = Out.AddDefaulted_GetRef();
			Data.Location = Pickup->GetActorLocation();
			Data.Stacks = Pickup->Stacks;
			Data.Lifetime = Pickup->Lifetime;
			Data.bIsBackpack = Pickup->bIsBackpack;
		}
	}
}

void UMadPickupSubsystem::ImportState(const TArray<FMadPickupSaveData>& In)
{
	for (const FMadPickupSaveData& Data : In)
	{
		if (AMadItemPickup* Pickup = Drop(Data.Location, Data.Stacks, Data.bIsBackpack))
		{
			Pickup->Lifetime = Data.Lifetime;
		}
	}
}

// Copyright MadFall. All Rights Reserved.

#include "MadProjectile.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "MadAudioSubsystem.h"
#include "MadDamageable.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadGameplayDefinitions.h"
#include "MadInventory.h"
#include "MadPickupSubsystem.h"
#include "MadVoxelRaycast.h"
#include "MadVoxelWorldSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

int32 AMadProjectile::TotalFired = 0;
int32 AMadProjectile::TotalCreatureHits = 0;
int32 AMadProjectile::TotalWorldHits = 0;

void MadFall::Projectile::Step(FVector& InOutLocation, FVector& InOutVelocity, float GravityScale, float DeltaSeconds)
{
	InOutVelocity.Z -= GravityCm * GravityScale * DeltaSeconds;
	InOutLocation += InOutVelocity * DeltaSeconds;
}

AMadProjectile::AMadProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	// After physics, so the pawn trace sees this frame's creature positions.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Shaft = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Shaft"));
	RootComponent = Shaft;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cylinder.Succeeded())
	{
		Shaft->SetStaticMesh(Cylinder.Object);
	}
	Shaft->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Shaft->SetCastShadow(false);
	// A cylinder is 100 uu tall along Z: 70 cm long and thin, its long axis turned along the flight.
	Shaft->SetRelativeScale3D(FVector(0.015, 0.015, 0.7));
}

void AMadProjectile::Launch(AActor* InShooter, const FVector& InVelocity, float InGravityScale, float InDamage, FName InDamageType,
	FName InAmmo, float InRecoverChance)
{
	Shooter = InShooter;
	Velocity = InVelocity;
	GravityScale = InGravityScale;
	Damage = InDamage;
	DamageType = InDamageType;
	Ammo = InAmmo;
	RecoverChance = FMath::Clamp(InRecoverChance, 0.0f, 1.0f);
	bLaunched = true;
	++TotalFired;

	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, this);
		Material->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.12f, 0.08f, 0.04f));
		Shaft->SetMaterial(0, Material);
	}
	// The cylinder's long axis is Z; tip it over so it points along the flight.
	SetActorRotation(FRotationMatrix::MakeFromZ(Velocity.GetSafeNormal()).Rotator());
}

void AMadProjectile::SetColour(const FLinearColor& Colour)
{
	if (UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Shaft->GetMaterial(0)))
	{
		Material->SetVectorParameterValue(TEXT("Color"), Colour);
	}
}

void AMadProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bLaunched)
	{
		return;
	}

	MAD_FRAME_SCOPE(Player);

	Age += DeltaSeconds;
	UWorld* World = GetWorld();
	const UMadVoxelWorldSubsystem* VoxelWorld = World->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (Age > MaxLifetime || VoxelWorld == nullptr)
	{
		Destroy();
		return;
	}

	const FVector From = GetActorLocation();
	FVector To = From;
	FVector NewVelocity = Velocity;
	MadFall::Projectile::Step(To, NewVelocity, GravityScale, FMath::Min(DeltaSeconds, 0.05f));

	const FVector FromVoxels = From / MadFall::VoxelSizeUU;
	const FVector ToVoxels = To / MadFall::VoxelSizeUU;
	if (!VoxelWorld->IsVoxelLoaded(FMath::FloorToInt32(ToVoxels.X), FMath::FloorToInt32(ToVoxels.Y), FMath::FloorToInt32(ToVoxels.Z)))
	{
		Destroy();
		return;
	}

	// The world: first solid voxel along the segment.
	const float SegmentVoxels = static_cast<float>((ToVoxels - FromVoxels).Size());
	FMadVoxelHit VoxelHit;
	const bool bHitVoxel = SegmentVoxels > KINDA_SMALL_NUMBER && MadFall::VoxelRaycast(FromVoxels, ToVoxels - FromVoxels, SegmentVoxels,
		[VoxelWorld](const FIntVector& V) { return VoxelWorld->GetVoxel(V.X, V.Y, V.Z).IsSolid(); }, VoxelHit);
	const float VoxelDistanceCm = bHitVoxel ? VoxelHit.Distance * MadFall::VoxelSizeUU : TNumericLimits<float>::Max();

	// Creatures: a pawn trace over the same segment, ignoring whoever fired it.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MadProjectile), false, this);
	if (AActor* ShooterActor = Shooter.Get())
	{
		Params.AddIgnoredActor(ShooterActor);
	}
	FHitResult PawnHit;
	bool bHitPawn = World->LineTraceSingleByChannel(PawnHit, From, To, ECC_Pawn, Params);
	// A shot never hurts its own kind: a spitter's acid flies through the horde in front of it.
	for (int32 Retry = 0; Retry < 4 && bHitPawn && Shooter.IsValid() && PawnHit.GetActor() != nullptr
		&& PawnHit.GetActor()->GetClass() == Shooter->GetClass(); ++Retry)
	{
		Params.AddIgnoredActor(PawnHit.GetActor());
		bHitPawn = World->LineTraceSingleByChannel(PawnHit, From, To, ECC_Pawn, Params);
	}
	if (bHitPawn && PawnHit.Distance < VoxelDistanceCm && !(Shooter.IsValid() && PawnHit.GetActor() && PawnHit.GetActor()->GetClass() == Shooter->GetClass()))
	{
		if (IMadDamageable* Victim = Cast<IMadDamageable>(PawnHit.GetActor()); Victim != nullptr && !Victim->IsDead())
		{
			++TotalCreatureHits;
			if (UMadAudioSubsystem* Audio = World->GetSubsystem<UMadAudioSubsystem>())
			{
				Audio->PlayAt(EMadSound::FleshHit, PawnHit.ImpactPoint);
			}
			const bool bKilled = Victim->ReceiveHit(Damage, DamageType, Shooter.Get());
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s hit %s for %.0f%s."), Ammo.IsNone() ? TEXT("Spit") : TEXT("Arrow"), *PawnHit.GetActor()->GetName(), Damage, bKilled ? TEXT(", killing it") : TEXT(""));
			Destroy();
			return;
		}
	}

	if (bHitVoxel)
	{
		++TotalWorldHits;
		const FVector Impact = From + (To - From).GetSafeNormal() * VoxelDistanceCm;
		if (UMadAudioSubsystem* Audio = World->GetSubsystem<UMadAudioSubsystem>())
		{
			Audio->PlayAt(EMadSound::HitWood, Impact, 0.5f);
		}
		// Some arrows survive to be pulled out of the ground and used again.
		if (!Ammo.IsNone() && FMath::FRand() < RecoverChance)
		{
			const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
			if (const FMadItemDefinition* Item = Definitions.FindItem(Ammo))
			{
				if (UMadPickupSubsystem* Pickups = World->GetSubsystem<UMadPickupSubsystem>())
				{
					// Backed off the face it struck, so the bag is not inside the block.
					Pickups->Drop(Impact - (To - From).GetSafeNormal() * 20.0f, { FMadItemStack::Make(*Item, 1) });
				}
			}
		}
		UE_LOG(LogMadFallGameplay, Verbose, TEXT("Arrow struck voxel %s."), *VoxelHit.Voxel.ToString());
		Destroy();
		return;
	}

	Velocity = NewVelocity;
	SetActorLocationAndRotation(To, FRotationMatrix::MakeFromZ(Velocity.GetSafeNormal()).Rotator());
}

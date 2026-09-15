// Copyright MadFall. All Rights Reserved.

#include "MadQuadrupedRig.h"

#include "MadBasicShapes.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	using MadFall::BasicShapes::CubeMesh;
	using MadFall::BasicShapes::TintMaterial;
	// Fur and eyes drawn like the humanoids' skin and faces (Scripts/make_character_material.py).
	using MadFall::BasicShapes::CharacterMaterial;

	constexpr float QuadAttackPoseSeconds = 0.4f;
	constexpr float QuadHitSeconds = 0.15f;
	constexpr float QuadDeathSeconds = 0.5f;
}

FMadQuadrupedPose MadFall::Quadruped::ComputePose(float Phase, float SpeedFactor, float Graze, float Attack, float Death)
{
	FMadQuadrupedPose Pose;
	SpeedFactor = FMath::Clamp(SpeedFactor, 0.0f, 1.0f);
	Graze = FMath::Clamp(Graze, 0.0f, 1.0f);
	Attack = FMath::Clamp(Attack, 0.0f, 1.0f);
	Death = FMath::Clamp(Death, 0.0f, 1.0f);

	// A trot: front-left with back-right, front-right with back-left. The swing
	// widens with speed, up to a stretched gallop.
	const float Swing = FMath::Sin(Phase) * (18.0f + 22.0f * SpeedFactor) * FMath::Min(1.0f, SpeedFactor * 4.0f);
	Pose.FrontLeft = Swing;
	Pose.BackRight = Swing;
	Pose.FrontRight = -Swing;
	Pose.BackLeft = -Swing;

	Pose.Bob = FMath::Abs(FMath::Sin(Phase)) * 4.0f * SpeedFactor;

	// The head dips to graze only when still, and drops for a butt.
	const float Stillness = 1.0f - FMath::Min(1.0f, SpeedFactor * 3.0f);
	const float Butt = Attack < 1.0f ? FMath::Sin(Attack * UE_PI) : 0.0f;
	Pose.Head = -55.0f * Graze * Stillness - 35.0f * Butt + 4.0f * FMath::Sin(Phase * 2.0f) * SpeedFactor;

	// Keels over onto its side.
	Pose.Fall = 90.0f * Death * Death;
	return Pose;
}

UMadQuadrupedRigComponent::UMadQuadrupedRigComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UMadQuadrupedRigComponent::SetShape(const FVector& InBodySize, float InLegLength, float InNeckLength)
{
	BodySize = InBodySize.ComponentMax(FVector(8.0));
	LegLength = FMath::Max(4.0f, InLegLength);
	NeckLength = FMath::Max(0.0f, InNeckLength);
}

USceneComponent* UMadQuadrupedRigComponent::MakeJoint(const TCHAR* Name, USceneComponent* Parent, const FVector& Location)
{
	USceneComponent* Component = NewObject<USceneComponent>(GetOwner(), Name, RF_Transient);
	Component->SetupAttachment(Parent);
	Component->SetRelativeLocation(Location);
	Component->RegisterComponent();
	return Component;
}

UStaticMeshComponent* UMadQuadrupedRigComponent::MakePart(const TCHAR* Name, USceneComponent* Parent,
	const FVector& Location, const FVector& ScaleCm, EPart Kind)
{
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, CharacterMaterial);
	bPatterned = Base != nullptr;
	if (Base == nullptr)
	{
		Base = LoadObject<UMaterialInterface>(nullptr, TintMaterial);
	}
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, this);
	Material->SetVectorParameterValue(TEXT("Size"), FLinearColor(ScaleCm.X, ScaleCm.Y, ScaleCm.Z));
	// M_MadCharacter parts: 4 fur, 5 an animal's head.
	Material->SetScalarParameterValue(TEXT("Part"), Kind == EPart::Head ? 5.0f : 4.0f);
	Material->SetScalarParameterValue(TEXT("Zombie"), 0.0f);
	Material->SetScalarParameterValue(TEXT("Seed"), static_cast<float>(GetOwner() ? GetOwner()->GetUniqueID() % 997 : 0));
	PartMaterials.Add(Material);
	PartKinds.Add(Kind);

	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(GetOwner(), Name, RF_Transient);
	Part->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, CubeMesh));
	Part->SetMaterial(0, Material);
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetCanEverAffectNavigation(false);
	Part->SetupAttachment(Parent);
	Part->SetRelativeLocation(Location);
	Part->SetRelativeScale3D(ScaleCm / 100.0f);
	Part->RegisterComponent();
	Parts.Add(Part);
	return Part;
}

void UMadQuadrupedRigComponent::Build()
{
	if (bBuilt || GetOwner() == nullptr || GetWorld() == nullptr || !GetWorld()->IsGameWorld())
	{
		return;
	}
	bBuilt = true;

	const double HalfLength = BodySize.X * 0.5;
	const double HalfWidth = BodySize.Y * 0.5;
	const double LegThickness = FMath::Clamp(BodySize.Y * 0.28, 4.0, 14.0);
	const double LegInset = LegThickness * 0.6;

	// Root at the feet; the body's underside sits on top of the legs. +X is forward.
	Root = MakeJoint(TEXT("QuadRoot"), this, FVector::ZeroVector);
	Body = MakeJoint(TEXT("QuadBody"), Root, FVector(0.0, 0.0, LegLength));
	MakePart(TEXT("QuadTorso"), Body, FVector(0.0, 0.0, BodySize.Z * 0.5), BodySize, EPart::Coat);

	// Hips and shoulders at the underside corners; each leg hangs from its joint.
	FrontLeft = MakeJoint(TEXT("QuadFrontLeft"), Body, FVector(HalfLength - LegInset, -HalfWidth + LegInset, 4.0));
	FrontRight = MakeJoint(TEXT("QuadFrontRight"), Body, FVector(HalfLength - LegInset, HalfWidth - LegInset, 4.0));
	BackLeft = MakeJoint(TEXT("QuadBackLeft"), Body, FVector(-HalfLength + LegInset, -HalfWidth + LegInset, 4.0));
	BackRight = MakeJoint(TEXT("QuadBackRight"), Body, FVector(-HalfLength + LegInset, HalfWidth - LegInset, 4.0));
	const FVector LegScale(LegThickness, LegThickness, LegLength + 4.0);
	const FVector LegOffset(0.0, 0.0, -(LegLength + 4.0) * 0.5);
	MakePart(TEXT("QuadLegFL"), FrontLeft, LegOffset, LegScale, EPart::Dark);
	MakePart(TEXT("QuadLegFR"), FrontRight, LegOffset, LegScale, EPart::Dark);
	MakePart(TEXT("QuadLegBL"), BackLeft, LegOffset, LegScale, EPart::Dark);
	MakePart(TEXT("QuadLegBR"), BackRight, LegOffset, LegScale, EPart::Dark);

	// The neck rises forward from the top front of the body and carries the head.
	Neck = MakeJoint(TEXT("QuadNeck"), Body, FVector(HalfLength * 0.8, 0.0, BodySize.Z * 0.75));
	const double NeckThickness = FMath::Max(BodySize.Y * 0.45, 6.0);
	if (NeckLength > 1.0f)
	{
		UStaticMeshComponent* NeckPart = MakePart(TEXT("QuadNeckPart"), Neck,
			FVector(NeckLength * 0.35, 0.0, NeckLength * 0.35), FVector(NeckThickness, NeckThickness, NeckLength), EPart::Coat);
		NeckPart->SetRelativeRotation(FRotator(-40.0, 0.0, 0.0));
	}
	const double HeadSize = FMath::Max(BodySize.Z * 0.55, 8.0);
	const FVector HeadAt(NeckLength * 0.7 + HeadSize * 0.4, 0.0, NeckLength * 0.7);
	MakePart(TEXT("QuadHead"), Neck, HeadAt, FVector(HeadSize * 1.4, HeadSize * 0.8, HeadSize * 0.8), EPart::Head);
	MakePart(TEXT("QuadSnout"), Neck, HeadAt + FVector(HeadSize * 0.8, 0.0, -HeadSize * 0.15),
		FVector(HeadSize * 0.5, HeadSize * 0.5, HeadSize * 0.45), EPart::Dark);

	MakePart(TEXT("QuadTail"), Body, FVector(-HalfLength - 3.0, 0.0, BodySize.Z * 0.8),
		FVector(8.0, FMath::Max(BodySize.Y * 0.2, 4.0), FMath::Max(BodySize.Z * 0.25, 4.0)), EPart::Dark);
	ApplyTint(0.0f);
}

void UMadQuadrupedRigComponent::SetColour(const FLinearColor& Coat)
{
	CoatColour = Coat;
	ApplyTint(HitFlash > 0.0f ? 1.0f : 0.0f);
}

void UMadQuadrupedRigComponent::ApplyTint(float Flash)
{
	const FLinearColor Hit(0.9f, 0.05f, 0.03f);
	// A pale belly, as most coats have.
	const FLinearColor Belly = FMath::Lerp(CoatColour, FLinearColor(0.45f, 0.4f, 0.32f), 0.55f);
	for (int32 Index = 0; Index < PartMaterials.Num(); ++Index)
	{
		UMaterialInstanceDynamic* Material = PartMaterials[Index];
		if (Material == nullptr)
		{
			continue;
		}
		// Legs, snout and tail a darker shade of the coat.
		const bool bDark = PartKinds[Index] == EPart::Dark;
		const FLinearColor Colour = bDark ? CoatColour * 0.45f : CoatColour;
		if (!bPatterned)
		{
			Material->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(Colour, Hit, bDark ? Flash * 0.6f : Flash));
			continue;
		}
		Material->SetVectorParameterValue(TEXT("Color"), Colour);
		Material->SetVectorParameterValue(TEXT("Accent"), bDark ? Colour : Belly);
		Material->SetScalarParameterValue(TEXT("Flash"), bDark ? Flash * 0.6f : Flash);
	}
}

void UMadQuadrupedRigComponent::PlayAttack()
{
	AttackProgress = 0.0f;
}

void UMadQuadrupedRigComponent::PlayHit()
{
	HitFlash = QuadHitSeconds;
	ApplyTint(1.0f);
}

void UMadQuadrupedRigComponent::PlayDeath()
{
	bDying = true;
}

void UMadQuadrupedRigComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bBuilt)
	{
		static uint64 BuildFrame = 0;
		static int32 BuiltThisFrame = 0;
		if (BuildFrame != GFrameCounter)
		{
			BuildFrame = GFrameCounter;
			BuiltThisFrame = 0;
		}
		if (BuiltThisFrame >= 2)
		{
			return;
		}
		++BuiltThisFrame;
		Build();
	}
	if (Root == nullptr)
	{
		return;
	}

	if (HitFlash > 0.0f)
	{
		HitFlash -= DeltaTime;
		ApplyTint(HitFlash > 0.0f ? HitFlash / QuadHitSeconds : 0.0f);
	}
	AttackProgress = FMath::Min(1.0f, AttackProgress + DeltaTime / QuadAttackPoseSeconds);
	GrazeAmount = FMath::FInterpConstantTo(GrazeAmount, bGrazing ? 1.0f : 0.0f, DeltaTime, 1.5f);
	if (bDying)
	{
		DeathProgress = FMath::Min(1.0f, DeathProgress + DeltaTime / QuadDeathSeconds);
	}

	if (!bDying && GetOwner() != nullptr && !GetOwner()->WasRecentlyRendered(0.25f))
	{
		return;
	}

	const AActor* Owner = GetOwner();
	const float Speed = Owner ? static_cast<float>(Owner->GetVelocity().Size2D()) : 0.0f;
	if (!bDying)
	{
		// One gait cycle per two leg lengths of ground covered: short legs patter.
		const float CycleCm = FMath::Max(20.0f, LegLength * 2.2f);
		Phase = FMath::Fmod(Phase + Speed * DeltaTime / CycleCm * UE_TWO_PI, UE_TWO_PI);
	}

	const FMadQuadrupedPose Pose = MadFall::Quadruped::ComputePose(Phase, Speed / GallopSpeed, GrazeAmount, AttackProgress, DeathProgress);
	Root->SetRelativeRotation(FRotator(0.0, 0.0, Pose.Fall));
	Body->SetRelativeLocation(FVector(0.0, 0.0, LegLength + Pose.Bob));
	// A positive pitch swings a hanging leg's foot forward, matching the pose's sign.
	FrontLeft->SetRelativeRotation(FRotator(Pose.FrontLeft, 0.0, 0.0));
	FrontRight->SetRelativeRotation(FRotator(Pose.FrontRight, 0.0, 0.0));
	BackLeft->SetRelativeRotation(FRotator(Pose.BackLeft, 0.0, 0.0));
	BackRight->SetRelativeRotation(FRotator(Pose.BackRight, 0.0, 0.0));
	Neck->SetRelativeRotation(FRotator(Pose.Head, 0.0, 0.0));
}

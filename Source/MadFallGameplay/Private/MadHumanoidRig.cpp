// Copyright MadFall. All Rights Reserved.

#include "MadHumanoidRig.h"

#include "MadBasicShapes.h"
#include "MadCharacterAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	using MadFall::BasicShapes::CubeMesh;
	using MadFall::BasicShapes::TintMaterial;
	using MadFall::BasicShapes::CharacterMaterial;

	/** Centimetres of stride per walk cycle. */
	constexpr float StrideCm = 90.0f;
	constexpr float HumanAttackPoseSeconds = 0.45f;
	constexpr float HumanHitSeconds = 0.15f;
	constexpr float HumanDeathSeconds = 0.6f;

	TAutoConsoleVariable<int32> CVarSkeletalCharacters(
		TEXT("mad.characters.Skeletal"),
		1,
		TEXT("Draw humanoids as the UE5 mannequin when its assets are installed (Scripts/copy_mannequin.ps1). 0 draws the box figures. Applies to characters built after the change."),
		ECVF_Default);

	const TCHAR* MannyPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
	const TCHAR* QuinnPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple");
}

FMadHumanoidPose MadFall::Humanoid::ComputePose(float Phase, float SpeedFactor, float Attack, float Death)
{
	FMadHumanoidPose Pose;
	SpeedFactor = FMath::Clamp(SpeedFactor, 0.0f, 1.0f);
	Attack = FMath::Clamp(Attack, 0.0f, 1.0f);
	Death = FMath::Clamp(Death, 0.0f, 1.0f);

	const float Swing = FMath::Sin(Phase);
	Pose.LeftLeg = 32.0f * Swing * SpeedFactor;
	Pose.RightLeg = -Pose.LeftLeg;

	// Arms reach forward, the classic shamble, swaying against the legs; an
	// attack raises them and brings them down across one swing.
	const float Reach = -80.0f + 8.0f * FMath::Sin(Phase + UE_PI) * SpeedFactor;
	const float Strike = Attack < 1.0f ? -55.0f * FMath::Sin(Attack * UE_PI) : 0.0f;
	Pose.LeftArm = Reach + Strike;
	Pose.RightArm = Reach + Strike + 6.0f * Swing * SpeedFactor;

	Pose.TorsoLean = -12.0f - 6.0f * SpeedFactor;
	Pose.HeadNod = 8.0f * FMath::Sin(Phase * 0.5f) - 10.0f;
	Pose.Bob = FMath::Abs(Swing) * 3.0f * SpeedFactor;

	// Topples onto its face, easing in like a falling body.
	Pose.Fall = -90.0f * Death * Death;
	return Pose;
}

UMadHumanoidRigComponent::UMadHumanoidRigComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

UStaticMeshComponent* UMadHumanoidRigComponent::MakePart(const TCHAR* Name, USceneComponent* Parent,
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
	Material->SetScalarParameterValue(TEXT("Part"), static_cast<float>(Kind));
	PartMaterials.Add(Material);
	PartKinds.Add(Kind);

	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(GetOwner(), Name, RF_Transient);
	Part->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, CubeMesh));
	Part->SetMaterial(0, Material);
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetCanEverAffectNavigation(false);
	Part->SetupAttachment(Parent);
	// Basic shapes are 100 uu on a side with a centred pivot.
	Part->SetRelativeLocation(Location);
	Part->SetRelativeScale3D(ScaleCm / 100.0f);
	Part->RegisterComponent();
	Parts.Add(Part);
	return Part;
}

void UMadHumanoidRigComponent::OnRegister()
{
	Super::OnRegister();
	// Built on a tick, not here: fourteen components registered inside SpawnActor
	// put a horde wave's whole construction on one frame (3.3 ms for a wave).
}

void UMadHumanoidRigComponent::Build()
{
	if (bBuilt || GetOwner() == nullptr || GetWorld() == nullptr || !GetWorld()->IsGameWorld())
	{
		return;
	}
	bBuilt = true;

	if (BuildSkeletal())
	{
		return;
	}

	auto Joint = [this](const TCHAR* Name, USceneComponent* Parent, const FVector& Location)
	{
		USceneComponent* Component = NewObject<USceneComponent>(GetOwner(), Name, RF_Transient);
		Component->SetupAttachment(Parent);
		Component->SetRelativeLocation(Location);
		Component->RegisterComponent();
		return Component;
	};

	// Proportions for a 180 cm figure, origin at the feet.
	Root = Joint(TEXT("RigRoot"), this, FVector::ZeroVector);
	Hips = Joint(TEXT("RigHips"), Root, FVector(0.0, 0.0, 92.0));
	LeftHip = Joint(TEXT("RigLeftHip"), Hips, FVector(0.0, -11.0, 0.0));
	RightHip = Joint(TEXT("RigRightHip"), Hips, FVector(0.0, 11.0, 0.0));
	Chest = Joint(TEXT("RigChest"), Hips, FVector(0.0, 0.0, 4.0));
	LeftShoulder = Joint(TEXT("RigLeftShoulder"), Chest, FVector(0.0, -27.0, 54.0));
	RightShoulder = Joint(TEXT("RigRightShoulder"), Chest, FVector(0.0, 27.0, 54.0));
	Neck = Joint(TEXT("RigNeck"), Chest, FVector(0.0, 0.0, 62.0));

	MakePart(TEXT("RigLeftLeg"), LeftHip, FVector(0.0, 0.0, -46.0), FVector(20.0, 20.0, 92.0), EPart::Trousers);
	MakePart(TEXT("RigRightLeg"), RightHip, FVector(0.0, 0.0, -46.0), FVector(20.0, 20.0, 92.0), EPart::Trousers);
	MakePart(TEXT("RigTorso"), Chest, FVector(0.0, 0.0, 30.0), FVector(26.0, 44.0, 64.0), EPart::Shirt);
	MakePart(TEXT("RigLeftArm"), LeftShoulder, FVector(0.0, 0.0, -32.0), FVector(15.0, 15.0, 66.0), EPart::Skin);
	MakePart(TEXT("RigRightArm"), RightShoulder, FVector(0.0, 0.0, -32.0), FVector(15.0, 15.0, 66.0), EPart::Skin);
	MakePart(TEXT("RigHead"), Neck, FVector(2.0, 0.0, 14.0), FVector(24.0, 24.0, 24.0), EPart::Head);
	ApplyTint(0.0f);
}

bool UMadHumanoidRigComponent::BuildSkeletal()
{
	// Nothing to see without a renderer, and animating a horde nobody can see
	// would only cost the server and CI.
	if (CVarSkeletalCharacters.GetValueOnGameThread() == 0 || !FApp::CanEverRender())
	{
		return false;
	}
	// Two builds, a man and a woman; a third of zombies and half the living are Quinn.
	const bool bQuinn = bLiving ? (Seed & 1) != 0 : (Seed % 3) == 0;
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, bQuinn ? QuinnPath : MannyPath);
	if (Mesh == nullptr)
	{
		return false;
	}

	USkeletalMeshComponent* Body = NewObject<USkeletalMeshComponent>(GetOwner(), TEXT("RigMannequin"), RF_Transient);
	Body->SetupAttachment(this);
	// The mannequin faces +Y and stands on its origin; the rig's origin is the feet, facing +X.
	Body->SetRelativeRotation(FRotator(0.0, -90.0, 0.0));
	Body->SetSkeletalMesh(Mesh);
	Body->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Body->SetAnimInstanceClass(UMadCharacterAnimInstance::StaticClass());
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	Body->SetCanEverAffectNavigation(false);
	// A horde behind the survivor is not evaluated, and distant ones update less
	// often: skeletal animation is the one per-character cost that grows with
	// the horde on the game and worker threads alike.
	Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	Body->bEnableUpdateRateOptimizations = true;
	// No collision, so no bodies to move with the bones, and a fixed bound from
	// the mesh rather than one refitted to the pose each frame: 30 zombies in
	// view cost 0.54 ms of game-thread animation a frame before, 0.46 after.
	Body->KinematicBonesUpdateType = EKinematicBonesUpdateToPhysics::SkipAllBones;
	Body->bComponentUseFixedSkelBounds = true;
	Body->RegisterComponent();

	UMadCharacterAnimInstance* Anim = Cast<UMadCharacterAnimInstance>(Body->GetAnimInstance());
	if (Anim == nullptr || !UMadCharacterAnimInstance::LoadSequences(*Anim))
	{
		Body->DestroyComponent();
		return false;
	}

	// The mannequin's material is a painted shell with a "Paint Tint": the first
	// slot (head, arms and legs) takes the skin colour, the second (torso) the
	// clothes, and a matte finish keeps a zombie from looking lacquered.
	for (int32 Slot = 0; Slot < Body->GetNumMaterials(); ++Slot)
	{
		UMaterialInterface* Base = Body->GetMaterial(Slot);
		if (Base == nullptr)
		{
			continue;
		}
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, this);
		Material->SetScalarParameterValue(TEXT("MetalPaintRoughness"), 0.8f);
		Material->SetScalarParameterValue(TEXT("MetalPaintMetallic"), 0.0f);
		Material->SetScalarParameterValue(TEXT("LogoScale"), 0.0f);
		Body->SetMaterial(Slot, Material);
		PartMaterials.Add(Material);
		PartKinds.Add(Slot == 0 ? EPart::Skin : EPart::Shirt);
	}
	Skeletal = Body;
	ApplyTint(0.0f);
	return true;
}

void UMadHumanoidRigComponent::SetColours(const FLinearColor& Skin, const FLinearColor& Clothes, const FLinearColor& Trousers, const FLinearColor& Hair)
{
	SkinColour = Skin;
	ClothesColour = Clothes;
	TrousersColour = Trousers;
	HairColour = Hair;
	ApplyTint(HitFlash > 0.0f ? 1.0f : 0.0f);
}

void UMadHumanoidRigComponent::SetLiving(bool bInLiving)
{
	bLiving = bInLiving;
	ApplyTint(HitFlash > 0.0f ? 1.0f : 0.0f);
}

void UMadHumanoidRigComponent::SetSeed(int32 InSeed)
{
	Seed = InSeed;
	ApplyTint(HitFlash > 0.0f ? 1.0f : 0.0f);
}

void UMadHumanoidRigComponent::ApplyTint(float Flash)
{
	const FLinearColor Hit(0.9f, 0.05f, 0.03f);
	for (int32 Index = 0; Index < PartMaterials.Num(); ++Index)
	{
		UMaterialInstanceDynamic* Material = PartMaterials[Index];
		if (Material == nullptr)
		{
			continue;
		}
		const EPart Kind = PartKinds[Index];
		const FLinearColor Colour = Kind == EPart::Shirt ? ClothesColour : (Kind == EPart::Trousers ? TrousersColour : SkinColour);
		if (Skeletal != nullptr)
		{
			Material->SetVectorParameterValue(TEXT("Paint Tint"), FMath::Lerp(Colour, Hit, Kind == EPart::Skin ? Flash : Flash * 0.6f));
			continue;
		}
		if (!bPatterned)
		{
			Material->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(Colour, Hit, Kind == EPart::Skin || Kind == EPart::Head ? Flash : Flash * 0.6f));
			continue;
		}
		Material->SetVectorParameterValue(TEXT("Color"), Colour);
		// A head's accent is its hair, an arm's its sleeve; clothes show the skin through tears and at the collar.
		Material->SetVectorParameterValue(TEXT("Accent"), Kind == EPart::Head ? HairColour : (Kind == EPart::Skin ? ClothesColour : SkinColour));
		Material->SetScalarParameterValue(TEXT("Zombie"), bLiving ? 0.0f : 1.0f);
		Material->SetScalarParameterValue(TEXT("Seed"), static_cast<float>(Seed % 997));
		Material->SetScalarParameterValue(TEXT("Flash"), Flash * (Kind == EPart::Skin || Kind == EPart::Head ? 1.0f : 0.6f));
	}
}

void UMadHumanoidRigComponent::PlayAttack()
{
	AttackProgress = 0.0f;
}

void UMadHumanoidRigComponent::PlayHit()
{
	HitFlash = HumanHitSeconds;
	SinceHit = 0.0f;
	ApplyTint(1.0f);
}

void UMadHumanoidRigComponent::PlayDeath()
{
	bDying = true;
}

void UMadHumanoidRigComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bBuilt)
	{
		// At most two rigs are assembled per frame across every zombie.
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
	if (Root == nullptr && Skeletal == nullptr)
	{
		return;
	}

	if (HitFlash > 0.0f)
	{
		HitFlash -= DeltaTime;
		ApplyTint(HitFlash > 0.0f ? HitFlash / HumanHitSeconds : 0.0f);
	}
	AttackProgress = FMath::Min(1.0f, AttackProgress + DeltaTime / HumanAttackPoseSeconds);
	if (bDying)
	{
		DeathProgress = FMath::Min(1.0f, DeathProgress + DeltaTime / (Skeletal != nullptr ? MadFall::CharacterAnim::DeathSeconds : HumanDeathSeconds));
	}

	if (Skeletal != nullptr)
	{
		SinceHit += DeltaTime;
		SinceDeath = bDying ? FMath::Max(SinceDeath, 0.0f) + DeltaTime : -1.0f;
		if (UMadCharacterAnimInstance* Anim = Cast<UMadCharacterAnimInstance>(Skeletal->GetAnimInstance()))
		{
			Anim->Inputs.Speed = GetOwner() != nullptr ? static_cast<float>(GetOwner()->GetVelocity().Size2D()) : 0.0f;
			Anim->Inputs.Attack = AttackProgress;
			Anim->Inputs.SinceHit = SinceHit;
			Anim->Inputs.SinceDeath = SinceDeath;
			Anim->Inputs.bZombie = !bLiving;
		}
		return;
	}

	// Posing is cheap but not free across a horde, and nobody sees a zombie
	// behind them. A dying one always finishes its fall.
	if (!bDying && GetOwner() != nullptr && !GetOwner()->WasRecentlyRendered(0.25f))
	{
		return;
	}

	const AActor* Owner = GetOwner();
	const float Speed = Owner ? static_cast<float>(Owner->GetVelocity().Size2D()) : 0.0f;
	if (!bDying)
	{
		Phase = FMath::Fmod(Phase + Speed * DeltaTime / StrideCm * UE_TWO_PI, UE_TWO_PI);
	}

	const FMadHumanoidPose Pose = MadFall::Humanoid::ComputePose(Phase, Speed / 300.0f, AttackProgress, DeathProgress);
	Root->SetRelativeRotation(FRotator(Pose.Fall, 0.0, 0.0));
	Hips->SetRelativeLocation(FVector(0.0, 0.0, 92.0 + Pose.Bob));
	// Limbs hang down, and a positive pitch swings a hanging limb forward, so the
	// pose's "negative is forward" is flipped for them.
	LeftHip->SetRelativeRotation(FRotator(-Pose.LeftLeg, 0.0, 0.0));
	RightHip->SetRelativeRotation(FRotator(-Pose.RightLeg, 0.0, 0.0));
	Chest->SetRelativeRotation(FRotator(Pose.TorsoLean, 0.0, 0.0));
	LeftShoulder->SetRelativeRotation(FRotator(-Pose.LeftArm, 0.0, 0.0));
	RightShoulder->SetRelativeRotation(FRotator(-Pose.RightArm, 0.0, 0.0));
	Neck->SetRelativeRotation(FRotator(Pose.HeadNod, 0.0, 0.0));
}

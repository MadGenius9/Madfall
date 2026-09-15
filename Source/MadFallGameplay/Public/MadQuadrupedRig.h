// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "MadGameplayDefinitions.h"
#include "MadQuadrupedRig.generated.h"

class UMaterialInstanceDynamic;
class USkeletalMeshComponent;
class UStaticMeshComponent;

/** A four-legged pose for one frame. Angles in degrees; positive swings a leg forward. */
struct FMadQuadrupedPose
{
	float FrontLeft = 0.0f;
	float FrontRight = 0.0f;
	float BackLeft = 0.0f;
	float BackRight = 0.0f;

	/** Head pitch, negative lowered (grazing, or a charge). */
	float Head = 0.0f;

	/** Vertical bob of the body, cm. */
	float Bob = 0.0f;

	/** Roll onto its side, 0 standing to 90 down. */
	float Fall = 0.0f;
};

namespace MadFall::Quadruped
{
	/**
	 * A walk or gallop. Pure so it is testable: Phase is the gait cycle in
	 * radians, SpeedFactor 0 (standing) to 1 (full gallop), Graze 0..1 lowers the
	 * head to the ground, Attack 0..1 through a head-down butt, Death 0..1 through
	 * the fall. Diagonal legs move together, as a trotting animal's do.
	 */
	MADFALLGAMEPLAY_API FMadQuadrupedPose ComputePose(float Phase, float SpeedFactor, float Graze, float Attack, float Death);
}

/**
 * An animal made of engine basic shapes, animated in code: the four-legged
 * sibling of UMadHumanoidRigComponent, for the same reason (no art, and a
 * capsule does not read as a deer).
 *
 * Proportions come from the animal definition - body length, width and height,
 * leg and neck length - so a rabbit, a deer and a boar are one rig. Built lazily
 * on tick (at most two per frame, shared with nothing else) and posed only when
 * recently rendered. The origin is the feet; attach it at the bottom of the capsule.
 */
UCLASS(ClassGroup = MadFall)
class MADFALLGAMEPLAY_API UMadQuadrupedRigComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UMadQuadrupedRigComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Body length, width, height in cm; leg and neck length in cm. Call before the first tick. */
	void SetShape(const FVector& InBodySize, float InLegLength, float InNeckLength);

	void SetColour(const FLinearColor& Coat);

	/** Gait speed, cm/s, at which the pose is a full gallop. */
	void SetGallopSpeed(float CmPerSecond) { GallopSpeed = FMath::Max(50.0f, CmPerSecond); }

	/** Head down to the grass while standing still. */
	void SetGrazing(bool bInGrazing) { bGrazing = bInGrazing; }

	/** An animated model to draw instead of the figure, fitted to the shape. Call before the first tick. */
	void SetModel(const FMadAnimalModel& InModel) { Model = InModel; }

	/**
	 * The transform that stands a model on the rig's origin at a target height:
	 * a uniform scale from the mesh's reference bounds and a lift that puts its
	 * lowest point on the ground. Pure, for tests.
	 */
	static FTransform FitModel(const FBoxSphereBounds& MeshBounds, float TargetHeight, float Yaw);

	void PlayAttack();
	void PlayHit();
	void PlayDeath();

	/** Height of the body's top above the feet, cm, for sizing the capsule. */
	static float GetStandingHeight(const FVector& BodySize, float LegLength) { return LegLength + BodySize.Z; }

private:
	void Build();
	/** The animated model; false (nothing built) without one, without a renderer, or if it does not load. */
	bool BuildSkeletal();
	void ApplyTint(float Flash);
	/** Part kinds: the coat, a darker shade of it (legs, snout, tail), and the head (with eyes). */
	enum class EPart : uint8 { Coat, Dark, Head };

	UStaticMeshComponent* MakePart(const TCHAR* Name, USceneComponent* Parent, const FVector& Location, const FVector& ScaleCm, EPart Kind);
	USceneComponent* MakeJoint(const TCHAR* Name, USceneComponent* Parent, const FVector& Location);

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Body;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> FrontLeft;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> FrontRight;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> BackLeft;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> BackRight;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Neck;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Parts;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> Skeletal;
	FMadAnimalModel Model;
	float AttackClipSeconds = 0.0f;
	float DeathClipSeconds = 0.0f;
	float SinceHit = 1000.0f;
	float SinceDeath = -1.0f;
	float AssetWaitSeconds = 0.0f;

	/** One per part (M_MadCharacter's Size differs per part), parallel to PartKinds. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> PartMaterials;
	TArray<EPart> PartKinds;
	bool bPatterned = false;

	FVector BodySize = FVector(90.0, 32.0, 40.0);
	float LegLength = 45.0f;
	float NeckLength = 25.0f;
	float GallopSpeed = 500.0f;
	FLinearColor CoatColour = FLinearColor(0.3f, 0.2f, 0.12f);

	bool bBuilt = false;
	bool bGrazing = false;
	float GrazeAmount = 0.0f;
	float Phase = 0.0f;
	float AttackProgress = 1.0f;
	float HitFlash = 0.0f;
	float DeathProgress = 0.0f;
	bool bDying = false;
};

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "MadCharacterAnimInstance.generated.h"

class UAnimSequence;

/** What the rig tells a character's animation each frame, on the game thread. */
struct FMadCharacterAnimInputs
{
	/** Ground speed, cm/s. */
	float Speed = 0.0f;
	/** 0..1 through an attack swing; 1 when not attacking. */
	float Attack = 1.0f;
	/** Seconds since the last hit; large when never hit. */
	float SinceHit = 1000.0f;
	/** Seconds since death began; negative while alive. */
	float SinceDeath = -1.0f;
	/** Arms reach forward and swing: a zombie. A living person walks with their arms down. */
	bool bZombie = true;
};

/** Locomotion weights and play rates for a ground speed. */
struct FMadCharacterBlend
{
	float Idle = 1.0f;
	float Walk = 0.0f;
	float Jog = 0.0f;
	float WalkRate = 1.0f;
	float JogRate = 1.0f;
};

namespace MadFall::CharacterAnim
{
	/** Ground speed the mannequin's walk and jog cycles cover at normal rate (their root motion over their length). */
	inline constexpr float WalkAnimSpeed = 300.0f;
	inline constexpr float JogAnimSpeed = 600.0f;

	/** Seconds the death animation takes to put the body on the ground. */
	inline constexpr float DeathSeconds = 1.1f;

	/**
	 * Idle, walk and jog weights for a ground speed (they sum to 1) and the rates
	 * that make the feet match it. Pure, for tests.
	 *
	 * A zombie walks at 1-1.4 m/s against a 3 m/s walk cycle: slowed below half
	 * rate a walk reads as a shamble, which is the look wanted, but not below 0.35,
	 * where it reads as slow motion.
	 */
	MADFALLGAMEPLAY_API FMadCharacterBlend ComputeBlend(float Speed);

	/**
	 * The component-space direction a zombie's upper arms reach (mesh forward is
	 * +Y): forward and a little down, raised through the first part of an attack
	 * and brought down across the rest, like the box rig's swing.
	 */
	MADFALLGAMEPLAY_API FVector ArmReachDirection(float Attack);
}

/**
 * Evaluates a mannequin character without an animation blueprint.
 *
 * WHY NATIVE AND NOT THE TEMPLATE'S ABP: the zombie shamble needs the arms held
 * out in front whatever the legs are doing, which the template graph has no
 * input for, and editing a copied Epic blueprint would put a binary asset we
 * cannot publish at the centre of the characters. A proxy that samples the
 * idle, walk and jog cycles, blends them by speed, and then turns the upper
 * arms towards a target direction is a hundred lines, runs on animation worker
 * threads like any graph, and is tested without assets (ComputeBlend,
 * ArmReachDirection). Hit reactions and the death fall are blended in over it.
 */
struct FMadCharacterAnimProxy : public FAnimInstanceProxy
{
	FMadCharacterAnimProxy() = default;
	explicit FMadCharacterAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

protected:
	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual void Update(float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	FMadCharacterAnimInputs Inputs;
	FMadCharacterBlend Blend;

	const UAnimSequence* Idle = nullptr;
	const UAnimSequence* Walk = nullptr;
	const UAnimSequence* Jog = nullptr;
	const UAnimSequence* HitReact = nullptr;
	const UAnimSequence* Death = nullptr;

	double IdleTime = 0.0;
	/** Walk and jog share one normalised phase, so blending between them keeps the feet in step. */
	double StridePhase = 0.0;
};

UCLASS(Transient, NotBlueprintable)
class MADFALLGAMEPLAY_API UMadCharacterAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** Set by the rig each frame. */
	FMadCharacterAnimInputs Inputs;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> Idle;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> Walk;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> Jog;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> HitReact;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> Death;

	/** Loads the mannequin animations; false if they are not installed (Scripts/copy_mannequin.ps1). */
	static bool LoadSequences(UMadCharacterAnimInstance& Instance);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override { return new FMadCharacterAnimProxy(this); }
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override { delete InProxy; }
	virtual void NativeInitializeAnimation() override;
};

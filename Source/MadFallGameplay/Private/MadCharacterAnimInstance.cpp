// Copyright MadFall. All Rights Reserved.

#include "MadCharacterAnimInstance.h"

#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"

namespace
{
	const TCHAR* IdlePath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle");
	const TCHAR* WalkPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Walk/MF_Unarmed_Walk_Fwd.MF_Unarmed_Walk_Fwd");
	const TCHAR* JogPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Jog/MF_Unarmed_Jog_Fwd.MF_Unarmed_Jog_Fwd");
	const TCHAR* HitReactPath = TEXT("/Game/Characters/Mannequins/Anims/Rifle/HitReact/MM_HitReact_Front_Lgt_01.MM_HitReact_Front_Lgt_01");
	const TCHAR* DeathPath = TEXT("/Game/Characters/Mannequins/Anims/Death/MM_Death_Front_01.MM_Death_Front_01");
	const TCHAR* MannyMeshPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
	const TCHAR* QuinnMeshPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple");

	TAutoConsoleVariable<int32> CVarSkeletalCharacters(
		TEXT("mad.characters.Skeletal"),
		1,
		TEXT("Draw humanoids as the UE5 mannequin (Scripts/copy_mannequin.ps1) and animals with a model as their animated model. 0 draws the box figures. Applies to characters built after the change."),
		ECVF_Default);

	/** The forward lean of a zombie's upper spine, degrees. */
	constexpr float ZombieLeanDegrees = 12.0f;

	FCompactPoseBoneIndex FindBone(const FBoneContainer& Bones, const TCHAR* Name)
	{
		const int32 MeshIndex = Bones.GetPoseBoneIndexForBoneName(FName(Name));
		return MeshIndex == INDEX_NONE ? FCompactPoseBoneIndex(INDEX_NONE) : Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
	}

	/**
	 * Turns a bone so the segment to its child points along Target (component
	 * space), Weight of the way, given the bone's parent's component rotation.
	 * Returns the bone's new component rotation, for turning the next bone down
	 * the chain.
	 */
	FQuat AimSegment(FCompactPose& Pose, FCompactPoseBoneIndex Bone, FCompactPoseBoneIndex Child, const FQuat& ParentRotation, const FVector& Target, float Weight)
	{
		FTransform& Local = Pose[Bone];
		const FQuat Rotation = ParentRotation * Local.GetRotation();
		const FVector Direction = Rotation.RotateVector(Pose[Child].GetTranslation()).GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			return Rotation;
		}
		const FQuat Delta = FQuat::Slerp(FQuat::Identity, FQuat::FindBetweenNormals(Direction, Target), Weight);
		const FQuat Aimed = (Delta * Rotation).GetNormalized();
		Local.SetRotation((ParentRotation.Inverse() * Aimed).GetNormalized());
		return Aimed;
	}

	/**
	 * Holds an arm out along Target (component space), Weight of the way: the
	 * upper arm, then the forearm. Turning only the upper arm left the walk
	 * cycle's bent elbow in place, and the hands ended up at the chest, a hug
	 * rather than a reach.
	 */
	void ReachArm(FCompactPose& Pose, FCSPose<FCompactPose>& ComponentPose, const TCHAR* UpperName, const TCHAR* LowerName, const TCHAR* HandName, const FVector& Target, float Weight)
	{
		const FBoneContainer& Bones = Pose.GetBoneContainer();
		const FCompactPoseBoneIndex Upper = FindBone(Bones, UpperName);
		const FCompactPoseBoneIndex Lower = FindBone(Bones, LowerName);
		const FCompactPoseBoneIndex Hand = FindBone(Bones, HandName);
		if (Upper.GetInt() == INDEX_NONE || Lower.GetInt() == INDEX_NONE || Hand.GetInt() == INDEX_NONE)
		{
			return;
		}
		const FCompactPoseBoneIndex Parent = Bones.GetParentBoneIndex(Upper);
		const FQuat ParentRotation = Parent.GetInt() == INDEX_NONE ? FQuat::Identity : ComponentPose.GetComponentSpaceTransform(Parent).GetRotation();
		const FQuat UpperRotation = AimSegment(Pose, Upper, Lower, ParentRotation, Target, Weight);
		// A little slack at the elbow: a dead-straight arm reads as a sleepwalker.
		AimSegment(Pose, Lower, Hand, UpperRotation, Target, Weight * 0.85f);
	}

	/** Tilts a bone forward about the component's X axis (forward is +Y). */
	void LeanBone(FCompactPose& Pose, FCSPose<FCompactPose>& ComponentPose, const TCHAR* Name, float Degrees)
	{
		const FBoneContainer& Bones = Pose.GetBoneContainer();
		const FCompactPoseBoneIndex Bone = FindBone(Bones, Name);
		if (Bone.GetInt() == INDEX_NONE)
		{
			return;
		}
		const FCompactPoseBoneIndex Parent = Bones.GetParentBoneIndex(Bone);
		const FQuat ParentRotation = Parent.GetInt() == INDEX_NONE ? FQuat::Identity : ComponentPose.GetComponentSpaceTransform(Parent).GetRotation();
		FTransform& Local = Pose[Bone];
		// A negative turn about +X takes +Z (up) towards +Y (forward).
		const FQuat Lean(FVector::XAxisVector, FMath::DegreesToRadians(-Degrees));
		Local.SetRotation((ParentRotation.Inverse() * Lean * ParentRotation * Local.GetRotation()).GetNormalized());
	}
}

bool MadFall::CharacterAnim::UseSkeletalBodies()
{
	return CVarSkeletalCharacters.GetValueOnGameThread() != 0 && FApp::CanEverRender();
}

FMadCharacterBlend MadFall::CharacterAnim::ComputeBlend(float Speed, float WalkCycleSpeed, float JogCycleSpeed)
{
	FMadCharacterBlend Result;
	Speed = FMath::Max(0.0f, Speed);
	WalkCycleSpeed = FMath::Max(1.0f, WalkCycleSpeed);
	JogCycleSpeed = FMath::Max(WalkCycleSpeed * 1.1f, JogCycleSpeed);
	// Moving past a fifth of a walk; the jog takes over across the first two
	// thirds of the gap between the cycles' speeds (for the mannequin, 300 to 500).
	const float Moving = FMath::Clamp(Speed / (WalkCycleSpeed * 0.2f), 0.0f, 1.0f);
	const float Jogging = FMath::Clamp((Speed - WalkCycleSpeed) / ((JogCycleSpeed - WalkCycleSpeed) * (2.0f / 3.0f)), 0.0f, 1.0f);
	Result.Idle = 1.0f - Moving;
	Result.Walk = Moving * (1.0f - Jogging);
	Result.Jog = Moving * Jogging;
	Result.WalkRate = FMath::Clamp(Speed / WalkCycleSpeed, 0.35f, 1.6f);
	Result.JogRate = FMath::Clamp(Speed / JogCycleSpeed, 0.6f, 1.5f);
	return Result;
}

FVector MadFall::CharacterAnim::ArmReachDirection(float Attack)
{
	Attack = FMath::Clamp(Attack, 0.0f, 1.0f);
	// Held a little below level; an attack lifts the arms 60 degrees and brings
	// them back down in one arc.
	const float Elevation = FMath::DegreesToRadians(-14.0f + 60.0f * FMath::Sin(Attack * UE_PI));
	return FVector(0.0, FMath::Cos(Elevation), FMath::Sin(Elevation));
}

// ===========================================================================
// Instance
// ===========================================================================

bool UMadCharacterAnimInstance::LoadSequences(UMadCharacterAnimInstance& Instance)
{
	Instance.Idle = LoadObject<UAnimSequence>(nullptr, IdlePath);
	Instance.Walk = LoadObject<UAnimSequence>(nullptr, WalkPath);
	Instance.Jog = LoadObject<UAnimSequence>(nullptr, JogPath);
	Instance.HitReact = LoadObject<UAnimSequence>(nullptr, HitReactPath);
	Instance.Death = LoadObject<UAnimSequence>(nullptr, DeathPath);
	return Instance.Idle != nullptr && Instance.Walk != nullptr && Instance.Jog != nullptr;
}

void UMadCharacterAnimInstance::GetMannequinPaths(TArray<FSoftObjectPath>& Out)
{
	for (const TCHAR* Path : { MannyMeshPath, QuinnMeshPath, IdlePath, WalkPath, JogPath, HitReactPath, DeathPath })
	{
		Out.Emplace(Path);
	}
}

void UMadCharacterAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	if (Idle == nullptr)
	{
		LoadSequences(*this);
	}
}

// ===========================================================================
// Proxy
// ===========================================================================

void FMadCharacterAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
	const UMadCharacterAnimInstance* Instance = CastChecked<UMadCharacterAnimInstance>(InAnimInstance);
	Inputs = Instance->Inputs;
	Idle = Instance->Idle;
	Walk = Instance->Walk;
	Jog = Instance->Jog;
	HitReact = Instance->HitReact;
	Death = Instance->Death;
	Attack = Instance->Attack;
	Graze = Instance->Graze;
	WalkCycleSpeed = Instance->WalkCycleSpeed;
	JogCycleSpeed = Instance->JogCycleSpeed;
}

void FMadCharacterAnimProxy::Update(float DeltaSeconds)
{
	Blend = MadFall::CharacterAnim::ComputeBlend(Inputs.Speed, WalkCycleSpeed, JogCycleSpeed);
	IdleTime += DeltaSeconds;

	const float Moving = Blend.Walk + Blend.Jog;
	if (Moving > 0.0f && Walk != nullptr && Jog != nullptr)
	{
		const double WalkCycles = Blend.WalkRate / FMath::Max(Walk->GetPlayLength(), 0.01f);
		const double JogCycles = Blend.JogRate / FMath::Max(Jog->GetPlayLength(), 0.01f);
		StridePhase = FMath::Fmod(StridePhase + DeltaSeconds * (Blend.Walk * WalkCycles + Blend.Jog * JogCycles) / Moving, 1.0);
	}
}

bool FMadCharacterAnimProxy::Evaluate(FPoseContext& Output)
{
	if (Idle == nullptr || Walk == nullptr || Jog == nullptr)
	{
		Output.ResetToRefPose();
		return true;
	}

	// Root motion is "extracted" (and dropped) so the root stays put: the capsule
	// moves the character, and a cycle that walked its own root forward would
	// slide ahead of it and snap back every loop.
	auto Sample = [](const UAnimSequence* Sequence, double Time, bool bLooping, FPoseContext& Into)
	{
		FAnimationPoseData PoseData(Into);
		Sequence->GetAnimationPose(PoseData, FAnimExtractContext(Time, /*bExtractRootMotion*/ true, FDeltaTimeRecord(), bLooping));
	};

	struct FLayer
	{
		const UAnimSequence* Sequence;
		double Time;
		bool bLooping;
		float Weight;
	};
	TArray<FLayer, TInlineAllocator<5>> Layers;
	const auto AddLayer = [&Layers](const UAnimSequence* Sequence, double Time, bool bLooping, float Weight)
	{
		if (Sequence != nullptr && Weight > 0.001f)
		{
			Layers.Add({ Sequence, Time, bLooping, Weight });
		}
	};

	// A death fall takes over within a fifth of a second; a hit reaction borrows
	// the body for its length, strongest in the middle.
	const float DeathWeight = Inputs.SinceDeath >= 0.0f && Death != nullptr ? FMath::Clamp(Inputs.SinceDeath / 0.2f, 0.0f, 1.0f) : 0.0f;
	float HitWeight = 0.0f;
	if (HitReact != nullptr && Inputs.SinceHit < HitReact->GetPlayLength())
	{
		HitWeight = 0.7f * FMath::Sin(UE_PI * Inputs.SinceHit / HitReact->GetPlayLength());
	}
	// A whole-body attack clip, where there is one, peaks early in the swing and
	// hands back to locomotion at its end.
	float AttackWeight = 0.0f;
	if (Attack != nullptr && Inputs.Attack < 1.0f)
	{
		AttackWeight = FMath::Min(1.0f, 2.5f * FMath::Sin(UE_PI * Inputs.Attack));
	}
	const float Locomotion = (1.0f - DeathWeight) * (1.0f - HitWeight) * (1.0f - AttackWeight);

	const bool bGrazing = Inputs.bGrazing && Graze != nullptr;
	const UAnimSequence* Resting = bGrazing ? Graze : Idle;
	AddLayer(Resting, FMath::Fmod(IdleTime, static_cast<double>(Resting->GetPlayLength())), true, Locomotion * Blend.Idle);
	AddLayer(Walk, StridePhase * Walk->GetPlayLength(), true, Locomotion * Blend.Walk);
	AddLayer(Jog, StridePhase * Jog->GetPlayLength(), true, Locomotion * Blend.Jog);
	AddLayer(HitReact, Inputs.SinceHit, false, (1.0f - DeathWeight) * HitWeight);
	if (AttackWeight > 0.0f)
	{
		AddLayer(Attack, Inputs.Attack * Attack->GetPlayLength(), false, (1.0f - DeathWeight) * (1.0f - HitWeight) * AttackWeight);
	}
	if (DeathWeight > 0.0f)
	{
		AddLayer(Death, FMath::Min(Inputs.SinceDeath, Death->GetPlayLength()), false, DeathWeight);
	}

	if (Layers.Num() == 0)
	{
		Output.ResetToRefPose();
		return true;
	}
	if (Layers.Num() == 1)
	{
		Sample(Layers[0].Sequence, Layers[0].Time, Layers[0].bLooping, Output);
	}
	else
	{
		TArray<FCompactPose, TInlineAllocator<5>> Poses;
		TArray<FBlendedCurve, TInlineAllocator<5>> Curves;
		TArray<UE::Anim::FStackAttributeContainer, TInlineAllocator<5>> Attributes;
		TArray<float, TInlineAllocator<5>> Weights;
		float Total = 0.0f;
		for (const FLayer& Layer : Layers)
		{
			Total += Layer.Weight;
		}
		for (const FLayer& Layer : Layers)
		{
			FPoseContext LayerPose(Output);
			Sample(Layer.Sequence, Layer.Time, Layer.bLooping, LayerPose);
			Poses.Add(MoveTemp(LayerPose.Pose));
			Curves.Add(MoveTemp(LayerPose.Curve));
			Attributes.Add(MoveTemp(LayerPose.CustomAttributes));
			Weights.Add(Layer.Weight / Total);
		}
		FAnimationPoseData OutData(Output);
		FAnimationRuntime::BlendPosesTogether(Poses, Curves, Attributes, Weights, OutData);
	}

	// The shamble: arms held out in front and the chest pitched forward, fading
	// out as the body falls.
	const float ReachWeight = Inputs.bZombie ? (1.0f - DeathWeight) * 0.9f : 0.0f;
	if (ReachWeight > 0.0f)
	{
		FCSPose<FCompactPose> ComponentPose;
		ComponentPose.InitPose(Output.Pose);
		const FVector Target = MadFall::CharacterAnim::ArmReachDirection(Inputs.Attack);
		ReachArm(Output.Pose, ComponentPose, TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l"), Target, ReachWeight);
		ReachArm(Output.Pose, ComponentPose, TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("hand_r"), Target, ReachWeight);
		LeanBone(Output.Pose, ComponentPose, TEXT("spine_03"), ZombieLeanDegrees * ReachWeight);
	}
	return true;
}

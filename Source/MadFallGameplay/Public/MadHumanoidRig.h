// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "MadHumanoidRig.generated.h"

class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/** The rig's pose for one frame: joint pitches in degrees, negative leaning or swinging forward. */
struct FMadHumanoidPose
{
	float LeftLeg = 0.0f;
	float RightLeg = 0.0f;
	float LeftArm = 0.0f;
	float RightArm = 0.0f;
	float TorsoLean = 0.0f;
	float HeadNod = 0.0f;
	/** Whole-body fall, 0 standing to -90 on its back. */
	float Fall = 0.0f;
	/** Vertical bob of the hips, cm. */
	float Bob = 0.0f;
};

namespace MadFall::Humanoid
{
	/**
	 * A shambling pose. Pure so it is testable: Phase is the walk cycle in
	 * radians, SpeedFactor 0 (standing) to 1 (full stride), Attack 0..1 through a
	 * swing, Death 0..1 through the fall.
	 */
	MADFALLGAMEPLAY_API FMadHumanoidPose ComputePose(float Phase, float SpeedFactor, float Attack, float Death);
}

/**
 * A humanoid made of boxes, animated in code.
 *
 * Stand-in for a skeletal mesh: no art exists and a tinted cylinder read as a
 * post, not a threat. Six box parts on joints (hips, shoulders, neck, and the
 * feet as the fall pivot), posed each frame from the owner's velocity plus
 * attack and hit pulses. Drawn with M_MadCharacter
 * (Scripts/make_character_material.py): pixel-art skin, a face, hair, and a
 * shirt and trousers, rotting and bloodied on a zombie and clean on a living
 * person. Boxes, not the cylinders of the first version, so a character is
 * drawn at the same pixel-art scale as the blocks around it. Falls back to the
 * tinted engine material if the asset is missing. Skips posing when not
 * recently rendered, so a horde behind you costs nothing.
 *
 * The component's origin is the feet; attach it at the bottom of the capsule.
 */
UCLASS(ClassGroup = MadFall)
class MADFALLGAMEPLAY_API UMadHumanoidRigComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UMadHumanoidRigComponent();

	virtual void OnRegister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Skin, shirt and trousers. Hair defaults to a dark brown. */
	void SetColours(const FLinearColor& Skin, const FLinearColor& Clothes, const FLinearColor& Trousers, const FLinearColor& Hair = FLinearColor(0.035f, 0.022f, 0.012f));

	/** A living face and clean clothes instead of a zombie's (the default). */
	void SetLiving(bool bInLiving);

	/** Varies blotches and stains between characters. */
	void SetSeed(int32 InSeed);

	/** A swing of both arms (an attack on the player or a block). */
	void PlayAttack();

	/** A brief red flash. */
	void PlayHit();

	/** Falls over and stays down. */
	void PlayDeath();

	bool IsDown() const { return DeathProgress >= 1.0f; }

private:
	/** Material parts: the values M_MadCharacter's Part parameter takes. */
	enum class EPart : uint8 { Skin = 0, Shirt = 1, Head = 2, Trousers = 3 };

	UStaticMeshComponent* MakePart(const TCHAR* Name, USceneComponent* Parent, const FVector& Location, const FVector& SizeCm, EPart Part);
	void Build();
	/** Pushes colours and style into every part's material; Flash 0..1 is the hit flash. */
	void ApplyTint(float Flash);

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Hips;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> LeftHip;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> RightHip;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Chest;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> LeftShoulder;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> RightShoulder;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Neck;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Parts;

	/** One per part (size and part differ), parallel to PartKinds. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> PartMaterials;
	TArray<EPart> PartKinds;

	FLinearColor SkinColour = FLinearColor(0.12f, 0.18f, 0.09f);
	FLinearColor ClothesColour = FLinearColor(0.05f, 0.05f, 0.06f);
	FLinearColor TrousersColour = FLinearColor(0.03f, 0.03f, 0.05f);
	FLinearColor HairColour = FLinearColor(0.035f, 0.022f, 0.012f);
	bool bLiving = false;
	/** Whether the parts use M_MadCharacter (style parameters) or the tinted fallback. */
	bool bPatterned = false;
	int32 Seed = 0;

	bool bBuilt = false;
	float Phase = 0.0f;
	float AttackProgress = 1.0f;
	float HitFlash = 0.0f;
	float DeathProgress = 0.0f;
	bool bDying = false;
};

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "MadViewModel.generated.h"

struct FMadItemDefinition;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/** What the survivor visibly holds. */
enum class EMadHeldShape : uint8
{
	Empty,
	Block,
	Pickaxe,
	Axe,
	Shovel,
	Hoe,
	Bow,
	Club,
	Food,
	Drink,
	Resource
};

namespace MadFall::ViewModel
{
	/** Which shape an item is drawn as, from its kind and tags. Null (nothing held) is Empty. */
	MADFALLGAMEPLAY_API EMadHeldShape ChooseShape(const FMadItemDefinition* Item);

	/**
	 * The held item's offset from its resting pose: Swing and Use run 0..1 through
	 * their motions (1 = finished), BobPhase is the walk cycle in radians and
	 * Moving 0..1 how much of a stride to show.
	 */
	MADFALLGAMEPLAY_API FTransform ComputeOffset(float Swing, float Use, float BobPhase, float Moving);
}

/**
 * The first-person held item: the tool, block or food in the survivor's hand.
 *
 * Built from engine basic shapes like the zombie rig - a handle and a head for
 * each tool, the block itself tinted with its surface colour, a can or a bottle
 * for food and drink, a fist when empty - attached to the camera. Rebuilt when
 * the selected hotbar item changes; animated with a swing when a tool is used,
 * a push when something is placed or eaten, and a bob while walking. Without it
 * a swing had no visible feedback at all.
 */
UCLASS(ClassGroup = MadFall)
class MADFALLGAMEPLAY_API UMadViewModelComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UMadViewModelComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** The item to show, or None. Cheap when unchanged. */
	void SetHeldItem(FName ItemId);

	void PlaySwing();
	void PlayUse();

	EMadHeldShape GetShape() const { return Shape; }

private:
	void Rebuild();
	UStaticMeshComponent* AddPart(const TCHAR* MeshPath, const FVector& Location, const FVector& SizeCm, const FLinearColor& Colour, const FRotator& Rotation = FRotator::ZeroRotator);

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Hand;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Parts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	FName HeldItem;
	bool bBuiltOnce = false;
	EMadHeldShape Shape = EMadHeldShape::Empty;
	float SwingProgress = 1.0f;
	float UseProgress = 1.0f;
	float BobPhase = 0.0f;
};

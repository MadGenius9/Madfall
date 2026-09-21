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
	Resource,
	/** A firearm: a ranged weapon tagged weapon.firearm. */
	Gun
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

	/**
	 * The hand-built model drawn for a shape (Scripts/build_prop_meshes.py), or
	 * null for the shapes still drawn from basic parts: a block (a textured cube
	 * is already the block), a resource and the empty hand.
	 */
	MADFALLGAMEPLAY_API const TCHAR* GetHeldModelPath(EMadHeldShape Shape);

	/**
	 * True for a held shape that needs a fist and forearm drawn under it.
	 *
	 * A pickaxe reads as held because its handle reaches the corner of the
	 * screen and the eye fills in an arm. A can, a bottle, a block or a lump of
	 * iron is fist-sized, so with nothing under it it hangs in the air beside
	 * the crosshair and reads as a bug. Every shape is one or the other, which
	 * is what the test checks: a new shape has to say which it is.
	 */
	MADFALLGAMEPLAY_API bool NeedsHand(EMadHeldShape Shape);

	/** True for a shape long enough to leave the frame on its own. */
	MADFALLGAMEPLAY_API bool ReachesTheEdge(EMadHeldShape Shape);
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
	/** A fist and a forearm, for held things too small to reach the screen edge. */
	void AddHand();

	UStaticMeshComponent* AddPart(const TCHAR* MeshPath, const FVector& Location, const FVector& SizeCm, const FLinearColor& Colour, const FRotator& Rotation = FRotator::ZeroRotator);

	/**
	 * Draws a hand-built model: each slot named after a surface wears that
	 * surface's texture (dry), and the slot named "head" is stone for a stone tool
	 * and steel for any other. False if the model did not load.
	 */
	bool AddModel(const TCHAR* ModelPath, bool bStoneHead);

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

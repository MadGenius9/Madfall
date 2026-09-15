// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadStressOverlay.generated.h"

class AActor;
class UInstancedStaticMeshComponent;

/**
 * Structural stress shading: while the survivor holds a block to place and aims
 * at a structure, the blocks around the aim point are washed in colour by how
 * close each is to its span or weight limit - blue sound, green, yellow, red at
 * the limit, pulsing red failing.
 *
 * WHY: "load 72%" on the one targeted block told a builder which support was
 * about to go only if they aimed at it. The shading shows the whole
 * neighbourhood at once, which is how a player learns where a roof needs a
 * pillar before it comes down.
 *
 * Data: the stress probe the HUD readout already runs
 * (UMadStructuralSubsystem::GetStressField), so there is no extra solve. One
 * instanced component of slightly oversized translucent boxes, the stress and
 * failing flag in per-instance custom data for M_MadStressOverlay; rebuilt only
 * when the probe's samples change (about once a second). Absent without a
 * renderer. mad.si.Overlay 0 turns it off.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadStressOverlaySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	int32 NumShaded() const;

private:
	UInstancedStaticMeshComponent* GetOrCreateComponent();

	UPROPERTY(Transient)
	TObjectPtr<AActor> OverlayActor;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Boxes;

	int32 AppliedVersion = -1;
	bool bVisible = false;
};

namespace MadFall::StressOverlay
{
	/**
	 * The colour ramp the overlay material draws, for tests and the HUD legend:
	 * 0 blue, 0.5 green, 0.8 yellow, 1 and above red. Linear.
	 */
	MADFALLGAMEPLAY_API FLinearColor RampColour(float Stress);
}

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/StreamableManager.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadCharacterAssetSubsystem.generated.h"

/**
 * Loads every skeletal character asset - the mannequins and each animal
 * definition's model - asynchronously when a rendering world starts.
 *
 * WHY: a rig used to load its mesh and clips on the frame its first character
 * appeared, a synchronous load of a skeletal mesh, a skeleton, a physics asset
 * and a handful of sequences in the middle of play. Rigs now wait (briefly) for
 * this to settle and then find everything already in memory. Only packages that
 * exist are requested, so a checkout without the mannequins (they are copied
 * from the engine, not committed) loads the animals and draws box humans
 * without a warning per zombie.
 *
 * Not created without a renderer: nothing skeletal is drawn there.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadCharacterAssetSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** True once the load has finished (whatever it found). */
	bool IsSettled() const { return bSettled; }

	int32 NumRequested() const { return Requested; }

private:
	TSharedPtr<FStreamableHandle> Handle;
	bool bSettled = false;
	int32 Requested = 0;
};

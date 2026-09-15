// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadGameplaySave.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadGameplaySaveSubsystem.generated.h"

class AMadPlayerCharacter;

/**
 * Saves and restores gameplay state (gameplay.json beside the region files).
 *
 * WHEN
 *   - `mad.save`: voxels and gameplay together
 *   - autosave every mad.save.AutosaveMinutes
 *   - the player leaving play (quit, map change)
 *
 * RESTORE ORDER
 *   The file is read at world start. The clock, containers and sleeper state
 *   are applied on the first tick, when their subsystems exist. The player's
 *   part is handed over when the survivor begins play - it replaces the
 *   starting kit - and their position is applied once the world under the saved
 *   location has streamed in, the same wait a fresh spawn gets.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadGameplaySaveSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Gathers everything and writes the file. Pass the player when calling from their EndPlay. */
	bool SaveNow(AMadPlayerCharacter* Player = nullptr);

	/** Hands the loaded player data to the survivor once. False when there is none. */
	bool ConsumePlayerSave(FMadPlayerSaveData& OutPlayer);

	FString GetSavePath() const;

	/** True once the loaded world state (clock, containers, script store...) has been handed to its subsystems. */
	bool HasAppliedWorldState() const { return bWorldStateApplied; }

private:
	void ApplyWorldState();

	TOptional<FMadGameplaySave> Loaded;
	bool bWorldStateApplied = false;
	bool bPlayerConsumed = false;
	float AutosaveTimer = 0.0f;
};

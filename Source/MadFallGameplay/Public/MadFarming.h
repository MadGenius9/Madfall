// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadGameplaySave.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadFarming.generated.h"

class FMadBlockRegistry;
class UMadVoxelWorldSubsystem;
class UMadWorldClockSubsystem;
struct FMadBlockDefinitionData;
struct FMadVoxel;

/**
 * Which voxels hold a block with a `grow` stage, and since when.
 *
 * WHY A LIST RATHER THAN A SCAN
 *   Growth could be found by scanning loaded chunks for growing block ids (the
 *   Minecraft random tick). That costs a 32,768-voxel pass per chunk whether or
 *   not anything grows there, and a crop in a chunk that is unloaded makes no
 *   progress at all - a farm left behind would still be seedlings on return.
 *   Planting is rare and deliberate, so remembering each plant is cheap, and a
 *   plant stores the hour its stage started rather than a countdown: a chunk
 *   loaded after three days catches up stage by stage, each stage timed from
 *   the end of the previous one, exactly as if it had never unloaded.
 *
 *   The cost is that a growing block the list does not know about - placed by a
 *   prefab, or with gameplay.json lost - never grows. Prefabs do not ship crops.
 *
 * Plain C++ with no world, so the rules are testable without one; the subsystem
 * below feeds it voxel changes and the clock.
 */
class MADFALLGAMEPLAY_API FMadPlantTracker
{
public:
	struct FPlant
	{
		FName Block;
		FName Into;
		double StageStartHour = 0.0;

		/** When the stage ends. Infinite for a block the registry no longer knows (its mod was removed). */
		double DueHour = 0.0;
	};

	struct FDue
	{
		FIntVector Position;
		FName From;
		FName Into;

		/** The next stage starts when this one was due, not when it was noticed. */
		double NextStageStartHour = 0.0;
	};

	/**
	 * Records the block now at a voxel. A block that grows starts its stage at
	 * StageStartHour; anything else forgets the voxel.
	 */
	void NoteBlock(const FIntVector& Position, const FMadBlockDefinitionData* Block, double StageStartHour);

	void Forget(const FIntVector& Position) { Plants.Remove(Position); }

	const FPlant* Find(const FIntVector& Position) const { return Plants.Find(Position); }

	int32 Num() const { return Plants.Num(); }

	/** Every plant whose stage is over at NowHour, earliest first. */
	void CollectDue(double NowHour, TArray<FDue>& Out) const;

	/** Hours until the next plant is due; negative if one already is, TNumericLimits max if none. */
	double HoursUntilNext(double NowHour) const;

	/** Moves every plant's clock forward by Hours, as if that much time had passed. */
	void Advance(double Hours);

	void Export(TArray<FMadPlantSaveData>& Out) const;

	/** Replaces the tracked plants. Stage lengths come from Blocks, so a mod's changed growth time applies to old saves. */
	void Import(const TArray<FMadPlantSaveData>& In, const FMadBlockRegistry& Blocks);

	void Reset() { Plants.Reset(); }

private:
	TMap<FIntVector, FPlant> Plants;
};

/**
 * Crops: blocks with `grow { into, hours }` turn into their next stage after
 * that many in-game hours.
 *
 * Game worlds only. Growth is checked once a second and a stage change is an
 * ordinary SetVoxel, so the mesher, model instances and structural solver see a
 * grown crop the way they see a placed block. Stages in unloaded chunks wait,
 * keeping their start hour, and catch up when the chunk returns.
 *
 *   `mad.farm.status`          plants tracked, how many are due
 *   `mad.farm.advance <hours>` fast-forwards every plant
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadFarmingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	const FMadPlantTracker& GetTracker() const { return Tracker; }

	/** How far through its current stage the plant at Position is, 0-1. False if nothing is growing there. */
	bool GetStageProgress(const FIntVector& Position, float& OutProgress) const;

	/** Fast-forwards every plant and checks growth on the next tick. */
	void Advance(double Hours);

	void ExportState(TArray<FMadPlantSaveData>& Out) const { Tracker.Export(Out); }
	void ImportState(const TArray<FMadPlantSaveData>& In);

	FString DescribeStatus() const;

	/** Growth checks run this often, seconds. A stage lasts hours; a second is plenty. */
	static constexpr float CheckInterval = 1.0f;

	/** Stage changes per check. Each is a SetVoxel and a remesh; a harvest-day field spreads over a few seconds. */
	static constexpr int32 MaxGrowthsPerCheck = 16;

private:
	void HandleVoxelChanged(const FIntVector& Position, const FMadVoxel& Before, const FMadVoxel& After);

	double GetNowHour() const;

	UPROPERTY(Transient)
	TObjectPtr<UMadVoxelWorldSubsystem> VoxelWorld;

	UPROPERTY(Transient)
	TObjectPtr<UMadWorldClockSubsystem> Clock;

	FMadPlantTracker Tracker;
	FDelegateHandle VoxelChangedHandle;
	float CheckTimer = 0.0f;

	/** Set while this subsystem writes a grown stage, so the change notification keeps the carried-over start hour. */
	TOptional<TPair<FIntVector, double>> GrowingWrite;
};

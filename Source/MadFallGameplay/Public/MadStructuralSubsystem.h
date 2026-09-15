// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBlockDamage.h"
#include "MadStructuralSolver.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadStructuralSubsystem.generated.h"

class UMadVoxelWorldSubsystem;

/** Fired once per applied collapse batch, after the voxels have been removed. */
DECLARE_MULTICAST_DELEGATE_OneParam(FMadOnStructureCollapsed, const TArray<FMadStructuralFailureRecord>& /*Failed*/);

/**
 * Fired when an edit leaves a structure standing with a member near its limit
 * (mad.si.CreakStress): the warning before a collapse. At most once per
 * mad.si.CreakCooldown seconds.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FMadOnStructureStrained, const FMadStressSample& /*Member*/, const FMadVoxel& /*Voxel*/);

/** What a stress query could tell. */
enum class EMadStressQuery : uint8
{
	NotMember,
	Pending,
	Ready,
	TooLarge
};

/** Running totals for mad.si.status and the CI acceptance check. */
struct MADFALLGAMEPLAY_API FMadStructuralStats
{
	int64 JobsCompleted = 0;
	int64 JobsRestarted = 0;
	int64 JobsTruncated = 0;
	int64 BlocksCollapsed = 0;
	int64 BlocksUnsupported = 0;
	int64 BlocksOverloaded = 0;
	int64 Creaks = 0;
	int64 ChunksSeededOnLoad = 0;
	int64 SeedsFromLoads = 0;
	int32 LargestComponent = 0;
	double WorstFrameMs = 0.0;
	double TotalSolveMs = 0.0;
};

/**
 * Runs the structural solver against the live voxel world.
 *
 * FLOW
 *   UMadVoxelWorldSubsystem::OnVoxelChanged -> filter -> pending seeds
 *   Tick: one job at a time, stepped until the frame budget is spent
 *   Done: failed members are re-verified against the world and removed
 *         through SetVoxel, which fires OnVoxelChanged again - that is how a
 *         collapse cascades, one pass per settled job.
 *
 * ONE JOB, TIME-SLICED, GAME THREAD
 *   The obvious alternative is a worker task over a snapshot. It was rejected
 *   for now because the result must be applied against the world as it is when
 *   the job finishes, and a snapshot of an arbitrarily large structure is
 *   itself an O(n) game-thread copy. Gathering incrementally on the game
 *   thread and then solving over the job's own copy is the same cost split
 *   across frames, with no snapshot. The solver never touches the world after
 *   gathering, so moving Solve/Load onto a worker later is a local change.
 *
 *   Changes arriving while a job runs are checked with FMadStructuralJob::
 *   Touches: unrelated edits queue for the next job; an edit to the structure
 *   being solved marks the job stale and it restarts with the merged seeds once
 *   it reaches the end of its current phase.
 *
 * WHAT COUNTS AS A STRUCTURAL CHANGE
 *   Only changes of role (empty/anchor/member), of block id on a member, or of
 *   damage stage on a member. A zombie chewing on a wall changes the damage
 *   byte on every hit; re-solving the building on each of those would be the
 *   single biggest waste of frame time in a horde night.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadStructuralSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	//~ End USubsystem

	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject

	/** Queues every member voxel in a box for a check. For POIs and tools. */
	int32 CheckBox(const FIntVector& Min, const FIntVector& Max);

	/** Runs every queued job to completion, ignoring the frame budget. Tests and console only. */
	void FlushNow(int32 MaxJobs = 64);

	/**
	 * Solves the structure containing Position synchronously without applying
	 * anything, and reports on that voxel. Returns false if it is not a member.
	 */
	bool Inspect(const FIntVector& Position, FMadStructuralNodeReport& OutReport, int32& OutMembers);

	/**
	 * Deals damage to a voxel and writes the result. Returns the result so
	 * callers can play the right sound and spawn the right particles.
	 */
	FMadBlockDamageResult ApplyBlockDamage(const FIntVector& Position, float Amount, FName DamageType);

	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
	bool IsEnabled() const { return bEnabled; }

	bool IsIdle() const { return !ActiveJob.IsValid() && PendingSeeds.Num() == 0; }

	const FMadStructuralStats& GetStats() const { return Stats; }
	FString DescribeStatus() const;

	/**
	 * Stress readout for one voxel, for the HUD. The solve runs in the background
	 * on its own small budget (mad.si.ProbeBudgetMs), refreshes about once a
	 * second, and gives up on structures above mad.si.ProbeMaxMembers, so aiming
	 * at a skyscraper can never hitch a frame. Call every frame with the target.
	 */
	EMadStressQuery QueryStress(const FIntVector& Position, FMadStructuralNodeReport& OutReport);

	/**
	 * The stress of the members around the last queried block, from the same
	 * probe solve (nearest first, mad.si.OverlayRadius, at most
	 * mad.si.OverlayMaxBlocks). Version changes when the samples do; LastQueryTime
	 * is when QueryStress was last called, so a view can hide once nothing asks.
	 * Kept across a change of target until the new solve lands, so moving the aim
	 * along a wall does not blink the shading off.
	 */
	const TArray<FMadStressSample>& GetStressField(int32& OutVersion, double& OutLastQueryTime) const
	{
		OutVersion = StressFieldVersion;
		OutLastQueryTime = LastQueryTime;
		return StressField;
	}

	FMadOnStructureCollapsed& OnStructureCollapsed() { return CollapsedDelegate; }
	FMadOnStructureStrained& OnStructureStrained() { return StrainedDelegate; }

	const FMadStructuralMaterials& GetMaterials();

private:
	void TickProbe();

	void HandleVoxelChanged(const FIntVector& Position, const FMadVoxel& Before, const FMadVoxel& After);

	/** Queues a check of every member in a chunk that just arrived: a structure can load already unsound. */
	void HandleChunkLoaded(const FMadChunkCoord& Coord);

	/** Adds one seed if it is not already pending. Returns true if added. */
	bool AddSeed(const FIntVector& Position);

	/** Seeds the next job from everything pending. */
	void StartJob();

	/** Applies a finished job. */
	void ApplyJob(const FMadStructuralJob& Job);

	/** Adds a position and its six neighbours to the pending set. */
	void AddSeedWithNeighbours(const FIntVector& Position);

	UPROPERTY(Transient)
	TObjectPtr<UMadVoxelWorldSubsystem> VoxelWorld;

	FMadStructuralMaterials Materials;
	bool bMaterialsBuilt = false;

	FMadStructuralSettings Settings;

	TUniquePtr<FMadStructuralJob> ActiveJob;
	bool bActiveJobStale = false;
	int32 ConsecutiveRestarts = 0;

	/** Insertion-ordered so job seeding - and therefore collapse order - is deterministic. */
	TArray<FIntVector> PendingOrder;
	TSet<FIntVector> PendingSeeds;

	FDelegateHandle VoxelChangedHandle;
	FDelegateHandle ChunkLoadedHandle;
	FMadOnStructureCollapsed CollapsedDelegate;
	FMadOnStructureStrained StrainedDelegate;

	/**
	 * Whether the pending seeds / the running job include an edit. Only those
	 * creak: a job seeded purely by chunks streaming in would groan at every
	 * stressed POI the survivor walks past, which is noise, not a warning.
	 */
	bool bPendingFromEdit = false;
	bool bActiveJobFromEdit = false;
	double LastCreakTime = -1.0e9;

	FMadStructuralStats Stats;

	TUniquePtr<FMadStructuralJob> ProbeJob;
	FIntVector ProbePosition = FIntVector(MAX_int32);
	FMadStructuralNodeReport ProbeReport;
	EMadStressQuery ProbeResult = EMadStressQuery::Pending;
	bool bProbeHasResult = false;
	double ProbeResultTime = 0.0;
	TArray<FMadStressSample> StressField;
	int32 StressFieldVersion = 0;
	double LastQueryTime = 0.0;
	bool bEnabled = true;

	/** Set while ApplyJob writes, so a collapse still seeds its own cascade but not the active job's stale flag. */
	bool bApplying = false;
};

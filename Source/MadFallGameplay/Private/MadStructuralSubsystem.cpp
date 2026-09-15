// Copyright MadFall. All Rights Reserved.

#include "MadStructuralSubsystem.h"

#include "MadFrameBudget.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MadBlockRegistry.h"
#include "MadFallGameplay.h"
#include "MadFallStats.h"
#include "MadVoxelWorldSubsystem.h"
#include "Misc/DefaultValueHelper.h"

DECLARE_CYCLE_STAT(TEXT("Structural Tick"), STAT_MadStructuralTick, STATGROUP_MadFallStructural);
DECLARE_CYCLE_STAT(TEXT("Structural Apply"), STAT_MadStructuralApply, STATGROUP_MadFallStructural);

namespace
{
	/**
	 * Frame budget. One millisecond leaves half of the 2 ms rule for the mesh
	 * applies a collapse inevitably triggers in the same frame.
	 */
	TAutoConsoleVariable<float> CVarBudgetMs(
		TEXT("mad.si.BudgetMs"),
		1.0f,
		TEXT("Game-thread milliseconds the structural solver may use per frame."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarCreakStress(
		TEXT("mad.si.CreakStress"),
		0.85f,
		TEXT("A structure an edit leaves standing creaks when its most stressed member is at least this close to failing (0..1). Above 1 disables creaking."));

	TAutoConsoleVariable<float> CVarCreakCooldown(
		TEXT("mad.si.CreakCooldown"),
		1.5f,
		TEXT("Least seconds between structural creaks, so building onto a strained frame is a warning, not a drone."));

	TAutoConsoleVariable<int32> CVarCheckOnLoad(
		TEXT("mad.si.CheckOnLoad"),
		1,
		TEXT("1: check the structures in every chunk as it loads, so a building that arrives unsound (a POI whose ground a road cut) falls. 0: only edits are checked."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarProbeBudgetMs(
		TEXT("mad.si.ProbeBudgetMs"),
		0.25f,
		TEXT("Game-thread milliseconds per frame for the HUD's stress readout of the targeted block."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarOverlayRadius(
		TEXT("mad.si.OverlayRadius"), 8,
		TEXT("Voxels around the aimed block whose stress is shaded while building."));

	TAutoConsoleVariable<int32> CVarOverlayMaxBlocks(
		TEXT("mad.si.OverlayMaxBlocks"), 768,
		TEXT("Most blocks stress shading draws at once, nearest first."));

	TAutoConsoleVariable<int32> CVarProbeMaxMembers(
		TEXT("mad.si.ProbeMaxMembers"),
		16384,
		TEXT("The stress readout gives up on structures larger than this."),
		ECVF_Default);

	/** Work units per Step between clock checks. ~256 node visits is well under 0.1 ms. */
	constexpr int32 WorkUnitsPerStep = 256;

	const FIntVector NeighbourOffsets[6] =
	{
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0),
		FIntVector(0, 0, 1), FIntVector(0, 0, -1)
	};

	/**
	 * The live world as the solver sees it.
	 *
	 * Caches the last chunk because the gather phase reads neighbours in runs,
	 * and a TMap find plus a shared-pointer copy per voxel would be most of the
	 * cost of a solve. Lives for a single tick, so it never outlives an unload.
	 */
	class FLiveStructuralWorld final : public IMadStructuralWorld
	{
	public:
		explicit FLiveStructuralWorld(const UMadVoxelWorldSubsystem& InWorld) : World(InWorld) {}

		virtual bool IsLoaded(const FIntVector& Position) const override
		{
			// Above the world is open sky, so "loaded" and empty. Below it is the
			// planet, which is what an unloaded chunk already means: an anchor.
			if (Position.Z > MadFall::WorldMaxZ) { return true; }
			if (Position.Z < MadFall::WorldMinZ) { return false; }
			return FindChunkCached(Position) != nullptr;
		}

		virtual FMadVoxel GetVoxel(const FIntVector& Position) const override
		{
			const FMadChunk* Chunk = FindChunkCached(Position);
			if (Chunk == nullptr)
			{
				return FMadVoxel();
			}

			int32 LX, LY, LZ;
			MadFall::WorldToLocal(Position.X, Position.Y, Position.Z, LX, LY, LZ);

			FRWScopeLock Lock(Chunk->Lock, SLT_ReadOnly);
			return Chunk->Storage.GetVoxel(LX, LY, LZ);
		}

	private:
		const FMadChunk* FindChunkCached(const FIntVector& Position) const
		{
			if (!MadFall::IsValidWorldZ(Position.Z))
			{
				return nullptr;
			}

			const FMadChunkCoord Coord = MadFall::WorldToChunk(Position.X, Position.Y, Position.Z);
			if (!bHaveCached || !(Coord == CachedCoord))
			{
				CachedChunk = World.FindChunk(Coord);
				CachedCoord = Coord;
				bHaveCached = true;
			}
			return CachedChunk.Get();
		}

		const UMadVoxelWorldSubsystem& World;
		mutable FMadChunkCoord CachedCoord;
		mutable FMadChunkPtr CachedChunk;
		mutable bool bHaveCached = false;
	};
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void UMadStructuralSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	VoxelWorld = Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
	if (VoxelWorld != nullptr)
	{
		VoxelChangedHandle = VoxelWorld->OnVoxelChanged().AddUObject(this, &UMadStructuralSubsystem::HandleVoxelChanged);
		ChunkLoadedHandle = VoxelWorld->OnChunkLoaded().AddUObject(this, &UMadStructuralSubsystem::HandleChunkLoaded);
	}
}

void UMadStructuralSubsystem::Deinitialize()
{
	if (VoxelWorld != nullptr)
	{
		VoxelWorld->OnVoxelChanged().Remove(VoxelChangedHandle);
		VoxelWorld->OnChunkLoaded().Remove(ChunkLoadedHandle);
	}
	VoxelWorld = nullptr;
	ActiveJob.Reset();
	PendingOrder.Reset();
	PendingSeeds.Reset();

	Super::Deinitialize();
}

bool UMadStructuralSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadStructuralSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadStructuralSubsystem, STATGROUP_Tickables);
}

const FMadStructuralMaterials& UMadStructuralSubsystem::GetMaterials()
{
	// Lazy, because the registry is itself built lazily on first use and a
	// world subsystem initialises before anything has asked for it.
	if (!bMaterialsBuilt)
	{
		Materials.Build(UMadVoxelWorldSubsystem::GetBlockRegistry());
		bMaterialsBuilt = true;
	}
	return Materials;
}

// ===========================================================================
// Change intake
// ===========================================================================

void UMadStructuralSubsystem::AddSeedWithNeighbours(const FIntVector& Position)
{
	auto Add = [this](const FIntVector& P)
	{
		bool bAlreadyPending = false;
		PendingSeeds.Add(P, &bAlreadyPending);
		if (!bAlreadyPending)
		{
			PendingOrder.Add(P);
		}
	};

	Add(Position);
	for (const FIntVector& Offset : NeighbourOffsets)
	{
		Add(Position + Offset);
	}
}

void UMadStructuralSubsystem::HandleVoxelChanged(const FIntVector& Position, const FMadVoxel& Before, const FMadVoxel& After)
{
	if (!bEnabled)
	{
		return;
	}

	const FMadStructuralMaterials& Mats = GetMaterials();
	const EMadStructuralRole RoleBefore = FMadStructuralJob::Classify(Before, Mats);
	const EMadStructuralRole RoleAfter = FMadStructuralJob::Classify(After, Mats);

	bool bRelevant = RoleBefore != RoleAfter;
	if (!bRelevant && RoleAfter == EMadStructuralRole::Member)
	{
		bRelevant = Before.BlockTypeID != After.BlockTypeID
			|| Mats.Get(Before.BlockTypeID).GetStageIndex(Before.Damage) != Mats.Get(After.BlockTypeID).GetStageIndex(After.Damage);
	}

	if (!bRelevant)
	{
		return;
	}

	AddSeedWithNeighbours(Position);
	bPendingFromEdit = true;

	if (ActiveJob.IsValid() && !bApplying && ActiveJob->Touches(Position))
	{
		bActiveJobStale = true;
	}
}

int32 UMadStructuralSubsystem::CheckBox(const FIntVector& Min, const FIntVector& Max)
{
	if (VoxelWorld == nullptr)
	{
		return 0;
	}

	const FMadStructuralMaterials& Mats = GetMaterials();
	int32 Queued = 0;

	for (int32 Z = FMath::Max(Min.Z, MadFall::WorldMinZ); Z <= FMath::Min(Max.Z, MadFall::WorldMaxZ); ++Z)
	{
		for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
		{
			for (int32 X = Min.X; X <= Max.X; ++X)
			{
				const FMadVoxel Voxel = VoxelWorld->GetVoxel(X, Y, Z);
				if (FMadStructuralJob::Classify(Voxel, Mats) == EMadStructuralRole::Member && AddSeed(FIntVector(X, Y, Z)))
				{
					++Queued;
				}
			}
		}
	}

	return Queued;
}

bool UMadStructuralSubsystem::AddSeed(const FIntVector& Position)
{
	bool bAlreadyPending = false;
	PendingSeeds.Add(Position, &bAlreadyPending);
	if (bAlreadyPending)
	{
		return false;
	}
	PendingOrder.Add(Position);
	return true;
}

void UMadStructuralSubsystem::HandleChunkLoaded(const FMadChunkCoord& Coord)
{
	if (!bEnabled || VoxelWorld == nullptr || CVarCheckOnLoad.GetValueOnGameThread() == 0)
	{
		return;
	}
	const FMadChunkPtr Chunk = VoxelWorld->FindChunk(Coord);
	if (!Chunk.IsValid())
	{
		return;
	}

	TArray<FIntVector> Members;
	{
		FReadScopeLock Lock(Chunk->Lock);
		MadFall::Structural::FindMembersInChunk(Chunk->Storage, Coord, GetMaterials(), Members);
	}
	if (Members.Num() == 0)
	{
		return;
	}

	// Every member rather than one per structure: finding structures would be a
	// flood fill here, on the load path, duplicating the job's own gather. The
	// job de-duplicates seeds it has already gathered, so extras cost a set probe.
	for (const FIntVector& Member : Members)
	{
		Stats.SeedsFromLoads += AddSeed(Member) ? 1 : 0;
	}
	++Stats.ChunksSeededOnLoad;
}

// ===========================================================================
// Execution
// ===========================================================================

void UMadStructuralSubsystem::StartJob()
{
	TArray<FIntVector> Seeds = MoveTemp(PendingOrder);
	PendingOrder.Reset();
	PendingSeeds.Reset();

	ActiveJob = MakeUnique<FMadStructuralJob>(MoveTemp(Seeds), Settings);
	bActiveJobStale = false;
	bActiveJobFromEdit = bPendingFromEdit;
	bPendingFromEdit = false;
}

void UMadStructuralSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Structural);

	TickProbe();

	if (!bEnabled || VoxelWorld == nullptr || IsIdle())
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_MadStructuralTick);

	const double Start = FPlatformTime::Seconds();
	const double Budget = MadFall::FrameBudget::GetRemainingMs(
		FMath::Max(0.05, static_cast<double>(CVarBudgetMs.GetValueOnGameThread())), 0.1) / 1000.0;

	FLiveStructuralWorld World(*VoxelWorld);
	const FMadStructuralMaterials& Mats = GetMaterials();

	while (FPlatformTime::Seconds() - Start < Budget)
	{
		if (!ActiveJob.IsValid())
		{
			if (PendingOrder.Num() == 0)
			{
				break;
			}
			StartJob();
		}

		if (!ActiveJob->Step(World, Mats, WorkUnitsPerStep))
		{
			continue;
		}

		if (bActiveJobStale)
		{
			// Re-queue this job's seeds ahead of whatever arrived meanwhile.
			TArray<FIntVector> Merged = ActiveJob->GetSeeds();
			for (const FIntVector& P : PendingOrder)
			{
				Merged.Add(P);
			}
			PendingOrder.Reset();
			PendingSeeds.Reset();
			for (const FIntVector& P : Merged)
			{
				bool bAlreadyPending = false;
				PendingSeeds.Add(P, &bAlreadyPending);
				if (!bAlreadyPending)
				{
					PendingOrder.Add(P);
				}
			}

			bPendingFromEdit |= bActiveJobFromEdit;
			++Stats.JobsRestarted;
			if (++ConsecutiveRestarts == 8)
			{
				UE_LOG(LogMadFallStructural, Warning,
					TEXT("Structural job restarted 8 times in a row - the structure is changing faster than it can be solved."));
			}
			ActiveJob.Reset();
			continue;
		}

		ConsecutiveRestarts = 0;
		ApplyJob(*ActiveJob);
		ActiveJob.Reset();
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	Stats.TotalSolveMs += ElapsedMs;
	Stats.WorstFrameMs = FMath::Max(Stats.WorstFrameMs, ElapsedMs);
}

void UMadStructuralSubsystem::FlushNow(int32 MaxJobs)
{
	if (VoxelWorld == nullptr)
	{
		return;
	}

	FLiveStructuralWorld World(*VoxelWorld);
	const FMadStructuralMaterials& Mats = GetMaterials();

	for (int32 Jobs = 0; Jobs < MaxJobs && !IsIdle(); ++Jobs)
	{
		if (!ActiveJob.IsValid())
		{
			StartJob();
		}

		// Nothing else can write to the world while this runs, so a job that
		// was stale before FlushNow is the only one that needs discarding.
		if (bActiveJobStale)
		{
			TArray<FIntVector> Seeds = ActiveJob->GetSeeds();
			ActiveJob.Reset();
			bPendingFromEdit |= bActiveJobFromEdit;
			for (const FIntVector& P : Seeds)
			{
				bool bAlreadyPending = false;
				PendingSeeds.Add(P, &bAlreadyPending);
				if (!bAlreadyPending)
				{
					PendingOrder.Add(P);
				}
			}
			StartJob();
		}

		ActiveJob->RunToCompletion(World, Mats);
		ApplyJob(*ActiveJob);
		ActiveJob.Reset();
	}
}

void UMadStructuralSubsystem::ApplyJob(const FMadStructuralJob& Job)
{
	SCOPE_CYCLE_COUNTER(STAT_MadStructuralApply);

	++Stats.JobsCompleted;
	Stats.LargestComponent = FMath::Max(Stats.LargestComponent, Job.NumMembers());

	if (Job.WasTruncated())
	{
		++Stats.JobsTruncated;
		UE_LOG(LogMadFallStructural, Warning,
			TEXT("Structure exceeds %d members; left standing without a solve."), Settings.MaxComponentSize);
		return;
	}

	const TArray<FMadStructuralFailureRecord>& Failures = Job.GetFailures();
	if (Failures.Num() == 0)
	{
		// Standing, but maybe only just. A collapse makes its own noise, so the
		// warning is for structures that hold.
		FMadStressSample Strained;
		const double Now = FPlatformTime::Seconds();
		if (bActiveJobFromEdit && Job.GetMostStressed(Strained)
			&& Strained.Stress >= CVarCreakStress.GetValueOnGameThread()
			&& Now - LastCreakTime >= CVarCreakCooldown.GetValueOnGameThread())
		{
			LastCreakTime = Now;
			++Stats.Creaks;
			const FMadVoxel Voxel = VoxelWorld->GetVoxel(Strained.Position.X, Strained.Position.Y, Strained.Position.Z);
			UE_LOG(LogMadFallStructural, Display, TEXT("Creak: %s at %s is at %.0f%% of its limit."),
				*UMadVoxelWorldSubsystem::GetBlockRegistry().GetStringId(Voxel.BlockTypeID).ToString(),
				*Strained.Position.ToString(), Strained.Stress * 100.0f);
			StrainedDelegate.Broadcast(Strained, Voxel);
		}
		return;
	}

	TArray<FMadStructuralFailureRecord> Applied;
	Applied.Reserve(Failures.Num());

	TGuardValue<bool> ApplyingGuard(bApplying, true);

	FMadVoxel Air;
	Air.BlockTypeID = MadFall::BlockTypeAir;
	Air.Density = 0;
	Air.Damage = 0;
	Air.Rotation = 0;
	Air.Flags = 0;

	for (const FMadStructuralFailureRecord& Failure : Failures)
	{
		const FIntVector& P = Failure.Position;

		// The job read this voxel frames ago. Only remove it if it is still the
		// block that was judged - the stale check makes a mismatch unlikely,
		// but a mismatch must never delete something the solver never saw.
		const FMadVoxel Current = VoxelWorld->GetVoxel(P.X, P.Y, P.Z);
		if (Current.BlockTypeID != Failure.Voxel.BlockTypeID)
		{
			continue;
		}

		if (VoxelWorld->SetVoxel(P.X, P.Y, P.Z, Air))
		{
			Applied.Add(Failure);
			if (Failure.Reason == EMadStructuralFailure::Overloaded)
			{
				++Stats.BlocksOverloaded;
			}
			else
			{
				++Stats.BlocksUnsupported;
			}
		}
	}

	Stats.BlocksCollapsed += Applied.Num();

	if (Applied.Num() > 0)
	{
		UE_LOG(LogMadFallStructural, Log, TEXT("Collapse: %d block(s) failed in a structure of %d (first at %s, %s, %s)."),
			Applied.Num(), Job.NumMembers(), *Applied[0].Position.ToString(),
			*UMadVoxelWorldSubsystem::GetBlockRegistry().GetStringId(Applied[0].Voxel.BlockTypeID).ToString(),
			Applied[0].Reason == EMadStructuralFailure::Overloaded ? TEXT("overloaded") : TEXT("unsupported"));
		UE_LOG(LogMadFallStructural, Verbose, TEXT("  neighbours of %s: %s"), *Applied[0].Position.ToString(), *Job.DescribeNeighbours(Applied[0].Position));
		CollapsedDelegate.Broadcast(Applied);
	}
}

bool UMadStructuralSubsystem::Inspect(const FIntVector& Position, FMadStructuralNodeReport& OutReport, int32& OutMembers)
{
	OutMembers = 0;
	if (VoxelWorld == nullptr)
	{
		return false;
	}

	FLiveStructuralWorld World(*VoxelWorld);
	FMadStructuralJob Job({ Position }, Settings);
	Job.RunToCompletion(World, GetMaterials());

	OutMembers = Job.NumMembers();
	return Job.GetNodeReport(Position, OutReport);
}

FMadBlockDamageResult UMadStructuralSubsystem::ApplyBlockDamage(const FIntVector& Position, float Amount, FName DamageType)
{
	FMadBlockDamageResult Result;
	if (VoxelWorld == nullptr)
	{
		return Result;
	}

	const FMadVoxel Current = VoxelWorld->GetVoxel(Position.X, Position.Y, Position.Z);
	Result = MadFall::BlockDamage::Compute(Current, Amount, DamageType, UMadVoxelWorldSubsystem::GetBlockRegistry());

	const FMadVoxel& New = Result.NewVoxel;
	const bool bChanged = New.BlockTypeID != Current.BlockTypeID || New.Damage != Current.Damage
		|| New.Density != Current.Density || New.Flags != Current.Flags;

	if (bChanged)
	{
		// SetVoxel is the single write funnel: meshing, the structural filter
		// above and saving all follow from it.
		VoxelWorld->SetVoxel(Position.X, Position.Y, Position.Z, New);
	}

	return Result;
}

FString UMadStructuralSubsystem::DescribeStatus() const
{
	return FString::Printf(
		TEXT("Structural: %s, %s, %d pending seed(s)\n")
		TEXT("  jobs: %lld completed, %lld restarted, %lld truncated; largest structure %d\n")
		TEXT("  collapsed: %lld (%lld unsupported, %lld overloaded); %lld creak(s); %lld chunk(s) checked on load, %lld seed(s)\n")
		TEXT("  time: %.2f ms total, worst frame %.3f ms (budget %.2f ms)"),
		bEnabled ? TEXT("enabled") : TEXT("DISABLED"),
		ActiveJob.IsValid() ? TEXT("job running") : TEXT("idle"),
		PendingOrder.Num(),
		Stats.JobsCompleted, Stats.JobsRestarted, Stats.JobsTruncated, Stats.LargestComponent,
		Stats.BlocksCollapsed, Stats.BlocksUnsupported, Stats.BlocksOverloaded, Stats.Creaks, Stats.ChunksSeededOnLoad, Stats.SeedsFromLoads,
		Stats.TotalSolveMs, Stats.WorstFrameMs, CVarBudgetMs.GetValueOnGameThread());
}

// ===========================================================================
// Console
// ===========================================================================

namespace
{
	UMadStructuralSubsystem* GetStructural(UWorld* World)
	{
		UMadStructuralSubsystem* Subsystem = World ? World->GetSubsystem<UMadStructuralSubsystem>() : nullptr;
		if (Subsystem == nullptr)
		{
			UE_LOG(LogMadFallStructural, Error, TEXT("No structural subsystem in this world."));
		}
		return Subsystem;
	}

	bool ParseIntArgs(const TArray<FString>& Args, int32 Count, int32* Out)
	{
		if (Args.Num() < Count)
		{
			return false;
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (!FDefaultValueHelper::ParseInt(Args[Index], Out[Index]))
			{
				return false;
			}
		}
		return true;
	}

	const TCHAR* FailureName(EMadStructuralFailure Failure)
	{
		switch (Failure)
		{
		case EMadStructuralFailure::Unsupported: return TEXT("UNSUPPORTED");
		case EMadStructuralFailure::Overloaded:  return TEXT("OVERLOADED");
		default:                                 return TEXT("ok");
		}
	}

	FAutoConsoleCommandWithWorld CmdStatus(
		TEXT("mad.si.status"),
		TEXT("Structural solver counters."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadStructuralSubsystem* S = GetStructural(World))
			{
				UE_LOG(LogMadFallStructural, Display, TEXT("%s"), *S->DescribeStatus());
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdEnable(
		TEXT("mad.si.enable"),
		TEXT("mad.si.enable <0|1> - turns structural collapse off or on."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			int32 Value = 1;
			if (UMadStructuralSubsystem* S = GetStructural(World); S && ParseIntArgs(Args, 1, &Value))
			{
				S->SetEnabled(Value != 0);
				UE_LOG(LogMadFallStructural, Display, TEXT("Structural solver %s."), Value ? TEXT("enabled") : TEXT("disabled"));
			}
		}));

	FAutoConsoleCommandWithWorld CmdFlush(
		TEXT("mad.si.flush"),
		TEXT("Runs every pending structural job to completion now."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadStructuralSubsystem* S = GetStructural(World))
			{
				S->FlushNow();
				UE_LOG(LogMadFallStructural, Display, TEXT("%s"), *S->DescribeStatus());
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdInspect(
		TEXT("mad.si.inspect"),
		TEXT("mad.si.inspect <x> <y> <z> - support distance, load and stress of one block."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			int32 C[3];
			UMadStructuralSubsystem* S = GetStructural(World);
			if (S == nullptr) { return; }
			if (!ParseIntArgs(Args, 3, C))
			{
				UE_LOG(LogMadFallStructural, Error, TEXT("Usage: mad.si.inspect <x> <y> <z>"));
				return;
			}

			FMadStructuralNodeReport Report;
			int32 Members = 0;
			if (!S->Inspect(FIntVector(C[0], C[1], C[2]), Report, Members))
			{
				UE_LOG(LogMadFallStructural, Display, TEXT("(%d, %d, %d) is not a structural member."), C[0], C[1], C[2]);
				return;
			}

			const FString Span = Report.SupportDistance == MadFall::Structural::Unreachable
				? FString(TEXT("none"))
				: FString::Printf(TEXT("%.2f of span"), static_cast<double>(Report.SupportDistance) / MadFall::Structural::SupportScale);

			UE_LOG(LogMadFallStructural, Display,
				TEXT("(%d, %d, %d): %s, support %s, carrying %.0f / %.0f kg, stress %.2f, structure of %d"),
				C[0], C[1], C[2], FailureName(Report.Failure), *Span, Report.CarriedKg, Report.CapacityKg,
				Report.Stress, Members);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdCheck(
		TEXT("mad.si.check"),
		TEXT("mad.si.check <x> <y> <z> [radius=16] - queues every structural block in a box."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			int32 C[3];
			UMadStructuralSubsystem* S = GetStructural(World);
			if (S == nullptr) { return; }
			if (!ParseIntArgs(Args, 3, C))
			{
				UE_LOG(LogMadFallStructural, Error, TEXT("Usage: mad.si.check <x> <y> <z> [radius]"));
				return;
			}

			int32 Radius = 16;
			if (Args.Num() > 3) { FDefaultValueHelper::ParseInt(Args[3], Radius); }
			Radius = FMath::Clamp(Radius, 1, 128);

			const FIntVector Centre(C[0], C[1], C[2]);
			const int32 Queued = S->CheckBox(Centre - FIntVector(Radius), Centre + FIntVector(Radius));
			UE_LOG(LogMadFallStructural, Display, TEXT("Queued %d structural block(s) for checking."), Queued);
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdDamage(
		TEXT("mad.damage"),
		TEXT("mad.damage <x> <y> <z> <amount> [type=madfall:blunt] - damages one voxel."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			int32 C[3];
			UMadStructuralSubsystem* S = GetStructural(World);
			if (S == nullptr) { return; }

			float Amount = 0.0f;
			if (!ParseIntArgs(Args, 3, C) || Args.Num() < 4 || !FDefaultValueHelper::ParseFloat(Args[3], Amount))
			{
				UE_LOG(LogMadFallStructural, Error, TEXT("Usage: mad.damage <x> <y> <z> <amount> [type]"));
				return;
			}

			const FName Type = Args.Num() > 4 ? FName(*Args[4]) : FName(TEXT("madfall:blunt"));
			const FMadBlockDamageResult Result = S->ApplyBlockDamage(FIntVector(C[0], C[1], C[2]), Amount, Type);

			UE_LOG(LogMadFallStructural, Display,
				TEXT("Dealt %.1f effective %s: damage byte %d%s%s%s"),
				Result.EffectiveDamage, *Type.ToString(), Result.NewVoxel.Damage,
				Result.bStageChanged ? TEXT(", stage changed") : TEXT(""),
				Result.bDowngraded ? TEXT(", downgraded") : TEXT(""),
				Result.bDestroyed ? TEXT(", destroyed") : TEXT(""));
		}));
}

// ===========================================================================
// Stress probe
// ===========================================================================

EMadStressQuery UMadStructuralSubsystem::QueryStress(const FIntVector& Position, FMadStructuralNodeReport& OutReport)
{
	if (VoxelWorld == nullptr || !VoxelWorld->IsVoxelLoaded(Position.X, Position.Y, Position.Z)
		|| FMadStructuralJob::Classify(VoxelWorld->GetVoxel(Position.X, Position.Y, Position.Z), GetMaterials()) != EMadStructuralRole::Member)
	{
		return EMadStressQuery::NotMember;
	}

	const double Now = FPlatformTime::Seconds();
	LastQueryTime = Now;
	const bool bSameTarget = Position == ProbePosition;

	if (!bSameTarget)
	{
		// A new target: drop the old answer rather than show it on the wrong block.
		ProbePosition = Position;
		bProbeHasResult = false;
		ProbeJob.Reset();
	}

	// Refresh about once a second: the structure may have changed under it.
	if (!ProbeJob.IsValid() && (!bProbeHasResult || Now - ProbeResultTime > 1.0))
	{
		FMadStructuralSettings ProbeSettings = Settings;
		ProbeSettings.MaxComponentSize = FMath::Max(64, CVarProbeMaxMembers.GetValueOnGameThread());
		ProbeJob = MakeUnique<FMadStructuralJob>(TArray<FIntVector>{ Position }, ProbeSettings);
	}

	if (!bProbeHasResult)
	{
		return EMadStressQuery::Pending;
	}
	OutReport = ProbeReport;
	return ProbeResult;
}

void UMadStructuralSubsystem::TickProbe()
{
	if (!ProbeJob.IsValid() || VoxelWorld == nullptr)
	{
		return;
	}

	const double Start = FPlatformTime::Seconds();
	const double Budget = FMath::Max(0.01, static_cast<double>(CVarProbeBudgetMs.GetValueOnGameThread())) / 1000.0;

	FLiveStructuralWorld World(*VoxelWorld);
	const FMadStructuralMaterials& Mats = GetMaterials();

	bool bDone = false;
	while (!bDone && FPlatformTime::Seconds() - Start < Budget)
	{
		bDone = ProbeJob->Step(World, Mats, WorkUnitsPerStep);
	}
	if (!bDone)
	{
		return;
	}

	if (ProbeJob->WasTruncated())
	{
		ProbeResult = EMadStressQuery::TooLarge;
		ProbeReport = FMadStructuralNodeReport();
		StressField.Reset();
	}
	else
	{
		ProbeResult = ProbeJob->GetNodeReport(ProbePosition, ProbeReport) ? EMadStressQuery::Ready : EMadStressQuery::NotMember;
		ProbeJob->CollectStressNear(ProbePosition, FMath::Clamp(CVarOverlayRadius.GetValueOnGameThread(), 1, 32),
			FMath::Clamp(CVarOverlayMaxBlocks.GetValueOnGameThread(), 1, 8192), StressField);
	}
	++StressFieldVersion;
	bProbeHasResult = true;
	ProbeResultTime = FPlatformTime::Seconds();
	ProbeJob.Reset();
}

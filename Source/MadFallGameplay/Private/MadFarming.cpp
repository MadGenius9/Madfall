// Copyright MadFall. All Rights Reserved.

#include "MadFarming.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "MadBlockDefinition.h"
#include "MadBlockRegistry.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"

namespace
{
	bool Grows(const FMadBlockDefinitionData* Block)
	{
		return Block != nullptr && !Block->GrowInto.IsNone() && Block->GrowHours > 0.0f;
	}
}

// --- FMadPlantTracker ----------------------------------------------------------

void FMadPlantTracker::NoteBlock(const FIntVector& Position, const FMadBlockDefinitionData* Block, double StageStartHour)
{
	if (!Grows(Block))
	{
		Plants.Remove(Position);
		return;
	}

	FPlant& Plant = Plants.FindOrAdd(Position);
	Plant.Block = Block->Id;
	Plant.Into = Block->GrowInto;
	Plant.StageStartHour = StageStartHour;
	Plant.DueHour = StageStartHour + Block->GrowHours;
}

void FMadPlantTracker::CollectDue(double NowHour, TArray<FDue>& Out) const
{
	const int32 First = Out.Num();
	for (const TPair<FIntVector, FPlant>& Pair : Plants)
	{
		if (Pair.Value.DueHour <= NowHour)
		{
			Out.Add({ Pair.Key, Pair.Value.Block, Pair.Value.Into, Pair.Value.DueHour });
		}
	}

	// Earliest first, then by position, so the order never depends on map layout
	// and a field planted together ripens in the same order every time.
	Algo::Sort(MakeArrayView(Out.GetData() + First, Out.Num() - First), [](const FDue& A, const FDue& B)
	{
		if (A.NextStageStartHour != B.NextStageStartHour) { return A.NextStageStartHour < B.NextStageStartHour; }
		if (A.Position.X != B.Position.X) { return A.Position.X < B.Position.X; }
		if (A.Position.Y != B.Position.Y) { return A.Position.Y < B.Position.Y; }
		return A.Position.Z < B.Position.Z;
	});
}

double FMadPlantTracker::HoursUntilNext(double NowHour) const
{
	double Next = TNumericLimits<double>::Max();
	for (const TPair<FIntVector, FPlant>& Pair : Plants)
	{
		Next = FMath::Min(Next, Pair.Value.DueHour - NowHour);
	}
	return Next;
}

void FMadPlantTracker::Advance(double Hours)
{
	for (TPair<FIntVector, FPlant>& Pair : Plants)
	{
		Pair.Value.StageStartHour -= Hours;
		Pair.Value.DueHour -= Hours;
	}
}

void FMadPlantTracker::Export(TArray<FMadPlantSaveData>& Out) const
{
	Out.Reset(Plants.Num());
	for (const TPair<FIntVector, FPlant>& Pair : Plants)
	{
		Out.Add({ Pair.Key, Pair.Value.Block, Pair.Value.StageStartHour });
	}
	// Sorted so two saves of the same farm are byte-identical.
	Out.Sort([](const FMadPlantSaveData& A, const FMadPlantSaveData& B)
	{
		if (A.Position.X != B.Position.X) { return A.Position.X < B.Position.X; }
		if (A.Position.Y != B.Position.Y) { return A.Position.Y < B.Position.Y; }
		return A.Position.Z < B.Position.Z;
	});
}

void FMadPlantTracker::Import(const TArray<FMadPlantSaveData>& In, const FMadBlockRegistry& Blocks)
{
	Plants.Reset();
	for (const FMadPlantSaveData& Saved : In)
	{
		const FMadBlockDefinitionData* Block = Blocks.FindDefinition(Blocks.ResolveRuntimeId(Saved.Block));
		if (Grows(Block))
		{
			NoteBlock(Saved.Position, Block, Saved.StageStartHour);
			continue;
		}

		// A crop from a removed mod keeps its record, like an unknown item keeps
		// its slot, so the farm resumes if the mod returns. It never comes due.
		FPlant& Plant = Plants.Add(Saved.Position);
		Plant.Block = Saved.Block;
		Plant.StageStartHour = Saved.StageStartHour;
		Plant.DueHour = TNumericLimits<double>::Max();
	}
}

// --- UMadFarmingSubsystem ------------------------------------------------------

bool UMadFarmingSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UMadFarmingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	VoxelWorld = Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
	Clock = Collection.InitializeDependency<UMadWorldClockSubsystem>();
	if (VoxelWorld != nullptr)
	{
		VoxelChangedHandle = VoxelWorld->OnVoxelChanged().AddUObject(this, &UMadFarmingSubsystem::HandleVoxelChanged);
	}
}

void UMadFarmingSubsystem::Deinitialize()
{
	if (VoxelWorld != nullptr)
	{
		VoxelWorld->OnVoxelChanged().Remove(VoxelChangedHandle);
	}
	Super::Deinitialize();
}

TStatId UMadFarmingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadFarmingSubsystem, STATGROUP_Tickables);
}

double UMadFarmingSubsystem::GetNowHour() const
{
	return Clock != nullptr ? Clock->GetTotalHours() : 0.0;
}

void UMadFarmingSubsystem::HandleVoxelChanged(const FIntVector& Position, const FMadVoxel& Before, const FMadVoxel& After)
{
	// A hit on a crop rewrites its voxel with more damage; that is not a replant.
	if (Before.BlockTypeID == After.BlockTypeID && Tracker.Find(Position) != nullptr)
	{
		return;
	}

	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const FMadBlockDefinitionData* Block = Blocks.FindDefinition(After.BlockTypeID);
	if (!Grows(Block) && Tracker.Find(Position) == nullptr)
	{
		return;
	}

	const bool bCarried = GrowingWrite.IsSet() && GrowingWrite->Key == Position;
	Tracker.NoteBlock(Position, Block, bCarried ? GrowingWrite->Value : GetNowHour());
}

bool UMadFarmingSubsystem::GetStageProgress(const FIntVector& Position, float& OutProgress) const
{
	const FMadPlantTracker::FPlant* Plant = Tracker.Find(Position);
	if (Plant == nullptr || Plant->DueHour <= Plant->StageStartHour || Plant->DueHour >= TNumericLimits<double>::Max())
	{
		return false;
	}
	OutProgress = static_cast<float>(FMath::Clamp((GetNowHour() - Plant->StageStartHour) / (Plant->DueHour - Plant->StageStartHour), 0.0, 1.0));
	return true;
}

void UMadFarmingSubsystem::Advance(double Hours)
{
	Tracker.Advance(Hours);
	CheckTimer = CheckInterval;
}

void UMadFarmingSubsystem::ImportState(const TArray<FMadPlantSaveData>& In)
{
	Tracker.Import(In, UMadVoxelWorldSubsystem::GetBlockRegistry());
	UE_LOG(LogMadFallGameplay, Log, TEXT("Farming: restored %d plant(s)."), Tracker.Num());
}

void UMadFarmingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	CheckTimer += DeltaTime;
	if (CheckTimer < CheckInterval || VoxelWorld == nullptr || Tracker.Num() == 0)
	{
		return;
	}
	CheckTimer = 0.0f;

	MAD_FRAME_SCOPE(Other);

	TArray<FMadPlantTracker::FDue> Due;
	Tracker.CollectDue(GetNowHour(), Due);

	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	int32 Grown = 0;
	for (const FMadPlantTracker::FDue& Plant : Due)
	{
		if (Grown >= MaxGrowthsPerCheck)
		{
			break;
		}

		const FIntVector& P = Plant.Position;
		if (!VoxelWorld->IsVoxelLoaded(P.X, P.Y, P.Z))
		{
			// Waits, start hour intact, for its chunk to come back.
			continue;
		}

		FMadVoxel Voxel = VoxelWorld->GetVoxel(P.X, P.Y, P.Z);
		if (Blocks.GetStringId(Voxel.BlockTypeID) != Plant.From)
		{
			// Replaced without a notification reaching us (a region reverted under
			// a newer gameplay save). Whatever is there now is not this plant.
			Tracker.Forget(P);
			continue;
		}

		const uint16 IntoId = Blocks.ResolveRuntimeId(Plant.Into);
		if (Blocks.FindDefinition(IntoId) == nullptr)
		{
			UE_LOG(LogMadFallGameplay, Warning, TEXT("Farming: %s grows into unknown block %s; it stops growing."),
				*Plant.From.ToString(), *Plant.Into.ToString());
			Tracker.Forget(P);
			continue;
		}

		// Same orientation and flags; a new stage starts undamaged.
		Voxel.BlockTypeID = IntoId;
		Voxel.Damage = 0;

		GrowingWrite = TPair<FIntVector, double>(P, Plant.NextStageStartHour);
		const bool bWritten = VoxelWorld->SetVoxel(P.X, P.Y, P.Z, Voxel);
		GrowingWrite.Reset();

		if (bWritten)
		{
			++Grown;
			UE_LOG(LogMadFallGameplay, Verbose, TEXT("Farming: %s grew into %s at %s."),
				*Plant.From.ToString(), *Plant.Into.ToString(), *P.ToString());
		}
	}

	if (Grown > 0)
	{
		UE_LOG(LogMadFallGameplay, Log, TEXT("Farming: %d plant(s) grew."), Grown);
	}
}

FString UMadFarmingSubsystem::DescribeStatus() const
{
	const double Now = GetNowHour();
	TArray<FMadPlantTracker::FDue> Due;
	Tracker.CollectDue(Now, Due);
	const double Next = Tracker.HoursUntilNext(Now);
	return FString::Printf(TEXT("Farming: %d plant(s), %d due, next stage in %s."), Tracker.Num(), Due.Num(),
		Tracker.Num() == 0 || Next >= TNumericLimits<double>::Max() * 0.5 ? TEXT("-") : *FString::Printf(TEXT("%.1f h"), FMath::Max(0.0, Next)));
}

static FAutoConsoleCommandWithWorld GMadFarmStatusCommand(
	TEXT("mad.farm.status"),
	TEXT("Prints how many plants are growing and when the next stage is due."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadFarmingSubsystem* Farming = World ? World->GetSubsystem<UMadFarmingSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Farming->DescribeStatus());
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadFarmAdvanceCommand(
	TEXT("mad.farm.advance"),
	TEXT("mad.farm.advance <hours>: every plant grows as if that many in-game hours had passed."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadFarmingSubsystem* Farming = World ? World->GetSubsystem<UMadFarmingSubsystem>() : nullptr;
		if (Farming == nullptr || Args.Num() < 1)
		{
			UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.farm.advance <hours> (game worlds only)"));
			return;
		}
		const double Hours = FCString::Atod(*Args[0]);
		Farming->Advance(Hours);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Farming: advanced %d plant(s) by %.1f h."), Farming->GetTracker().Num(), Hours);
	}));

// Copyright MadFall. All Rights Reserved.

#include "MadGameplaySaveSubsystem.h"

#include "MadFrameBudget.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "MadContainerSubsystem.h"
#include "MadFallGameplay.h"
#include "MadFarming.h"
#include "MadWorldMap.h"
#include "MadHordeSubsystem.h"
#include "MadPickupSubsystem.h"
#include "MadPlayerCharacter.h"
#include "MadScriptSubsystem.h"
#include "MadTrading.h"
#include "MadSession.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "Misc/Paths.h"

namespace
{
	TAutoConsoleVariable<float> CVarAutosaveMinutes(
		TEXT("mad.save.AutosaveMinutes"),
		5.0f,
		TEXT("Real-time minutes between autosaves of voxels and gameplay. 0 disables."),
		ECVF_Default);

}

bool UMadGameplaySaveSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadGameplaySaveSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadGameplaySaveSubsystem, STATGROUP_Tickables);
}

FString UMadGameplaySaveSubsystem::GetSavePath() const
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld() ? GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	return VoxelWorld ? FPaths::Combine(VoxelWorld->GetWorldDirectory(), TEXT("gameplay.json")) : FString();
}

void UMadGameplaySaveSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
	Collection.InitializeDependency<UMadWorldClockSubsystem>();
	Collection.InitializeDependency<UMadContainerSubsystem>();
	Collection.InitializeDependency<UMadHordeSubsystem>();
	Collection.InitializeDependency<UMadPickupSubsystem>();
	Collection.InitializeDependency<UMadFarmingSubsystem>();
	Collection.InitializeDependency<UMadWorldMapSubsystem>();
	Collection.InitializeDependency<UMadScriptSubsystem>();
	Collection.InitializeDependency<UMadTraderSubsystem>();

	const FString Path = GetSavePath();
	if (Path.IsEmpty() || (!IFileManager::Get().FileExists(*Path) && !IFileManager::Get().FileExists(*(Path + TEXT(".bak")))))
	{
		UE_LOG(LogMadFallGameplay, Log, TEXT("No gameplay save at %s; starting fresh."), *Path);
		return;
	}

	FMadGameplaySave Save;
	TArray<FString> Warnings;
	bool bUsedBackup = false;
	if (!MadFall::GameplaySave::ReadFile(Path, Save, Warnings, bUsedBackup))
	{
		// A corrupt save is kept on disk untouched, not overwritten by the next
		// autosave of a fresh game: rename it out of the way first.
		const FString Corrupt = Path + TEXT(".corrupt");
		IFileManager::Get().Move(*Corrupt, *Path, /*Replace*/ true);
		UE_LOG(LogMadFallGameplay, Error, TEXT("Gameplay save %s could not be read and was moved to %s: %s"),
			*Path, *Corrupt, Warnings.Num() > 0 ? *Warnings[0] : TEXT("unknown error"));
		return;
	}

	for (const FString& Warning : Warnings)
	{
		UE_LOG(LogMadFallGameplay, Warning, TEXT("Gameplay save: %s"), *Warning);
	}

	if (const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
		VoxelWorld && Save.WorldSeed != 0 && Save.WorldSeed != VoxelWorld->GetSeed())
	{
		UE_LOG(LogMadFallGameplay, Warning, TEXT("Gameplay save was written for seed %lld but the world is seed %lld."),
			Save.WorldSeed, VoxelWorld->GetSeed());
	}

	UE_LOG(LogMadFallGameplay, Display, TEXT("Loaded gameplay save%s: day %d, %d container(s), player %s."),
		bUsedBackup ? TEXT(" (from backup)") : TEXT(""), Save.Day, Save.Containers.Num(), Save.bHasPlayer ? TEXT("yes") : TEXT("no"));
	Loaded = MoveTemp(Save);
}

void UMadGameplaySaveSubsystem::ApplyWorldState()
{
	bWorldStateApplied = true;
	if (!Loaded.IsSet())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (UMadWorldClockSubsystem* Clock = World->GetSubsystem<UMadWorldClockSubsystem>())
	{
		Clock->SetTime(Loaded->TimeOfDay, Loaded->Day);
	}
	if (UMadContainerSubsystem* Containers = World->GetSubsystem<UMadContainerSubsystem>())
	{
		Containers->ImportState(Loaded->Containers);
	}
	if (UMadHordeSubsystem* Horde = World->GetSubsystem<UMadHordeSubsystem>())
	{
		Horde->ImportSleeperDays(Loaded->SleeperDays);
	}
	if (UMadPickupSubsystem* Pickups = World->GetSubsystem<UMadPickupSubsystem>())
	{
		Pickups->ImportState(Loaded->Pickups);
	}
	if (UMadFarmingSubsystem* Farming = World->GetSubsystem<UMadFarmingSubsystem>())
	{
		Farming->ImportState(Loaded->Plants);
	}
	if (UMadWorldMapSubsystem* Map = World->GetSubsystem<UMadWorldMapSubsystem>())
	{
		Map->ImportState(Loaded->Explored);
	}
	if (UMadScriptSubsystem* Scripts = World->GetSubsystem<UMadScriptSubsystem>())
	{
		Scripts->ImportStore(Loaded->ModStore);
	}
	if (UMadTraderSubsystem* Traders = World->GetSubsystem<UMadTraderSubsystem>())
	{
		Traders->ImportState(Loaded->Traders);
	}
}

bool UMadGameplaySaveSubsystem::ConsumePlayerSave(FMadPlayerSaveData& OutPlayer)
{
	if (bPlayerConsumed || !Loaded.IsSet() || !Loaded->bHasPlayer)
	{
		return false;
	}
	bPlayerConsumed = true;
	OutPlayer = Loaded->Player;
	return true;
}

void UMadGameplaySaveSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Saving);

	if (!bWorldStateApplied)
	{
		ApplyWorldState();
	}

	const float Minutes = CVarAutosaveMinutes.GetValueOnGameThread();
	if (Minutes <= 0.0f)
	{
		return;
	}

	AutosaveTimer += DeltaTime;
	if (AutosaveTimer >= Minutes * 60.0f)
	{
		AutosaveTimer = 0.0f;
		// Voxels too: before this only gameplay was autosaved, so blocks placed in
		// chunks that never unloaded (the base the survivor stands in) were lost to
		// a crash while the materials spent on them were saved. The world write
		// runs on a worker; the copy is taken in the same frame as the gameplay
		// state, so the two agree.
		if (UMadVoxelWorldSubsystem* VoxelWorld = GetWorld() ? GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr)
		{
			VoxelWorld->SaveAllAsync();
		}
		SaveNow();
	}
}

bool UMadGameplaySaveSubsystem::SaveNow(AMadPlayerCharacter* Player)
{
	UWorld* World = GetWorld();
	const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	if (VoxelWorld == nullptr || VoxelWorld->IsReadOnly())
	{
		return false;
	}

	if (Player == nullptr)
	{
		Player = MadFall::FindLocalPlayer(World);
	}

	FMadGameplaySave Save;
	Save.WorldSeed = VoxelWorld->GetSeed();

	if (const UMadWorldClockSubsystem* Clock = World->GetSubsystem<UMadWorldClockSubsystem>())
	{
		Save.Day = Clock->GetDay();
		Save.TimeOfDay = Clock->GetTimeOfDay();
	}

	// A player still waiting for the world has not been placed yet; saving now
	// would overwrite their real position with the spawn estimate. Keep the
	// loaded player data instead.
	if (Player != nullptr && !Player->IsWaitingForWorld())
	{
		Save.bHasPlayer = true;
		Player->SaveState(Save.Player);
	}
	else if (Loaded.IsSet() && Loaded->bHasPlayer)
	{
		Save.bHasPlayer = true;
		Save.Player = Loaded->Player;
	}

	if (const UMadContainerSubsystem* Containers = World->GetSubsystem<UMadContainerSubsystem>())
	{
		Containers->ExportState(Save.Containers);
	}
	if (const UMadHordeSubsystem* Horde = World->GetSubsystem<UMadHordeSubsystem>())
	{
		Save.SleeperDays = Horde->GetSleeperDays();
	}
	if (const UMadPickupSubsystem* Pickups = World->GetSubsystem<UMadPickupSubsystem>())
	{
		Pickups->ExportState(Save.Pickups);
	}
	if (const UMadFarmingSubsystem* Farming = World->GetSubsystem<UMadFarmingSubsystem>())
	{
		Farming->ExportState(Save.Plants);
	}
	if (const UMadWorldMapSubsystem* Map = World->GetSubsystem<UMadWorldMapSubsystem>())
	{
		Map->ExportState(Save.Explored);
	}
	if (const UMadScriptSubsystem* Scripts = World->GetSubsystem<UMadScriptSubsystem>())
	{
		Scripts->ExportStore(Save.ModStore);
	}
	if (const UMadTraderSubsystem* Traders = World->GetSubsystem<UMadTraderSubsystem>())
	{
		Traders->ExportState(Save.Traders);
	}

	const FString Path = GetSavePath();
	FString Error;
	if (!MadFall::GameplaySave::WriteFile(Path, Save, Error))
	{
		UE_LOG(LogMadFallGameplay, Error, TEXT("Gameplay save failed: %s"), *Error);
		return false;
	}

	// The world list shows when a world was last played and how far in it is.
	FMadWorldInfo Info;
	if (MadFall::Session::ReadWorldInfo(VoxelWorld->GetWorldDirectory(), Info))
	{
		Info.LastPlayed = FDateTime::UtcNow();
		Info.Day = Save.Day;
		MadFall::Session::WriteWorldInfo(Info, Error);
	}

	UE_LOG(LogMadFallGameplay, Display, TEXT("Gameplay saved to %s (day %d, %d container(s), %d plant(s))."), *Path, Save.Day, Save.Containers.Num(), Save.Plants.Num());
	return true;
}

static FAutoConsoleCommandWithWorld GMadSaveCommand(
	TEXT("mad.save"),
	TEXT("Saves the voxel world and all gameplay state."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
		if (VoxelWorld != nullptr)
		{
			FString Error;
			if (!VoxelWorld->SaveAll(Error))
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("Voxel save failed: %s"), *Error);
			}
		}
		if (UMadGameplaySaveSubsystem* Saves = World ? World->GetSubsystem<UMadGameplaySaveSubsystem>() : nullptr)
		{
			Saves->SaveNow();
		}
		else
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("Voxels saved; gameplay state is only saved in game worlds."));
		}
	}));

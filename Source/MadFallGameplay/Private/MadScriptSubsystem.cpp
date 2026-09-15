// Copyright MadFall. All Rights Reserved.

#include "MadScriptSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MadBlockDefinition.h"
#include "MadBlockRegistry.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadGameplayDefinitions.h"
#include "MadGameplaySaveSubsystem.h"
#include "MadHordeSubsystem.h"
#include "MadModManager.h"
#include "MadPlayerCharacter.h"
#include "MadSurvivorComponents.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "MadZombie.h"

UMadScriptSubsystem::UMadScriptSubsystem() = default;
UMadScriptSubsystem::~UMadScriptSubsystem() = default;

bool UMadScriptSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UMadScriptSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	VoxelWorld = Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
	Clock = Collection.InitializeDependency<UMadWorldClockSubsystem>();
	Horde = Collection.InitializeDependency<UMadHordeSubsystem>();

	Runtime = MakeUnique<FMadScriptRuntime>(*this);

	if (Clock != nullptr)
	{
		DawnHandle = Clock->OnDawn().AddWeakLambda(this, [this](int32 Day)
		{
			FMadScriptEvent Event(TEXT("dawn"));
			Event.Add(TEXT("day"), static_cast<double>(Day));
			QueueEvent(MoveTemp(Event));
		});
		DuskHandle = Clock->OnDusk().AddWeakLambda(this, [this](int32 Day)
		{
			FMadScriptEvent Event(TEXT("dusk"));
			Event.Add(TEXT("day"), static_cast<double>(Day));
			Event.Add(TEXT("horde"), Clock != nullptr && Clock->IsHordeNight());
			QueueEvent(MoveTemp(Event));
		});
	}
}

void UMadScriptSubsystem::Deinitialize()
{
	if (Clock != nullptr)
	{
		Clock->OnDawn().Remove(DawnHandle);
		Clock->OnDusk().Remove(DuskHandle);
	}
	// Closes every Lua state and unregisters the mods' console commands.
	Runtime.Reset();
	Queue.Reset();
	Super::Deinitialize();
}

TStatId UMadScriptSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadScriptSubsystem, STATGROUP_Tickables);
}

AMadPlayerCharacter* UMadScriptSubsystem::FindPlayer() const
{
	const APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return Controller ? Cast<AMadPlayerCharacter>(Controller->GetPawn()) : nullptr;
}

// --- loading ----------------------------------------------------------------------------

void UMadScriptSubsystem::LoadNextMod()
{
	if (!bModsGathered)
	{
		bModsGathered = true;
		for (const FMadModManifest& Manifest : MadFall::GetModManager().GetResolution().LoadOrder)
		{
			if (Manifest.Scripts.Num() > 0)
			{
				ModsToLoad.Add({ Manifest.Id, Manifest.Directory, Manifest.Scripts });
			}
		}
		if (ModsToLoad.Num() > 0)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("Scripts: %d script mod(s) to load."), ModsToLoad.Num());
		}
	}
	if (NextModToLoad < ModsToLoad.Num() && Runtime.IsValid())
	{
		Runtime->LoadMods({ ModsToLoad[NextModToLoad] });
		++NextModToLoad;
	}
}

void UMadScriptSubsystem::LoadAllNow()
{
	do
	{
		LoadNextMod();
	}
	while (!HasLoadedAll());
}

// --- events -----------------------------------------------------------------------------

void UMadScriptSubsystem::QueueEvent(FMadScriptEvent&& Event)
{
	if (!Runtime.IsValid() || !Runtime->HasHandlers(Event.Name))
	{
		return;
	}
	if (Queue.Num() >= MaxQueuedEvents)
	{
		Queue.RemoveAt(0, 1, EAllowShrinking::No);
		if (EventsDropped++ == 0)
		{
			UE_LOG(LogMadFallGameplay, Warning, TEXT("Scripts: more than %d events waiting; the oldest are being dropped. A script handler is too slow."), MaxQueuedEvents);
		}
	}
	Queue.Add(MoveTemp(Event));
}

void UMadScriptSubsystem::NotifyPlayerAction(EMadQuestObjectiveType Type, FName Id, int32 Amount, const TOptional<FIntVector>& Where)
{
	const TCHAR* Name = nullptr;
	const TCHAR* IdField = TEXT("block");
	switch (Type)
	{
	case EMadQuestObjectiveType::Place:      Name = TEXT("block_placed"); break;
	case EMadQuestObjectiveType::Break:      Name = TEXT("block_broken"); break;
	case EMadQuestObjectiveType::SetSpawn:   Name = TEXT("bed_set"); break;
	case EMadQuestObjectiveType::Craft:      Name = TEXT("item_crafted"); IdField = TEXT("item"); break;
	case EMadQuestObjectiveType::Wear:       Name = TEXT("item_worn"); IdField = TEXT("item"); break;
	case EMadQuestObjectiveType::KillZombie: Name = TEXT("zombie_killed"); IdField = TEXT("zombie"); break;
	case EMadQuestObjectiveType::KillAnimal: Name = TEXT("animal_killed"); IdField = TEXT("animal"); break;
	default: return;
	}

	FMadScriptEvent Event{ FName(Name) };
	if (!Runtime.IsValid() || !Runtime->HasHandlers(Event.Name))
	{
		return;
	}
	Event.Add(IdField, Id.ToString());
	Event.Add(TEXT("count"), static_cast<double>(Amount));
	if (Where.IsSet())
	{
		Event.Add(TEXT("x"), static_cast<double>(Where->X));
		Event.Add(TEXT("y"), static_cast<double>(Where->Y));
		Event.Add(TEXT("z"), static_cast<double>(Where->Z));
	}
	QueueEvent(MoveTemp(Event));
}

void UMadScriptSubsystem::PollState(float DeltaTime)
{
	// Transitions are polled rather than hooked: each is one bool compare a
	// frame, and polling keeps the player and horde code free of script calls.
	const AMadPlayerCharacter* Player = FindPlayer();
	// Down covers both waiting for the world and dead-awaiting-respawn, so a respawn is a new spawn.
	const bool bActive = Player != nullptr && !Player->IsDown();
	const bool bDead = Player != nullptr && Player->GetSurvival() != nullptr && Player->GetSurvival()->IsDead();
	if (bActive && !bPlayerWasActive)
	{
		const FIntVector Feet = Player->GetFeetVoxel();
		FMadScriptEvent Event(TEXT("player_spawned"));
		Event.Add(TEXT("x"), static_cast<double>(Feet.X)).Add(TEXT("y"), static_cast<double>(Feet.Y)).Add(TEXT("z"), static_cast<double>(Feet.Z));
		QueueEvent(MoveTemp(Event));
	}
	if (bDead && !bPlayerWasDead)
	{
		QueueEvent(FMadScriptEvent(TEXT("player_died")));
	}
	bPlayerWasActive = bActive;
	bPlayerWasDead = bDead;

	const bool bHorde = Horde != nullptr && Horde->IsHordeActive();
	if (bHorde && !bHordeWasActive)
	{
		FMadScriptEvent Event(TEXT("horde_night"));
		Event.Add(TEXT("day"), static_cast<double>(Clock ? Clock->GetNightDay() : 0));
		QueueEvent(MoveTemp(Event));
	}
	bHordeWasActive = bHorde;

	SecondTimer += DeltaTime;
	if (SecondTimer >= 1.0f)
	{
		SecondTimer = FMath::Fmod(SecondTimer, 1.0f);
		FMadScriptEvent Event(TEXT("second"));
		Event.Add(TEXT("day"), static_cast<double>(Clock ? Clock->GetDay() : 1));
		Event.Add(TEXT("hour"), static_cast<double>(Clock ? Clock->GetTimeOfDay() : 0.0f));
		QueueEvent(MoveTemp(Event));
	}
}

void UMadScriptSubsystem::DrainAllNow()
{
	while (Queue.Num() > 0 && Runtime.IsValid())
	{
		const FMadScriptEvent Event = MoveTemp(Queue[0]);
		Queue.RemoveAt(0, 1, EAllowShrinking::No);
		Runtime->Dispatch(Event);
		++EventsDispatched;
	}
}

void UMadScriptSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Runtime.IsValid())
	{
		return;
	}

	MAD_FRAME_SCOPE(Scripts);

	// Scripts see the world as the save left it: wait for the save to be applied.
	if (const UMadGameplaySaveSubsystem* Save = GetWorld()->GetSubsystem<UMadGameplaySaveSubsystem>(); Save && !Save->HasAppliedWorldState())
	{
		return;
	}
	if (!HasLoadedAll() || !bModsGathered)
	{
		LoadNextMod();
		return;
	}
	if (!bWorldLoadedSent)
	{
		bWorldLoadedSent = true;
		int32 Day = 1;
		float Hour = 0.0f;
		GetTime(Day, Hour);
		FMadScriptEvent Event(TEXT("world_loaded"));
		Event.Add(TEXT("day"), static_cast<double>(Day)).Add(TEXT("hour"), static_cast<double>(Hour));
		QueueEvent(MoveTemp(Event));
	}

	PollState(DeltaTime);

	// At least one event a frame so the queue always moves; after that, only
	// while the frame has budget left.
	const double Start = FPlatformTime::Seconds();
	const double AllowedMs = MadFall::FrameBudget::GetRemainingMs(ScriptShareMs, 0.0);
	int32 Dispatched = 0;
	while (Queue.Num() > 0)
	{
		if (Dispatched > 0 && (FPlatformTime::Seconds() - Start) * 1000.0 >= AllowedMs)
		{
			break;
		}
		const FMadScriptEvent Event = MoveTemp(Queue[0]);
		Queue.RemoveAt(0, 1, EAllowShrinking::No);
		Runtime->Dispatch(Event);
		++Dispatched;
		++EventsDispatched;
	}
}

// --- store ------------------------------------------------------------------------------

void UMadScriptSubsystem::ExportStore(TMap<FName, TMap<FString, FMadScriptValue>>& Out) const
{
	Out.Reset();
	if (Runtime.IsValid())
	{
		Runtime->ExportStore(Out);
	}
}

void UMadScriptSubsystem::ImportStore(const TMap<FName, TMap<FString, FMadScriptValue>>& In)
{
	if (Runtime.IsValid())
	{
		Runtime->ImportStore(In);
	}
}

// --- IMadScriptHost ---------------------------------------------------------------------

void UMadScriptSubsystem::Log(FName ModId, const FString& Message)
{
	UE_LOG(LogMadFallGameplay, Display, TEXT("Script [%s]: %s"), *ModId.ToString(), *Message);
}

void UMadScriptSubsystem::Warn(FName ModId, const FString& Message)
{
	UE_LOG(LogMadFallGameplay, Warning, TEXT("Script error [%s]: %s"), *ModId.ToString(), *Message);
}

void UMadScriptSubsystem::ShowMessage(FName ModId, const FString& Message)
{
	UE_LOG(LogMadFallGameplay, Display, TEXT("Script message [%s]: %s"), *ModId.ToString(), *Message);
	if (AMadPlayerCharacter* Player = FindPlayer())
	{
		// Bounded: a HUD line is not a place for a script's essay.
		Player->PushMessage(Message.Left(160), 4.0f);
	}
}

bool UMadScriptSubsystem::GetBlock(const FIntVector& Voxel, FName& OutBlock)
{
	if (VoxelWorld == nullptr || !VoxelWorld->IsVoxelLoaded(Voxel.X, Voxel.Y, Voxel.Z))
	{
		return false;
	}
	OutBlock = UMadVoxelWorldSubsystem::GetBlockRegistry().GetStringId(VoxelWorld->GetVoxel(Voxel.X, Voxel.Y, Voxel.Z).BlockTypeID);
	return !OutBlock.IsNone();
}

bool UMadScriptSubsystem::SetBlock(FName ModId, const FIntVector& Voxel, FName Block)
{
	if (VoxelWorld == nullptr || VoxelWorld->IsReadOnly() || !VoxelWorld->IsVoxelLoaded(Voxel.X, Voxel.Y, Voxel.Z))
	{
		return false;
	}
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	if (!Blocks.IsRegistered(Block))
	{
		return false;
	}
	const uint16 RuntimeId = Blocks.ResolveRuntimeId(Block);
	const FMadBlockDefinitionData* Definition = Blocks.FindDefinition(RuntimeId);

	// Built the way the survivor's placement builds a voxel, so the mesher,
	// structural solver and farming see a scripted block as a placed one.
	FMadVoxel Written;
	Written.BlockTypeID = RuntimeId;
	Written.Damage = 0;
	Written.Rotation = 0;
	Written.Flags = 0;
	if (RuntimeId == MadFall::BlockTypeAir || Definition == nullptr)
	{
		Written.Density = 0;
	}
	else
	{
		Written.Density = 255;
		Written.SetFlag(EMadVoxelFlags::Cubic, Definition->ShapeKind != EMadBlockShapeKind::Isosurface);
		Written.SetFlag(EMadVoxelFlags::Liquid, Definition->bLiquid);
	}
	if (!VoxelWorld->SetVoxel(Voxel.X, Voxel.Y, Voxel.Z, Written))
	{
		return false;
	}
	++BlocksSet;
	UE_LOG(LogMadFallGameplay, Verbose, TEXT("Script [%s] set %s at %s."), *ModId.ToString(), *Block.ToString(), *Voxel.ToString());
	return true;
}

bool UMadScriptSubsystem::GetPlayer(FMadScriptPlayer& Out)
{
	const AMadPlayerCharacter* Player = FindPlayer();
	if (Player == nullptr || Player->IsWaitingForWorld() || Player->GetSurvival() == nullptr)
	{
		return false;
	}
	const FMadSurvivalStats Stats = Player->GetSurvival()->GetStats();
	Out.Voxel = Player->GetFeetVoxel();
	Out.Health = Stats.Health;
	Out.MaxHealth = Stats.MaxHealth;
	Out.Food = Stats.Food;
	Out.Water = Stats.Water;
	Out.CoreTemperature = Stats.CoreTemperature;
	Out.Level = Player->GetLevel();
	Out.bAlive = !Player->IsDown();
	return true;
}

int32 UMadScriptSubsystem::GiveItem(FName ModId, FName Item, int32 Count)
{
	AMadPlayerCharacter* Player = FindPlayer();
	const FMadItemDefinition* Definition = MadFall::GetGameplayDefinitions().FindItem(Item);
	if (Player == nullptr || Definition == nullptr || Count <= 0)
	{
		return 0;
	}
	Player->GiveOrDrop({ FMadItemStack::Make(*Definition, Count) }, Player->GetActorLocation());
	ItemsGiven += Count;
	UE_LOG(LogMadFallGameplay, Log, TEXT("Script [%s] gave %d x %s."), *ModId.ToString(), Count, *Item.ToString());
	return Count;
}

bool UMadScriptSubsystem::SpawnZombie(FName ModId, FName Zombie, const FIntVector& Near)
{
	const FMadZombieDefinition* Definition = MadFall::GetGameplayDefinitions().FindZombie(Zombie);
	FIntVector Feet;
	if (Horde == nullptr || Definition == nullptr || !Horde->FindStandableNear(Near.X, Near.Y, Near.Z, Feet))
	{
		return false;
	}
	if (Horde->SpawnZombie(*Definition, Feet, /*bHorde*/ false) == nullptr)
	{
		return false;
	}
	++ZombiesSpawned;
	UE_LOG(LogMadFallGameplay, Log, TEXT("Script [%s] spawned %s at %s."), *ModId.ToString(), *Zombie.ToString(), *Feet.ToString());
	return true;
}

void UMadScriptSubsystem::GetTime(int32& OutDay, float& OutHour)
{
	OutDay = Clock ? Clock->GetDay() : 1;
	OutHour = Clock ? Clock->GetTimeOfDay() : 8.0f;
}

// --- status -----------------------------------------------------------------------------

FString UMadScriptSubsystem::DescribeStatus() const
{
	TArray<FMadScriptModStatus> Mods;
	if (Runtime.IsValid())
	{
		Runtime->GetStatus(Mods);
	}
	FString Out = FString::Printf(TEXT("Scripts: %d mod(s), %d/%d loaded, %lld event(s) dispatched, %lld dropped, %d queued, %lld block(s) set, %lld item(s) given, %lld zombie(s) spawned."),
		Mods.Num(), NextModToLoad, ModsToLoad.Num(), EventsDispatched, EventsDropped, Queue.Num(), BlocksSet, ItemsGiven, ZombiesSpawned);
	for (const FMadScriptModStatus& Mod : Mods)
	{
		Out += FString::Printf(TEXT("\n  %s: %d script(s), %d handler(s), %d command(s), %d error(s)%s, %.0f KB, worst call %.3f ms%s"),
			*Mod.Id.ToString(), Mod.ScriptsLoaded, Mod.Handlers, Mod.Commands, Mod.Errors, Mod.bDisabled ? TEXT(" DISABLED") : TEXT(""),
			Mod.MemoryBytes / 1024.0, Mod.WorstCallMs,
			Mod.LastError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", last error: %s"), *Mod.LastError.Left(200)));
	}
	return Out;
}

static FAutoConsoleCommandWithWorld GMadScriptsStatusCommand(
	TEXT("mad.scripts"),
	TEXT("Lists script mods: handlers, commands, errors, memory and their slowest call."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadScriptSubsystem* Scripts = World ? World->GetSubsystem<UMadScriptSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Scripts->DescribeStatus());
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadScriptsEventCommand(
	TEXT("mad.scripts.event"),
	TEXT("mad.scripts.event <name> [key=value ...]: queues a script event by hand. Numbers and true/false are typed."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadScriptSubsystem* Scripts = World ? World->GetSubsystem<UMadScriptSubsystem>() : nullptr;
		if (Scripts == nullptr || Args.Num() < 1)
		{
			UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.scripts.event <name> [key=value ...] (game worlds only)"));
			return;
		}
		FMadScriptEvent Event{ FName(*Args[0]) };
		for (int32 Index = 1; Index < Args.Num(); ++Index)
		{
			FString Key, Value;
			if (!Args[Index].Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			if (Value == TEXT("true") || Value == TEXT("false"))
			{
				Event.Add(Key, Value == TEXT("true"));
			}
			else if (Value.IsNumeric())
			{
				Event.Add(Key, FCString::Atod(*Value));
			}
			else
			{
				Event.Add(Key, Value);
			}
		}
		const bool bListened = Scripts->GetRuntime() && Scripts->GetRuntime()->HasHandlers(Event.Name);
		Scripts->QueueEvent(MoveTemp(Event));
		UE_LOG(LogMadFallGameplay, Display, TEXT("Scripts: queued %s (%s)."), *Args[0], bListened ? TEXT("has handlers") : TEXT("no handlers; dropped"));
	}));

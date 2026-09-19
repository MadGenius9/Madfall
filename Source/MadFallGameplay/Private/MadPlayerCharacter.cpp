// Copyright MadFall. All Rights Reserved.

#include "MadPlayerCharacter.h"

#include "MadPrefabRegistry.h"

#include "MadFrameBudget.h"
#include "AbilitySystemComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "MadAudioSubsystem.h"
#include "MadBlockDamage.h"
#include "MadBlockRegistry.h"
#include "MadChunkMeshSubsystem.h"
#include "MadChunkStreamingSubsystem.h"
#include "MadConsoleScript.h"
#include "MadContainerSubsystem.h"
#include "MadFallGameplay.h"
#include "MadGameplayDefinitions.h"
#include "MadGameplaySave.h"
#include "MadGameplaySaveSubsystem.h"
#include "MadDifficulty.h"
#include "MadHarvest.h"
#include "MadKeyBindings.h"
#include "MadLocalization.h"
#include "MadMenus.h"
#include "MadOrientation.h"
#include "MadPickupSubsystem.h"
#include "MadProjectile.h"
#include "MadScriptSubsystem.h"
#include "MadTrading.h"
#include "MadStructuralSubsystem.h"
#include "MadSurvivalAttributeSet.h"
#include "MadSurvivorComponents.h"
#include "MadSwim.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "MadViewModel.h"
#include "MadWorldMap.h"
#include "MadWorldGenerator.h"
#include "MadZombie.h"
#include "Misc/StringBuilder.h"

namespace
{
	TAutoConsoleVariable<bool> CVarSwimTrace(
		TEXT("mad.player.SwimTrace"), false,
		TEXT("Logs the swimming solver's numbers each frame."));

	constexpr float BareHandsUseSeconds = 0.5f;
	constexpr float BareHandsStamina = 1.0f;
	constexpr float SprintSpeed = 720.0f;
	constexpr float WalkSpeed = 420.0f;

	/** How long collision is given to cook after the spawn chunk's mesh settles. */
	constexpr float SpawnSettleSeconds = 0.5f;

	/** Past this, the player is released even if the world is not ready, with a warning. */
	constexpr float SpawnTimeoutSeconds = 30.0f;

	constexpr int32 StationRadius = 3;

	TAutoConsoleVariable<float> CVarCraftTimeScale(
		TEXT("mad.craft.TimeScale"),
		1.0f,
		TEXT("Multiplier on recipe craft_seconds. 0 makes crafting instant."),
		ECVF_Default);


}

AMadPlayerCharacter::AMadPlayerCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	// A 2-voxel gap is 200 cm; the capsule has to fit through one with room to spare.
	GetCapsuleComponent()->InitCapsuleSize(38.0f, 90.0f);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(GetCapsuleComponent());
	Camera->SetRelativeLocation(FVector(0.0, 0.0, 70.0));
	Camera->bUsePawnControlRotation = true;

	ViewModel = CreateDefaultSubobject<UMadViewModelComponent>(TEXT("ViewModel"));
	ViewModel->SetupAttachment(Camera);
	Camera->SetFieldOfView(90.0f);

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->MaxWalkSpeed = WalkSpeed;
	Movement->JumpZVelocity = 520.0f;
	Movement->AirControl = 0.35f;
	// Steps up one full voxel: walking up a block staircase should not need jumps.
	Movement->MaxStepHeight = 105.0f;
	Movement->SetWalkableFloorAngle(50.0f);

	AbilitySystem = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystem"));
	SurvivalAttributes = CreateDefaultSubobject<UMadSurvivalAttributeSet>(TEXT("SurvivalAttributes"));
	Survival = CreateDefaultSubobject<UMadSurvivalComponent>(TEXT("Survival"));
	Inventory = CreateDefaultSubobject<UMadInventoryComponent>(TEXT("Inventory"));

	Random.Initialize(0x5A17);
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void AMadPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	AbilitySystem->InitAbilityActorInfo(this, this);
	Survival->OnDied().AddUObject(this, &AMadPlayerCharacter::HandleDied);
	BindingsChangedHandle = MadFall::Input::OnBindingsChanged().AddUObject(this, &AMadPlayerCharacter::MapKeys);

	// Hold the player still until the ground under them exists. Start them high
	// over the generator's estimate of the surface so the streaming ring is
	// centred on the right chunk layers.
	GetCharacterMovement()->DisableMovement();
	SpawnColumn = FIntPoint(MadFall::WorldCmToVoxel(GetActorLocation()).X, MadFall::WorldCmToVoxel(GetActorLocation()).Y);

	if (const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>())
	{
		if (const FMadWorldGenerator* Generator = VoxelWorld->GetWorldGenerator())
		{
			const float Surface = Generator->GetSurfaceHeight(SpawnColumn.X + 0.5f, SpawnColumn.Y + 0.5f);
			TeleportToVoxel(FIntVector(SpawnColumn.X, SpawnColumn.Y, FMath::CeilToInt32(Surface) + 3));
		}
	}

	FMadPlayerSaveData Saved;
	UMadGameplaySaveSubsystem* Saves = GetWorld()->GetSubsystem<UMadGameplaySaveSubsystem>();
	if (Saves != nullptr && Saves->ConsumePlayerSave(Saved))
	{
		RestoreState(Saved);
		SetActorLocation(Saved.Location, false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Restored the survivor from the save: level %d, %d item stack(s)."),
			Level, Saved.Inventory.FilterByPredicate([](const FMadItemStack& S) { return !S.IsEmpty(); }).Num());
	}
	else
	{
		// A new survivor gets a way to make a first tool.
		Inventory->AddItem(FName(TEXT("madfall:wood_plank")), 6);
		Inventory->AddItem(FName(TEXT("madfall:rock")), 4);
		Inventory->AddItem(FName(TEXT("madfall:canned_food")), 2);
		Inventory->AddItem(FName(TEXT("madfall:water_bottle")), 2);
	}

	ApplyPerkStats();
	for (const FName& Started : Quests.Refresh(MadFall::GetGameplayDefinitions()))
	{
		UE_LOG(LogMadFallGameplay, Display, TEXT("Quest started: %s."), *Started.ToString());
	}
	PushMessage(TEXT("Waiting for the world to load..."), 5.0f);
}

bool AMadPlayerCharacter::ReceiveHit(float Amount, FName DamageType, AActor* Attacker)
{
	if (IsDown() || Amount <= 0.0f)
	{
		return false;
	}
	Survival->ApplyAttackDamage(Amount);
	// The edge of the screen reddens with the size of the hit, so a scratch and
	// a mauling do not look the same.
	HurtAtSeconds = GetWorld()->GetTimeSeconds();
	HurtAmount = FMath::Max(HurtAmount * 0.5f, FMath::Clamp(Amount / 25.0f, 0.15f, 1.0f));
	// Zombie acid is infectious, like a bite.
	if (Cast<AMadZombie>(Attacker) != nullptr)
	{
		Survival->ApplyEffects({ { FName(TEXT("infection")), 2.0f } });
	}
	return Survival->IsDead();
}

float AMadPlayerCharacter::GetRespawnCountdown() const
{
	if (RespawnAt < 0.0f || GetWorld() == nullptr)
	{
		return 0.0f;
	}
	return FMath::Max(0.0f, static_cast<float>(RespawnAt - GetWorld()->GetTimeSeconds()));
}

float AMadPlayerCharacter::GetHurtFlash() const
{
	if (HurtAmount <= 0.0f || GetWorld() == nullptr)
	{
		return 0.0f;
	}
	constexpr float HurtFadeSeconds = 0.5f;
	const float Age = static_cast<float>(GetWorld()->GetTimeSeconds() - HurtAtSeconds);
	return HurtAmount * FMath::Clamp(1.0f - Age / HurtFadeSeconds, 0.0f, 1.0f);
}

void AMadPlayerCharacter::NotifyQuest(EMadQuestObjectiveType Type, FName Id, const TArray<FName>& ThingTags, int32 Amount, const TOptional<FIntVector>& Where)
{
	// The same notifications drive script events: one list of "things the survivor did".
	if (UMadScriptSubsystem* Scripts = GetWorld()->GetSubsystem<UMadScriptSubsystem>())
	{
		Scripts->NotifyPlayerAction(Type, Id, Amount, Where);
	}
	HandleQuestsCompleted(Quests.Notify(Type, Id, ThingTags, Amount, MadFall::GetGameplayDefinitions()));
}

bool AMadPlayerCharacter::CompleteQuest(FName Quest)
{
	if (!Quests.ForceComplete(Quest, MadFall::GetGameplayDefinitions()))
	{
		return false;
	}
	HandleQuestsCompleted({ Quest });
	return true;
}

void AMadPlayerCharacter::HandleQuestsCompleted(const TArray<FName>& Done)
{
	if (Done.Num() == 0)
	{
		return;
	}
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	for (const FName& Id : Done)
	{
		const FMadQuestDefinition* Quest = Definitions.FindQuest(Id);
		if (Quest == nullptr)
		{
			continue;
		}
		TArray<FMadItemStack> Reward;
		for (const FMadItemAmount& Amount : Quest->RewardItems)
		{
			if (const FMadItemDefinition* Item = Definitions.FindItem(Amount.Item))
			{
				Reward.Add(FMadItemStack::Make(*Item, Amount.Count));
			}
		}
		GiveOrDrop(Reward, GetActorLocation());
		if (Quest->RewardExperience > 0)
		{
			AddExperience(Quest->RewardExperience);
		}
		PushMessage(FString::Printf(TEXT("Quest complete: %s"), *MadFall::Localize(Quest->DisplayName)), 4.0f);
		if (UMadScriptSubsystem* Scripts = GetWorld()->GetSubsystem<UMadScriptSubsystem>())
		{
			FMadScriptEvent Event(TEXT("quest_completed"));
			Event.Add(TEXT("quest"), Id.ToString());
			Scripts->QueueEvent(MoveTemp(Event));
		}
		UE_LOG(LogMadFallGameplay, Display, TEXT("Quest complete: %s."), *Id.ToString());
	}
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->Play2D(EMadSound::CraftDone, 1.0f);
	}
	for (const FName& Started : Quests.Refresh(Definitions))
	{
		if (const FMadQuestDefinition* Quest = Definitions.FindQuest(Started))
		{
			PushMessage(FString::Printf(TEXT("New quest: %s"), *MadFall::Localize(Quest->DisplayName)), 4.0f);
		}
		UE_LOG(LogMadFallGameplay, Display, TEXT("Quest started: %s."), *Started.ToString());
	}
}

void AMadPlayerCharacter::RefreshJobSite(float DeltaSeconds)
{
	// Looking at every cell in reach is 169 planning lookups, and the building
	// a job points at does not move. Once found it is re-checked every few
	// seconds - enough to notice a nearer one after a long walk - but the first
	// search runs at once, so taking a job puts a marker on the compass now
	// rather than in five seconds' time.
	JobSearchTimer -= DeltaSeconds;
	if (JobSite.IsSet() && JobSearchTimer > 0.0f)
	{
		return;
	}
	JobSearchTimer = 5.0f;

	JobSite.Reset();
	JobLabel.Reset();

	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	if (Generator == nullptr || !Generator->GetPoiPlanner().HasPrefabs())
	{
		return;
	}

	// The first unfinished clearing objective of the first quest that has one:
	// a survivor is walking to one building, and a compass with three job
	// arrows on it is a compass with none.
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadQuestObjective* Job = nullptr;
	for (const FMadQuestProgress& Progress : Quests.GetActive())
	{
		const FMadQuestDefinition* Quest = Definitions.FindQuest(Progress.Quest);
		if (Quest == nullptr)
		{
			continue;
		}
		for (int32 Index = 0; Index < Quest->Objectives.Num() && Job == nullptr; ++Index)
		{
			const FMadQuestObjective& Objective = Quest->Objectives[Index];
			const int32 Done = Progress.Counts.IsValidIndex(Index) ? Progress.Counts[Index] : 0;
			if (Objective.Type == EMadQuestObjectiveType::ClearPoi && Done < Objective.Count)
			{
				Job = &Objective;
			}
		}
		if (Job != nullptr)
		{
			break;
		}
	}

	if (Job == nullptr)
	{
		return;
	}

	const FMadPoiPlanner& Planner = Generator->GetPoiPlanner();
	const FMadPrefabRegistry& Prefabs = UMadVoxelWorldSubsystem::GetPrefabRegistry();
	const int32 CellSize = FMath::Max(1, Planner.GetCellSizeVoxels());
	const FIntVector Feet = GetFeetVoxel();
	const int32 CellX = FMath::FloorToInt32(static_cast<float>(Feet.X) / CellSize);
	const int32 CellY = FMath::FloorToInt32(static_cast<float>(Feet.Y) / CellSize);

	// Every cell within reach, keeping the nearest match.
	//
	// WHY not stop at the first ring that has one: a cell is 256 voxels across
	// and a POI sits anywhere inside its own cell, so ring order is not
	// distance order. Stopping early sent a survivor to a cabin 525 voxels off
	// while a nearer one stood at 344. Planning is cached by the generator and
	// this runs once a second, so looking at all of them costs map lookups.
	constexpr int32 Reach = 6;
	for (int32 DY = -Reach; DY <= Reach; ++DY)
	{
		{
			for (int32 DX = -Reach; DX <= Reach; ++DX)
			{
				FMadPoiInstance Poi;
				if (!Planner.PlanCell(*Generator, CellX + DX, CellY + DY, Poi) || Poi.PrefabIndex >= Prefabs.Num())
				{
					continue;
				}
				const FMadPrefab& Prefab = Prefabs.Get(Poi.PrefabIndex);
				if (!Job->Matches(Prefab.Id, Prefab.Tags))
				{
					continue;
				}

				const FIntVector Centre = Poi.Origin + FIntVector(Poi.RotatedSize.X / 2, Poi.RotatedSize.Y / 2, 0);
				const auto FlatDistanceSquared = [&Feet](const FIntVector& At)
				{
					const int64 DX64 = At.X - Feet.X;
					const int64 DY64 = At.Y - Feet.Y;
					return DX64 * DX64 + DY64 * DY64;
				};
				if (!JobSite.IsSet() || FlatDistanceSquared(Centre) < FlatDistanceSquared(JobSite.GetValue()))
				{
					JobSite = Centre;
					JobLabel = Prefab.DisplayName.IsEmpty() ? Prefab.Id.ToString() : MadFall::Localize(Prefab.DisplayName);
				}
			}
		}
	}
}

void AMadPlayerCharacter::TickQuests(float DeltaSeconds)
{
	QuestCheckTimer -= DeltaSeconds;
	if (QuestCheckTimer > 0.0f || bWaitingForWorld)
	{
		return;
	}
	QuestCheckTimer = 1.0f;
	RefreshJobSite(1.0f);

	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadInventory& Items = Inventory->GetInventory();
	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	HandleQuestsCompleted(Quests.Evaluate([&](const FMadQuestObjective& Objective)
	{
		if (Objective.Type == EMadQuestObjectiveType::ReachDay)
		{
			// Count is the day to reach, so progress is the day itself.
			return Clock ? Clock->GetDay() : 1;
		}
		int32 Held = 0;
		for (int32 Slot = 0; Slot < Items.NumSlots(); ++Slot)
		{
			const FMadItemStack& Stack = Items.GetSlot(Slot);
			if (Stack.IsEmpty())
			{
				continue;
			}
			const FMadItemDefinition* Item = Definitions.FindItem(Stack.Item);
			if (Objective.Matches(Stack.Item, Item ? Item->Tags : TArray<FName>()))
			{
				Held += Stack.Count;
			}
		}
		return Held;
	}, Definitions));
}

void AMadPlayerCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	MadFall::Input::OnBindingsChanged().Remove(BindingsChangedHandle);

	// Leaving play for any reason other than being destroyed in-world (quit,
	// map change, PIE stop) is when progress would otherwise be lost.
	if (EndPlayReason != EEndPlayReason::Destroyed)
	{
		if (UMadGameplaySaveSubsystem* Saves = GetWorld() ? GetWorld()->GetSubsystem<UMadGameplaySaveSubsystem>() : nullptr)
		{
			Saves->SaveNow(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void AMadPlayerCharacter::SaveState(FMadPlayerSaveData& Out) const
{
	Out.Location = GetActorLocation();
	Out.ViewRotation = GetControlRotation();
	Out.SpawnColumn = SpawnColumn;
	Out.bHasBed = BedVoxel.IsSet();
	Out.BedVoxel = BedVoxel.Get(FIntVector::ZeroValue);
	Out.Stats = Survival->GetStats();
	Out.Level = Level;
	Out.Experience = Experience;
	Out.PerkRanks = PerkRanks;
	// Crafts in progress are saved as their refunded ingredients: a save never
	// holds items that exist nowhere.
	FMadInventory WithRefunds = Inventory->GetInventory();
	TArray<FMadItemStack> Refund;
	GetCraftRefund(Refund);
	for (const FMadItemStack& Stack : Refund)
	{
		WithRefunds.Add(Stack, MadFall::GetGameplayDefinitions());
	}
	Out.Inventory = WithRefunds.GetSlots();
	Out.Worn = Inventory->GetWorn().GetSlots();
	Quests.Export(Out.CompletedQuests, Out.ActiveQuests);
	Out.SelectedSlot = Inventory->GetSelectedSlot();
}

void AMadPlayerCharacter::RestoreState(const FMadPlayerSaveData& In)
{
	SavedLocation = In.bHasLocation ? TOptional<FVector>(In.Location) : TOptional<FVector>();
	SavedView = In.ViewRotation;
	SpawnColumn = In.SpawnColumn;
	BedVoxel = In.bHasBed ? TOptional<FIntVector>(In.BedVoxel) : TOptional<FIntVector>();
	Level = FMath::Max(1, In.Level);
	Experience = FMath::Max(0, In.Experience);
	PerkRanks = In.PerkRanks;
	Survival->SetStats(In.Stats);

	// Maxima are recomputed from the restored ranks rather than trusted from
	// the save, so a perk rebalanced or removed by a mod since takes effect.
	ApplyPerkStats();

	FMadInventory& Items = Inventory->GetInventory();
	for (int32 Slot = 0; Slot < Items.NumSlots(); ++Slot)
	{
		// Unknown item ids are restored verbatim: removing a mod must not empty a backpack.
		Items.SetSlot(Slot, In.Inventory.IsValidIndex(Slot) ? In.Inventory[Slot] : FMadItemStack());
	}
	FMadInventory& Worn = Inventory->GetWorn();
	for (int32 Slot = 0; Slot < Worn.NumSlots(); ++Slot)
	{
		Worn.SetSlot(Slot, In.Worn.IsValidIndex(Slot) ? In.Worn[Slot] : FMadItemStack());
	}
	Survival->SetWear(Inventory->GetWearTotals());
	Inventory->SelectSlot(In.SelectedSlot);
	Quests.Import(In.CompletedQuests, In.ActiveQuests, MadFall::GetGameplayDefinitions());
}

void AMadPlayerCharacter::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	EnsureInput();
	if (const APlayerController* PlayerController = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			InputSubsystem->AddMappingContext(MappingContext, 0);
		}
	}
}

void AMadPlayerCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	static const IConsoleVariable* FieldOfView = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.view.FieldOfView"));
	if (FieldOfView && Camera && !FMath::IsNearlyEqual(Camera->FieldOfView, FieldOfView->GetFloat()))
	{
		Camera->SetFieldOfView(FieldOfView->GetFloat());
	}

	const double Now = GetWorld()->GetTimeSeconds();
	Messages.RemoveAll([Now](const FMessage& Message) { return Message.ExpiresAt < Now; });

	if (bWaitingForWorld)
	{
		MAD_FRAME_SCOPE(Player);
		TickSpawn(DeltaSeconds);
		return;
	}

	MadFall::ConsoleScript::Tick(GetWorld(), DeltaSeconds);

	// After the script runner: console-driven tests would otherwise charge their commands to the player.
	MAD_FRAME_SCOPE(Player);
	TickCrafting(DeltaSeconds);
	TickQuests(DeltaSeconds);

	if (RespawnAt >= 0.0f)
	{
		if (Now >= RespawnAt)
		{
			Respawn();
		}
		return;
	}

	if (TickTerrainHold(DeltaSeconds))
	{
		return;
	}

	if (ScriptedWalkSeconds > 0.0f)
	{
		ScriptedWalkSeconds -= DeltaSeconds;
		AddMovementInput(ScriptedWalkDirection, 1.0f);
		ClimbInput = 1.0f;
		DiveInput = ScriptedDive;
	}
	TickSwimming(DeltaSeconds);
	TickClimbing();
	TickSounds(DeltaSeconds);

	const FMadItemStack* HeldStack = Inventory->GetSelectedStack();
	ViewModel->SetHeldItem(HeldStack && HeldStack->Count > 0 ? HeldStack->Item : NAME_None);
	// Screens over the view hide the hand, like a real first-person game.
	ViewModel->SetVisibility(!bInventoryOpen, /*bPropagateToChildren*/ true);
	UpdateTarget();

	TickInventoryScreen();

	if (bPrimaryHeld && !bInventoryOpen)
	{
		UsePrimary();
	}
}

// ===========================================================================
// Spawning
// ===========================================================================

int32 AMadPlayerCharacter::FindStandableZ(int32 X, int32 Y) const
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return INDEX_NONE;
	}

	const int32 StartZ = MadFall::WorldCmToVoxel(GetActorLocation()).Z + 40;
	for (int32 Z = FMath::Min(StartZ, MadFall::WorldMaxZ - 2); Z > MadFall::WorldMinZ; --Z)
	{
		if (!VoxelWorld->IsVoxelLoaded(X, Y, Z))
		{
			continue;
		}
		const FMadVoxel Ground = VoxelWorld->GetVoxel(X, Y, Z);
		if (Ground.IsSolid() && !Ground.HasFlag(EMadVoxelFlags::Liquid)
			&& !VoxelWorld->GetVoxel(X, Y, Z + 1).IsSolid() && !VoxelWorld->GetVoxel(X, Y, Z + 2).IsSolid())
		{
			return Z + 1;
		}
	}
	return INDEX_NONE;
}

void AMadPlayerCharacter::TickClimbing()
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	const float Input = ClimbInput;
	ClimbInput = 0.0f;
	if (VoxelWorld == nullptr || Movement == nullptr)
	{
		return;
	}

	auto IsClimbable = [VoxelWorld](const FIntVector& V)
	{
		const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(VoxelWorld->GetVoxel(V.X, V.Y, V.Z).BlockTypeID);
		return Def != nullptr && Def->bClimbable;
	};
	const FIntVector Feet = GetFeetVoxel();
	const bool bOnLadder = !IsDown() && (IsClimbable(Feet) || IsClimbable(Feet + FIntVector(0, 0, 1)));

	if (bOnLadder)
	{
		// Flying, so gravity stops; forward climbs, back descends, nothing holds on.
		if (Movement->MovementMode != MOVE_Flying)
		{
			Movement->SetMovementMode(MOVE_Flying);
		}
		Movement->Velocity.Z = Input * 280.0f;
		bClimbing = true;
	}
	else if (bClimbing)
	{
		bClimbing = false;
		if (Movement->MovementMode == MOVE_Flying)
		{
			// A little lift off the top rung so the survivor steps onto the floor above.
			Movement->Velocity.Z = FMath::Max(Movement->Velocity.Z, 0.0f) + (Input > 0.0f ? 200.0f : 0.0f);
			Movement->SetMovementMode(MOVE_Falling);
		}
	}
}

float AMadPlayerCharacter::MeasureSubmersion(float& OutWaterTopZ, float& OutWaterBottomZ) const
{
	OutWaterTopZ = -FLT_MAX;
	OutWaterBottomZ = -FLT_MAX;
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld() ? GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (VoxelWorld == nullptr || Capsule == nullptr)
	{
		return 0.0f;
	}

	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const float FeetZ = static_cast<float>(GetActorLocation().Z) - HalfHeight;
	const float HeadZ = FeetZ + 2.0f * HalfHeight;

	const FIntVector Feet = GetFeetVoxel();
	auto IsLiquid = [VoxelWorld](const FIntVector& V)
	{
		const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(
			VoxelWorld->GetVoxel(V.X, V.Y, V.Z).BlockTypeID);
		return Def != nullptr && Def->bLiquid;
	};

	// Up from the first water at or above the feet: the top is the last liquid
	// voxel of that unbroken column, so a survivor in a flooded cellar floats to
	// its ceiling rather than to the lake's surface two chunks away.
	//
	// Starting at the feet is not enough: someone standing on the seabed has
	// sand under their feet and ten voxels of water over their head, and a scan
	// from the feet stopped at once and called that dry land.
	// The lowest water anywhere the body spans, not just at the feet: standing
	// on the seabed puts the feet in sand, and a capsule pressed into the sand
	// puts the feet voxel below that again, which read as dry land one frame
	// and ten voxels of water the next.
	const int32 HeadVoxel = FMath::FloorToInt32(HeadZ / MadFall::VoxelSizeUU);
	int32 Start = MAX_int32;
	for (int32 Z = Feet.Z; Z <= HeadVoxel; ++Z)
	{
		if (IsLiquid(FIntVector(Feet.X, Feet.Y, Z)))
		{
			Start = Z;
			break;
		}
	}
	if (Start == MAX_int32)
	{
		return 0.0f;
	}
	int32 Top = Start;
	for (int32 Z = Start; Z < Start + 32; ++Z)
	{
		if (!IsLiquid(FIntVector(Feet.X, Feet.Y, Z)))
		{
			break;
		}
		Top = Z;
	}
	OutWaterTopZ = static_cast<float>(Top + 1) * MadFall::VoxelSizeUU;
	OutWaterBottomZ = static_cast<float>(Start) * MadFall::VoxelSizeUU;
	return MadFall::Swim::SubmergedFraction(FeetZ, HeadZ, OutWaterTopZ);
}

void AMadPlayerCharacter::TickSwimming(float DeltaSeconds)
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (Movement == nullptr)
	{
		return;
	}

	const float Dive = DiveInput;
	DiveInput = 0.0f;

	float WaterTopZ = 0.0f;
	float WaterBottomZ = 0.0f;
	const float Fraction = MeasureSubmersion(WaterTopZ, WaterBottomZ);
	const FMadSwimTuning Tuning;
	const float EyeZ = static_cast<float>(GetActorLocation().Z) + BaseEyeHeight;
	const bool bSprint = Survival->IsSprinting();
	const FMadSwimState State = MadFall::Swim::Evaluate(Fraction, EyeZ, WaterTopZ, Dive, bSwimming, bSprint, Tuning);

	Survival->SetSubmersion(Fraction, State.bSwimming && State.bHeadUnder);

	// Under the surface the world goes blue and the edges close in. No asset and
	// no depth fade - a post-process material would do it properly - but without
	// something the only sign a survivor is underwater is the breath bar, and
	// the water's own surface seen from beneath is a dark plane that reads as a
	// bug rather than as a ceiling of water.
	if (Camera != nullptr)
	{
		const bool bEyesUnder = State.bSwimming && State.bHeadUnder;
		FPostProcessSettings& Post = Camera->PostProcessSettings;
		Post.bOverride_ColorGain = bEyesUnder;
		Post.bOverride_VignetteIntensity = bEyesUnder;
		Post.bOverride_SceneFringeIntensity = bEyesUnder;
		if (bEyesUnder)
		{
			Post.ColorGain = FVector4(0.34, 0.62, 0.95, 1.0);
			Post.VignetteIntensity = 0.85f;
			Post.SceneFringeIntensity = 2.0f;
		}
	}

#if !UE_BUILD_SHIPPING
	static const IConsoleVariable* Trace = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.player.SwimTrace"));
	if (Trace && Trace->GetBool())
	{
		UE_LOG(LogMadFallGameplay, Display, TEXT("MADSWIM frac=%.2f eye=%.0f top=%.0f dive=%.2f buoy=%.0f vz=%.0f mode=%d"),
			Fraction, EyeZ, WaterTopZ, Dive, State.BuoyancyVelocity, Movement->Velocity.Z, static_cast<int32>(Movement->MovementMode));
	}
#endif

	if (State.bSwimming)
	{
		if (!bSwimming)
		{
			// Entering: the fall stops in the water rather than in the mud at
			// the bottom of it, which is also why a dive does no fall damage.
			Movement->Velocity.Z = FMath::Max(Movement->Velocity.Z, -200.0f);
			if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
			{
				Audio->PlayForMaterial(EMadSound::Step, FName(TEXT("madfall:water")), GetActorLocation());
			}
			PushMessage(TEXT("Swimming - watch your breath."));
		}
		bSwimming = true;
		bClimbing = false;
		ClimbInput = 0.0f;

		// Flying, like the ladder: gravity is the water's job here, not the
		// movement component's.
		if (Movement->MovementMode != MOVE_Flying)
		{
			Movement->SetMovementMode(MOVE_Flying);
		}
		Movement->MaxFlySpeed = WalkSpeed * State.SpeedMultiplier;
		Movement->BrakingDecelerationFlying = WalkSpeed * Tuning.Drag;
		// Set, not eased: flying braking is stronger than any ramp toward the
		// target would be, so an interpolated velocity was cancelled every frame
		// and the survivor hung motionless at whatever depth they entered.
		// Buoyancy is already a speed with the spring inside it.
		float Vertical = State.BuoyancyVelocity;

		// A swimmer stops at the bed rather than swimming into it. Terrain
		// collision alone let a hard dive push the capsule a metre into the
		// sand, where the body was no longer in water at all: the survivor read
		// as standing on dry land at the bottom of the sea, and their breath
		// came back while they were under it.
		const float FeetZ = static_cast<float>(GetActorLocation().Z) - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (Vertical < 0.0f && FeetZ <= WaterBottomZ + 4.0f)
		{
			Vertical = 0.0f;
		}
		Movement->Velocity.Z = Vertical;
	}
	else if (bSwimming)
	{
		bSwimming = false;
		if (Movement->MovementMode == MOVE_Flying)
		{
			// A push out of the water, so climbing out onto the bank works.
			Movement->Velocity.Z = FMath::Max(static_cast<float>(Movement->Velocity.Z), 120.0f);
			Movement->SetMovementMode(MOVE_Falling);
		}
		Movement->MaxWalkSpeed = bSprint ? SprintSpeed : WalkSpeed;
	}
	else if (Fraction > 0.0f)
	{
		// Wading: slower, but still walking.
		Movement->MaxWalkSpeed = (bSprint ? SprintSpeed : WalkSpeed) * State.SpeedMultiplier;
	}
}

void AMadPlayerCharacter::TickSounds(float DeltaSeconds)
{
	UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>();
	if (Audio == nullptr)
	{
		return;
	}

	// Footsteps: one per stride on the ground, voiced by what is underfoot.
	const float Speed = static_cast<float>(GetVelocity().Size2D());
	if (GetCharacterMovement()->IsMovingOnGround() && Speed > 50.0f)
	{
		StepDistance += Speed * DeltaSeconds;
		const float Stride = Speed > 450.0f ? 210.0f : 165.0f;
		if (StepDistance >= Stride)
		{
			StepDistance = 0.0f;
			const FIntVector Below = GetFeetVoxel() - FIntVector(0, 0, 1);
			if (const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>())
			{
				const FMadVoxel Ground = VoxelWorld->GetVoxel(Below.X, Below.Y, Below.Z);
				const FMadBlockDefView View = UMadVoxelWorldSubsystem::GetBlockRegistry().GetBlockView(Ground.BlockTypeID);
				const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				Audio->PlayForMaterial(EMadSound::Step, View.MaterialClass, GetActorLocation() - FVector(0.0, 0.0, HalfHeight), 0.45f);
			}
		}
	}
	else
	{
		StepDistance = 120.0f;   // the first step after stopping comes quickly
	}

	// Any drop in health - a zombie, falling debris, starvation - is heard.
	const float Health = Survival->GetStats().Health;
	HurtCooldown -= DeltaSeconds;
	if (Health < LastHealth - 1.0f && HurtCooldown <= 0.0f)
	{
		Audio->Play2D(EMadSound::PlayerHurt, 0.8f);
		HurtCooldown = 0.6f;
	}
	LastHealth = Health;
}

void AMadPlayerCharacter::TickSpawn(float DeltaSeconds)
{
	SpawnWait += DeltaSeconds;

	const UWorld* World = GetWorld();
	const UMadChunkStreamingSubsystem* Streaming = World->GetSubsystem<UMadChunkStreamingSubsystem>();
	const UMadChunkMeshSubsystem* Meshes = World->GetSubsystem<UMadChunkMeshSubsystem>();

	bool bReady = Streaming != nullptr && Streaming->IsAreaLoaded(GetActorLocation(), 1);
	if (bReady && Meshes != nullptr)
	{
		const FIntVector Voxel = MadFall::WorldCmToVoxel(GetActorLocation());
		const FMadChunkCoord Centre = MadFall::WorldToChunk(Voxel.X, Voxel.Y, Voxel.Z);
		for (int32 DZ = -1; DZ <= 1 && bReady; ++DZ)
		{
			for (int32 DY = -1; DY <= 1 && bReady; ++DY)
			{
				for (int32 DX = -1; DX <= 1 && bReady; ++DX)
				{
					const FMadChunkCoord Coord(Centre.X + DX, Centre.Y + DY, Centre.Z + DZ);
					bReady = !Coord.IsValidZ() || Meshes->IsChunkMeshSettled(Coord);
				}
			}
		}
	}

	if (bReady)
	{
		if (SavedLocation.IsSet())
		{
			// Exactly where they saved; the ground they stood on was saved with them.
			SetActorLocation(*SavedLocation, false, nullptr, ETeleportType::TeleportPhysics);
			SettleTime += DeltaSeconds;
		}
		else if (const int32 StandZ = FindStandableZ(SpawnColumn.X, SpawnColumn.Y); StandZ != INDEX_NONE)
		{
			TeleportToVoxel(FIntVector(SpawnColumn.X, SpawnColumn.Y, StandZ));
			SettleTime += DeltaSeconds;
		}
	}

	const bool bTimedOut = SpawnWait > SpawnTimeoutSeconds;
	if (SettleTime >= SpawnSettleSeconds || bTimedOut)
	{
		bWaitingForWorld = false;
		GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		if (SavedLocation.IsSet() && Controller != nullptr)
		{
			Controller->SetControlRotation(SavedView);
		}
		SavedLocation.Reset();
		if (bTimedOut)
		{
			UE_LOG(LogMadFallGameplay, Warning, TEXT("Spawn timed out after %.0f s waiting for the world; releasing the player anyway."), SpawnWait);
			PushMessage(TEXT("World loading is slow - watch your step."));
		}
		else
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("Player spawned at %s after %.2f s."), *GetFeetVoxel().ToString(), SpawnWait);
			PushMessage(TEXT("Day 1. Find shelter, find food, and get ready for the seventh night."), 6.0f);
		}
	}
}

bool AMadPlayerCharacter::TickTerrainHold(float DeltaSeconds)
{
	// Spawning and travel wait for the chunks around the survivor; anything else
	// that puts them ahead of streaming - a raw teleport, a fall out of a very
	// tall build - let them drop through chunks with no collision yet. Measured:
	// teleported 70 m up into unloaded ground, the survivor came to rest 43 m
	// under it. While the chunk at their feet, or the one just below, is not
	// loaded or has never been meshed, a falling survivor hangs where they are.
	// A chunk being REbuilt does not count: it keeps its old mesh and collision
	// until the new one lands, and holding on it froze the survivor mid-jump
	// every time they dug or built in their own chunk.
	UCharacterMovementComponent* Move = GetCharacterMovement();
	const UWorld* World = GetWorld();
	const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	const UMadChunkMeshSubsystem* Meshes = World ? World->GetSubsystem<UMadChunkMeshSubsystem>() : nullptr;
	if (Move == nullptr || VoxelWorld == nullptr || Meshes == nullptr || (!bTerrainHold && Move->MovementMode != MOVE_Falling))
	{
		return false;
	}

	const FIntVector Feet = GetFeetVoxel();
	auto Ready = [VoxelWorld, Meshes](const FIntVector& Voxel)
	{
		const FMadChunkCoord Coord = MadFall::WorldToChunk(Voxel.X, Voxel.Y, Voxel.Z);
		return !Coord.IsValidZ()
			|| (VoxelWorld->IsVoxelLoaded(Voxel.X, Voxel.Y, Voxel.Z) && (Meshes->HasChunkMesh(Coord) || Meshes->IsChunkMeshSettled(Coord)));
	};
	const bool bReady = Ready(Feet) && Ready(Feet - FIntVector(0, 0, 2));

	if (!bReady)
	{
		if (!bTerrainHold)
		{
			bTerrainHold = true;
			Move->DisableMovement();
			UE_LOG(LogMadFallGameplay, Display, TEXT("Holding the survivor at %s until the ground below loads."), *Feet.ToString());
		}
		Move->Velocity = FVector::ZeroVector;
		TerrainHoldSettle = 0.0f;
		return true;
	}
	if (!bTerrainHold)
	{
		return false;
	}
	// Collision cooks a few frames after the mesh settles.
	TerrainHoldSettle += DeltaSeconds;
	if (TerrainHoldSettle < 0.25f)
	{
		return true;
	}
	bTerrainHold = false;
	Move->SetMovementMode(MOVE_Falling);
	return false;
}

AMadPlayerCharacter* MadFall::FindLocalPlayer(const UWorld* World)
{
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	return Controller ? Cast<AMadPlayerCharacter>(Controller->GetPawn()) : nullptr;
}

FIntVector AMadPlayerCharacter::GetFeetVoxel() const
{
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 5.0);
	return MadFall::WorldCmToVoxel(Feet);
}

void AMadPlayerCharacter::TeleportToVoxel(const FIntVector& Voxel)
{
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Location(
		(Voxel.X + 0.5) * MadFall::VoxelSizeUU,
		(Voxel.Y + 0.5) * MadFall::VoxelSizeUU,
		Voxel.Z * MadFall::VoxelSizeUU + HalfHeight + 2.0);
	SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	GetCharacterMovement()->Velocity = FVector::ZeroVector;
}

void AMadPlayerCharacter::TravelToVoxel(const FIntVector& Voxel)
{
	TeleportToVoxel(Voxel);
	SavedLocation = GetActorLocation();
	SavedView = GetControlRotation();
	GetCharacterMovement()->DisableMovement();
	bWaitingForWorld = true;
	SpawnWait = 0.0f;
	SettleTime = 0.0f;
}

void AMadPlayerCharacter::HandleDied(const FMadSurvivalStepResult& Causes)
{
	const FString Cause = MadFall::Survival::WorstCause(Causes);
	DeathCause = Cause;
	DiedAtSeconds = GetWorld()->GetTimeSeconds();
	DeathPlace = GetFeetVoxel();

	UE_LOG(LogMadFallGameplay, Display, TEXT("Player died of %s."), *Cause);
	PushMessage(FString::Printf(TEXT("You died of %s."), *Cause), 4.0f);

	GetCharacterMovement()->DisableMovement();
	bPrimaryHeld = false;
	CloseInventory();
	RespawnAt = GetWorld()->GetTimeSeconds() + 4.0f;

	// The backpack stays where the survivor fell, 7 Days to Die style: dying
	// costs the walk back, and on a horde night that walk is the real penalty.
	// Queued crafts are refunded into it rather than lost.
	CancelCrafting();
	if (UMadPickupSubsystem* Pickups = GetWorld()->GetSubsystem<UMadPickupSubsystem>())
	{
		const TArray<FMadItemStack> Everything = Inventory->GetInventory().GetSlots();
		if (Pickups->Drop(GetActorLocation(), Everything, /*bBackpack*/ true) != nullptr)
		{
			FMadInventory& Items = Inventory->GetInventory();
			for (int32 Slot = 0; Slot < Items.NumSlots(); ++Slot)
			{
				Items.SetSlot(Slot, FMadItemStack());
			}
			Inventory->NotifyChanged();
			UE_LOG(LogMadFallGameplay, Display, TEXT("Dropped a backpack at %s."), *GetFeetVoxel().ToString());
		}
	}
}

void AMadPlayerCharacter::GiveOrDrop(const TArray<FMadItemStack>& Stacks, const FVector& DropLocation)
{
	TArray<FMadItemStack> Overflow;
	for (const FMadItemStack& Stack : Stacks)
	{
		const int32 Left = Inventory->AddStack(Stack);
		if (Left > 0)
		{
			FMadItemStack Rest = Stack;
			Rest.Count = Left;
			Overflow.Add(Rest);
		}
	}

	if (Overflow.Num() > 0)
	{
		if (UMadPickupSubsystem* Pickups = GetWorld()->GetSubsystem<UMadPickupSubsystem>())
		{
			Pickups->Drop(DropLocation, Overflow);
		}
		PushMessage(TEXT("Inventory full - dropped on the ground."), 2.0f);
	}
}

bool AMadPlayerCharacter::DropSelected()
{
	const FMadItemStack* Held = Inventory->GetSelectedStack();
	UMadPickupSubsystem* Pickups = GetWorld()->GetSubsystem<UMadPickupSubsystem>();
	if (Held == nullptr || Pickups == nullptr)
	{
		return false;
	}

	// Two metres in front, so it is not instantly picked back up.
	const FVector Forward = GetControlRotation().Vector().GetSafeNormal2D();
	if (Pickups->Drop(GetActorLocation() + Forward * 250.0, { *Held }) == nullptr)
	{
		return false;
	}
	Inventory->SetSelectedStack(FMadItemStack());
	return true;
}

EMadItemActionResult AMadPlayerCharacter::RepairSelected()
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const EMadItemActionResult Result = MadFall::Items::Repair(Inventory->GetInventory(), Inventory->GetSelectedSlot(), Definitions);
	Inventory->NotifyChanged();
	PushMessage(Result == EMadItemActionResult::Ok ? FString(TEXT("Repaired."))
		: FString::Printf(TEXT("Cannot repair: %s."), MadFall::Items::ToString(Result)), 1.5f);
	return Result;
}

EMadItemActionResult AMadPlayerCharacter::InstallModOnSelected(FName ModItem)
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const EMadItemActionResult Result = MadFall::Items::InstallMod(Inventory->GetInventory(), Inventory->GetSelectedSlot(), ModItem, Definitions);
	Inventory->NotifyChanged();
	PushMessage(Result == EMadItemActionResult::Ok ? FString::Printf(TEXT("Installed %s."), *MadFall::GetGameplayDefinitions().GetItemName(ModItem))
		: FString::Printf(TEXT("Cannot install: %s."), MadFall::Items::ToString(Result)), 1.5f);
	return Result;
}

void AMadPlayerCharacter::Respawn()
{
	RespawnAt = -1.0f;

	Survival->SetStats(FMadSurvivalStats());

	// At the bed, if it is still there - broken or collapsed, it no longer counts.
	bool bAtBed = false;
	if (BedVoxel.IsSet())
	{
		const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
		const FIntVector Bed = *BedVoxel;
		const FMadBlockDefinitionData* Def = VoxelWorld
			? UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(VoxelWorld->GetVoxel(Bed.X, Bed.Y, Bed.Z).BlockTypeID) : nullptr;
		if (Def != nullptr && Def->bSpawnPoint)
		{
			TeleportToVoxel(Bed + FIntVector(0, 0, 1));
			bAtBed = true;
		}
		else
		{
			BedVoxel.Reset();
		}
	}
	if (!bAtBed)
	{
		const int32 StandZ = FindStandableZ(SpawnColumn.X, SpawnColumn.Y);
		if (StandZ != INDEX_NONE)
		{
			TeleportToVoxel(FIntVector(SpawnColumn.X, SpawnColumn.Y, StandZ));
		}
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Player respawned at %s%s."), *GetFeetVoxel().ToString(), bAtBed ? TEXT(" (bed)") : TEXT(""));
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	PushMessage(bAtBed ? TEXT("You wake up in your bed.") : TEXT("You wake up where you started."));
}

// ===========================================================================
// Targeting and verbs
// ===========================================================================

float AMadPlayerCharacter::GetReach() const
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	if (const FMadItemStack* Held = Inventory->GetSelectedStack())
	{
		if (const FMadItemDefinition* Item = Definitions.FindItem(Held->Item); Item && Item->bHasTool)
		{
			return Item->Tool.Range * MadFall::Harvest::GetModMultiplier(*Held, Definitions, FName(TEXT("range")));
		}
	}
	return 4.0f;
}

void AMadPlayerCharacter::UpdateTarget()
{
	bHasTarget = false;

	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr || Controller == nullptr)
	{
		return;
	}

	// The camera component and control rotation, not the camera manager's view
	// point: that is cached once per frame, so a target computed right after
	// the control rotation changes would still be last frame's.
	const FVector EyeLocation = Camera->GetComponentLocation();
	const FRotator EyeRotation = GetControlRotation();

	auto IsTargetable = [VoxelWorld](const FIntVector& V)
	{
		const FMadVoxel Voxel = VoxelWorld->GetVoxel(V.X, V.Y, V.Z);
		return Voxel.IsSolid() && !Voxel.HasFlag(EMadVoxelFlags::Liquid);
	};

	bHasTarget = MadFall::VoxelRaycast(EyeLocation / MadFall::VoxelSizeUU, EyeRotation.Vector(), GetReach(), IsTargetable, Target);

	// A trader in reach and nearer than the block behind them.
	AimedTrader.Reset();
	const double Reach = FMath::Min(4.0f, GetReach()) * MadFall::VoxelSizeUU;
	const double BlockDistance = bHasTarget ? FVector::Dist(EyeLocation, (FVector(Target.Voxel) + FVector(0.5)) * MadFall::VoxelSizeUU) : Reach;
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MadTraderAim), false, this);
	if (GetWorld()->LineTraceSingleByChannel(Hit, EyeLocation, EyeLocation + EyeRotation.Vector() * Reach, ECC_Pawn, Params)
		&& Hit.Distance <= BlockDistance + 0.5 * MadFall::VoxelSizeUU)
	{
		AimedTrader = Cast<AMadTrader>(Hit.GetActor());
	}
}

void AMadPlayerCharacter::AimAtVoxel(const FIntVector& Voxel)
{
	AimAtLocation((FVector(Voxel) + FVector(0.5)) * MadFall::VoxelSizeUU);
}

void AMadPlayerCharacter::AimAtLocation(const FVector& Location)
{
	if (Controller == nullptr)
	{
		return;
	}
	// From the camera, which is what swings and targeting trace from.
	Controller->SetControlRotation((Location - Camera->GetComponentLocation()).Rotation());
	UpdateTarget();
}

bool AMadPlayerCharacter::UsePrimary(bool bIgnoreCooldown)
{
	UWorld* World = GetWorld();
	const double Now = World->GetTimeSeconds();
	if (!bIgnoreCooldown && Now < NextUseTime)
	{
		return false;
	}
	if (RespawnAt >= 0.0f)
	{
		return false;
	}

	UMadVoxelWorldSubsystem* VoxelWorld = World->GetSubsystem<UMadVoxelWorldSubsystem>();
	UMadStructuralSubsystem* Structural = World->GetSubsystem<UMadStructuralSubsystem>();
	if (VoxelWorld == nullptr || Structural == nullptr)
	{
		return false;
	}

	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();

	if (const FMadItemStack* RangedStack = Inventory->GetSelectedStack())
	{
		const FMadItemDefinition* RangedItem = Definitions.FindItem(RangedStack->Item);
		if (RangedItem != nullptr && RangedItem->bHasTool && RangedItem->Tool.IsRanged())
		{
			return FireRanged(*RangedItem, Now);
		}
	}

	// A zombie or animal in front of the crosshair takes the swing if it is nearer
	// than the targeted block. Actors are traced against physics; voxels are not.
	IMadDamageable* Victim = nullptr;
	AActor* VictimActor = nullptr;
	float VictimDistance = TNumericLimits<float>::Max();
	{
		const FVector Eye = Camera->GetComponentLocation();
		const FVector End = Eye + GetControlRotation().Vector() * GetReach() * MadFall::VoxelSizeUU;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(MadPlayerSwing), false, this);
		FHitResult TraceHit;
		if (World->LineTraceSingleByChannel(TraceHit, Eye, End, ECC_Pawn, Params))
		{
			VictimActor = TraceHit.GetActor();
			Victim = Cast<IMadDamageable>(VictimActor);
			VictimDistance = TraceHit.Distance / MadFall::VoxelSizeUU;
			if (Victim != nullptr && Victim->IsDead())
			{
				Victim = nullptr;
			}
		}
		UE_LOG(LogMadFallGameplay, Verbose, TEXT("Swing trace: %s at %.2f voxels; block target %s at %.2f."),
			VictimActor ? *VictimActor->GetName() : TEXT("nothing"), VictimDistance,
			bHasTarget ? *Target.Voxel.ToString() : TEXT("none"), bHasTarget ? Target.Distance : -1.0f);
	}
	const bool bHitZombie = Victim != nullptr && (!bHasTarget || VictimDistance < Target.Distance);

	const FMadBlockDefinitionData* Block = nullptr;
	if (!bHitZombie)
	{
		if (!bHasTarget)
		{
			return false;
		}
		const FMadVoxel Voxel = VoxelWorld->GetVoxel(Target.Voxel.X, Target.Voxel.Y, Target.Voxel.Z);
		Block = Blocks.FindDefinition(Voxel.BlockTypeID);
		if (Block == nullptr)
		{
			return false;
		}
	}

	const FMadItemStack* HeldPtr = Inventory->GetSelectedStack();
	FMadItemStack Held = HeldPtr ? *HeldPtr : FMadItemStack();
	const FMadItemDefinition* HeldItem = HeldPtr ? Definitions.FindItem(Held.Item) : nullptr;
	const bool bTool = HeldItem != nullptr && HeldItem->bHasTool;

	const float UseSeconds = bTool
		? HeldItem->Tool.UseSeconds * MadFall::Harvest::GetModMultiplier(Held, Definitions, FName(TEXT("use_seconds")))
		: BareHandsUseSeconds;
	const float StaminaCost = (bTool
		? HeldItem->Tool.StaminaCost * MadFall::Harvest::GetModMultiplier(Held, Definitions, FName(TEXT("stamina_cost")))
		: BareHandsStamina) * GetPerkMultiplier(FName(TEXT("stamina_cost")));

	NextUseTime = Now + UseSeconds;

	if (!Survival->TrySpendStamina(StaminaCost))
	{
		PushMessage(TEXT("Too exhausted to swing."), 1.0f);
		return false;
	}
	Survival->NoteExertion(UseSeconds);
	ViewModel->PlaySwing();
	ReportNoise();

	if (bHitZombie)
	{
		float Damage = 6.0f;
		FName DamageType(TEXT("madfall:blunt"));
		if (bTool && HeldItem->Tool.Damage.Num() > 0)
		{
			// The damage type that does the most to this creature after its resistance.
			Damage = MadFall::Combat::ChooseDamage(HeldItem->Tool.Damage,
				[Victim](FName Type) { return Victim->GetDamageMultiplier(Type); }, DamageType);
			Damage *= MadFall::Harvest::GetModMultiplier(Held, Definitions, FName(TEXT("damage")));
		}
		if (Victim->GetDamageMultiplier(DamageType) <= 0.6f)
		{
			// Say why a swing did little, once in a while, so armour is learnable.
			PushMessage(TEXT("It barely feels that."), 1.2f);
		}

		Damage *= GetPerkMultiplier(FName(TEXT("melee_damage")));
		if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
		{
			Audio->PlayAt(EMadSound::FleshHit, VictimActor->GetActorLocation());
		}
		if (Victim->ReceiveHit(Damage, DamageType, this))
		{
			PushMessage(TEXT("Killed it."), 1.5f);
		}

		if (bTool && Held.Durability >= 0)
		{
			const bool bBroke = MadFall::Harvest::ConsumeDurability(Held, Definitions, Random);
			Inventory->SetSelectedStack(Held);
			if (bBroke)
			{
				PushMessage(FString::Printf(TEXT("Your %s broke."), *MadFall::GetGameplayDefinitions().GetItemName(HeldItem->Id)), 3.0f);
			}
		}
		return true;
	}

	const FMadToolHit Hit = MadFall::Harvest::ComputeHit(HeldPtr ? &Held : nullptr, Definitions, *Block);
	if (Hit.bPenalised && LastPenaltyTarget != Target.Voxel)
	{
		LastPenaltyTarget = Target.Voxel;
		PushMessage(FString::Printf(TEXT("Wrong tool for %s - it will not drop anything."), *FMadGameplayDefinitions::GetBlockName(Block->Id)), 2.0f);
	}

	const FMadBlockDamageResult Result = Structural->ApplyBlockDamage(Target.Voxel, Hit.Amount * GetPerkMultiplier(FName(TEXT("mining_damage"))), Hit.DamageType);
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayForMaterial(Result.bDestroyed ? EMadSound::Break : EMadSound::Hit, Block->MaterialClass,
			(FVector(Target.Voxel) + FVector(0.5)) * MadFall::VoxelSizeUU);
	}

	if (Result.bDestroyed)
	{
		TArray<FMadItemStack> Drops;
		MadFall::Harvest::RollDrops(*Block, HeldPtr ? &Held : nullptr, Hit.bHarvests, Definitions, Random, Drops);

		GiveOrDrop(Drops, (FVector(Target.Voxel) + FVector(0.5)) * MadFall::VoxelSizeUU);
		AddExperience(Hit.bHarvests ? 2 : 1);
		NotifyQuest(EMadQuestObjectiveType::Break, Block->Id, Block->Tags, 1, Target.Voxel);
		UpdateTarget();
	}

	if (bTool && Held.Durability >= 0)
	{
		const bool bBroke = MadFall::Harvest::ConsumeDurability(Held, Definitions, Random);
		Inventory->SetSelectedStack(Held);
		if (bBroke)
		{
			PushMessage(FString::Printf(TEXT("Your %s broke."), *MadFall::GetGameplayDefinitions().GetItemName(HeldItem->Id)), 3.0f);
		}
	}

	return true;
}

bool AMadPlayerCharacter::FireRanged(const FMadItemDefinition& Weapon, double Now)
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadToolStats& Tool = Weapon.Tool;
	FMadInventory& Items = Inventory->GetInventory();
	if (Items.CountItem(Tool.Ammo) <= 0)
	{
		PushMessage(FString::Printf(TEXT("No %s."), *Definitions.GetItemName(Tool.Ammo)), 1.5f);
		return false;
	}

	FMadItemStack Held = *Inventory->GetSelectedStack();
	NextUseTime = Now + Tool.UseSeconds * MadFall::Harvest::GetModMultiplier(Held, Definitions, FName(TEXT("use_seconds")));
	const float StaminaCost = Tool.StaminaCost * MadFall::Harvest::GetModMultiplier(Held, Definitions, FName(TEXT("stamina_cost")))
		* GetPerkMultiplier(FName(TEXT("stamina_cost")));
	if (!Survival->TrySpendStamina(StaminaCost))
	{
		PushMessage(TEXT("Too exhausted to draw."), 1.0f);
		return false;
	}

	float Damage = 0.0f;
	FName DamageType(TEXT("madfall:pierce"));
	for (const TPair<FName, float>& Pair : Tool.Damage)
	{
		if (Pair.Value > Damage)
		{
			Damage = Pair.Value;
			DamageType = Pair.Key;
		}
	}
	Damage *= MadFall::Harvest::GetModMultiplier(Held, Definitions, FName(TEXT("damage"))) * GetPerkMultiplier(FName(TEXT("ranged_damage")));

	// From just in front of the eye, so it never starts inside the survivor.
	const FVector Direction = GetControlRotation().Vector();
	const FVector Start = Camera->GetComponentLocation() + Direction * 40.0f;
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Owner = this;
	AMadProjectile* Arrow = GetWorld()->SpawnActor<AMadProjectile>(AMadProjectile::StaticClass(), Start, Direction.Rotation(), Params);
	if (Arrow == nullptr)
	{
		return false;
	}
	Arrow->Launch(this, Direction * Tool.ProjectileSpeed * MadFall::VoxelSizeUU, Tool.ProjectileGravity, Damage, DamageType,
		Tool.Ammo, Tool.RecoverChance);
	verify(Items.Remove(Tool.Ammo, 1));

	// A bow is quiet: unlike a swing, a shot is not a noise zombies or deer hear.
	Survival->NoteExertion(0.3f);
	ViewModel->PlayUse();
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayAt(EMadSound::BowRelease, Start, 0.7f);
	}

	if (Held.Durability >= 0)
	{
		const bool bBroke = MadFall::Harvest::ConsumeDurability(Held, Definitions, Random);
		Inventory->SetSelectedStack(Held);
		if (bBroke)
		{
			PushMessage(FString::Printf(TEXT("Your %s broke."), *Definitions.GetItemName(Weapon.Id)), 3.0f);
		}
	}
	return true;
}

bool AMadPlayerCharacter::UseSecondary()
{
	if (RespawnAt >= 0.0f)
	{
		return false;
	}

	const FMadItemStack* HeldPtr = Inventory->GetSelectedStack();
	if (HeldPtr == nullptr)
	{
		return false;
	}

	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadItemDefinition* Item = Definitions.FindItem(HeldPtr->Item);
	if (Item == nullptr)
	{
		return false;
	}

	if (Item->bHasConsumable)
	{
		Survival->ApplyEffects(Item->Consumable.Effects);
		ViewModel->PlayUse();
		FMadItemStack Held = *HeldPtr;
		Held.Count -= 1;
		Inventory->SetSelectedStack(Held);
		if (!Item->Consumable.Returns.IsNone() && Definitions.FindItem(Item->Consumable.Returns) != nullptr)
		{
			FMadItemStack Returned;
			Returned.Item = Item->Consumable.Returns;
			Returned.Count = 1;
			GiveOrDrop({ Returned }, GetActorLocation());
		}
		PushMessage(FString::Printf(TEXT("Used %s."), *MadFall::GetGameplayDefinitions().GetItemName(Item->Id)), 1.5f);
		return true;
	}

	if (!Item->FillsInto.IsNone() && TryFill(*Item))
	{
		return true;
	}

	if (Item->bHasWear)
	{
		if (!Inventory->WearSelected())
		{
			return false;
		}
		Survival->SetWear(Inventory->GetWearTotals());
		ViewModel->PlayUse();
		PushMessage(FString::Printf(TEXT("Wearing %s."), *Definitions.GetItemName(Item->Id)), 1.5f);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Wearing %s on the %s."), *Item->Id.ToString(), *Item->Wear.Slot.ToString());
		NotifyQuest(EMadQuestObjectiveType::Wear, Item->Id, Item->Tags);
		return true;
	}

	if (!Item->TillsInto.IsNone())
	{
		// A hoe never places anything, so a failed till is still the whole action.
		return TryTill(*Item);
	}

	if (Item->PlacesBlock.IsNone() || !bHasTarget || Target.Normal == FIntVector::ZeroValue)
	{
		return false;
	}

	UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return false;
	}

	const FIntVector Place = Target.Voxel + Target.Normal;
	if (VoxelWorld->GetVoxel(Place.X, Place.Y, Place.Z).IsSolid())
	{
		return false;
	}

	// Never place a block inside the survivor. Shrunk slightly so standing flush
	// against a wall still lets you build the wall.
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const uint16 BlockId = Blocks.ResolveRuntimeId(Item->PlacesBlock);
	const FMadBlockDefinitionData* Block = Blocks.FindDefinition(BlockId);
	if (Block == nullptr)
	{
		return false;
	}

	// Seeds go on farmland: the block underneath must carry the placement tag.
	if (!Block->PlaceOnTag.IsNone())
	{
		const FMadBlockDefinitionData* Below = Blocks.FindDefinition(VoxelWorld->GetVoxel(Place.X, Place.Y, Place.Z - 1).BlockTypeID);
		if (Below == nullptr || !Below->Tags.Contains(Block->PlaceOnTag))
		{
			PushMessage(FString::Printf(TEXT("%s needs to go on %s."), *FMadGameplayDefinitions::GetBlockName(Block->Id),
				*MadFall::Localize(TEXT("@tags.") + Block->PlaceOnTag.ToString())), 2.0f);
			return false;
		}
	}

	// Blocks you walk through (a ladder) may be placed where you stand.
	const FBox BlockBox(FVector(Place) * MadFall::VoxelSizeUU, (FVector(Place) + FVector(1.0)) * MadFall::VoxelSizeUU);
	const FBox Body = GetCapsuleComponent()->Bounds.GetBox().ExpandBy(-2.0);
	if (Block->Collision != EMadBlockCollisionKind::None && BlockBox.Intersect(Body))
	{
		PushMessage(TEXT("Something is in the way."), 1.0f);
		return false;
	}

	// Facing blocks (doors, ladders, stations) turn to face away from the survivor:
	// their local +X points where the survivor is looking, snapped to a quarter turn.
	uint8 Orientation = 0;
	if (Block->RotationMode == EMadBlockRotationMode::Facing4)
	{
		const int32 QuarterTurns = FMath::RoundToInt32(GetControlRotation().Yaw / 90.0);
		const int32 Index = MadFall::Orientation::FindIndex(MadFall::Orientation::MakeYaw(QuarterTurns));
		Orientation = Index == INDEX_NONE ? 0 : static_cast<uint8>(Index);
	}

	FMadVoxel Voxel;
	Voxel.BlockTypeID = BlockId;
	Voxel.Density = 255;
	Voxel.Damage = 0;
	Voxel.Rotation = Orientation;
	Voxel.Flags = 0;
	Voxel.SetFlag(EMadVoxelFlags::Cubic, Block->ShapeKind != EMadBlockShapeKind::Isosurface);

	if (!VoxelWorld->SetVoxel(Place.X, Place.Y, Place.Z, Voxel))
	{
		return false;
	}
	ReportNoise();
	ViewModel->PlayUse();
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayAt(EMadSound::Place, (FVector(Place) + FVector(0.5)) * MadFall::VoxelSizeUU);
	}

	FMadItemStack Held = *HeldPtr;
	Held.Count -= 1;
	Inventory->SetSelectedStack(Held);
	UpdateTarget();
	NotifyQuest(EMadQuestObjectiveType::Place, Block->Id, Block->Tags, 1, Place);
	return true;
}

bool AMadPlayerCharacter::ToggleBlock(const FIntVector& Voxel, const FMadBlockDefinitionData& Block)
{
	UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const uint16 ToId = Registry.ResolveRuntimeId(Block.ToggleTo);
	if (VoxelWorld == nullptr || ToId == MadFall::BlockTypeUnresolved || ToId == MadFall::BlockTypeAir)
	{
		return false;
	}

	const uint16 FromId = VoxelWorld->GetVoxel(Voxel.X, Voxel.Y, Voxel.Z).BlockTypeID;
	int32 Toggled = 0;
	// Both halves of a two-high door open together: the matching blocks directly
	// above and below, up to a three-high gate.
	for (int32 Direction : { 0, 1, -1 })
	{
		for (int32 Step = (Direction == 0 ? 0 : 1); Step <= (Direction == 0 ? 0 : 2); ++Step)
		{
			const FIntVector At = Voxel + FIntVector(0, 0, Direction * Step);
			FMadVoxel Current = VoxelWorld->GetVoxel(At.X, At.Y, At.Z);
			if (Current.BlockTypeID != FromId)
			{
				break;
			}
			// Orientation, damage and flags carry over: an opened door stays hung
			// the way it was placed, and a battered one stays battered.
			Current.BlockTypeID = ToId;
			Toggled += VoxelWorld->SetVoxel(At.X, At.Y, At.Z, Current) ? 1 : 0;
		}
	}

	if (Toggled > 0)
	{
		ReportNoise();
		if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
		{
			Audio->PlayForMaterial(EMadSound::Hit, Block.MaterialClass, (FVector(Voxel) + FVector(0.5)) * MadFall::VoxelSizeUU, 0.6f);
		}
		UE_LOG(LogMadFallGameplay, Display, TEXT("Toggled %d x %s to %s."), Toggled, *Block.Id.ToString(), *Block.ToggleTo.ToString());
		UpdateTarget();
	}
	return Toggled > 0;
}

bool AMadPlayerCharacter::TryFill(const FMadItemDefinition& Item)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadItemDefinition* Filled = MadFall::GetGameplayDefinitions().FindItem(Item.FillsInto);
	if (VoxelWorld == nullptr || Filled == nullptr)
	{
		return false;
	}

	// The normal target skips liquid; this looks for it, stopping at the first solid.
	auto IsWaterOrSolid = [VoxelWorld](const FIntVector& V)
	{
		return VoxelWorld->GetVoxel(V.X, V.Y, V.Z).IsSolid();
	};
	FMadVoxelHit Hit;
	if (!MadFall::VoxelRaycast(Camera->GetComponentLocation() / MadFall::VoxelSizeUU, GetControlRotation().Vector(), GetReach(), IsWaterOrSolid, Hit))
	{
		return false;
	}
	// Generated water carries the liquid flag; a water block placed by a mod or
	// the console may only say so in its definition.
	const FMadVoxel HitVoxel = VoxelWorld->GetVoxel(Hit.Voxel.X, Hit.Voxel.Y, Hit.Voxel.Z);
	const FMadBlockDefinitionData* HitDef = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(HitVoxel.BlockTypeID);
	if (!HitVoxel.HasFlag(EMadVoxelFlags::Liquid) && !(HitDef && HitDef->bLiquid))
	{
		return false;
	}

	FMadItemStack Held = *Inventory->GetSelectedStack();
	Held.Count -= 1;
	Inventory->SetSelectedStack(Held);
	FMadItemStack Result;
	Result.Item = Filled->Id;
	Result.Count = 1;
	GiveOrDrop({ Result }, GetActorLocation());
	ViewModel->PlayUse();
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayForMaterial(EMadSound::Step, FName(TEXT("madfall:water")), (FVector(Hit.Voxel) + FVector(0.5)) * MadFall::VoxelSizeUU);
	}
	PushMessage(FString::Printf(TEXT("Filled: %s."), *MadFall::GetGameplayDefinitions().GetItemName(Filled->Id)), 1.5f);
	UE_LOG(LogMadFallGameplay, Display, TEXT("Filled %s from water at %s."), *Filled->Id.ToString(), *Hit.Voxel.ToString());
	return true;
}

bool AMadPlayerCharacter::TryTill(const FMadItemDefinition& Item)
{
	UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr || !bHasTarget)
	{
		return false;
	}

	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const FMadVoxel Targeted = VoxelWorld->GetVoxel(Target.Voxel.X, Target.Voxel.Y, Target.Voxel.Z);
	const FMadBlockDefinitionData* From = Blocks.FindDefinition(Targeted.BlockTypeID);
	const uint16 IntoId = Blocks.ResolveRuntimeId(Item.TillsInto);
	const FMadBlockDefinitionData* Into = Blocks.FindDefinition(IntoId);
	if (From == nullptr || Into == nullptr || From->Id == Into->Id || !From->Tags.Contains(FName(TEXT("block.tillable"))))
	{
		return false;
	}

	// Only open ground: tilling under a wall or a crop would bury what it grows.
	const FMadVoxel Above = VoxelWorld->GetVoxel(Target.Voxel.X, Target.Voxel.Y, Target.Voxel.Z + 1);
	if (Above.IsSolid())
	{
		PushMessage(TEXT("Clear the ground above first."), 1.5f);
		return false;
	}

	FMadVoxel Tilled;
	Tilled.BlockTypeID = IntoId;
	Tilled.Density = 255;
	Tilled.Damage = 0;
	Tilled.Rotation = 0;
	Tilled.Flags = 0;
	Tilled.SetFlag(EMadVoxelFlags::Cubic, Into->ShapeKind != EMadBlockShapeKind::Isosurface);
	if (!VoxelWorld->SetVoxel(Target.Voxel.X, Target.Voxel.Y, Target.Voxel.Z, Tilled))
	{
		return false;
	}

	FMadItemStack Held = *Inventory->GetSelectedStack();
	if (Held.Durability >= 0)
	{
		const bool bBroke = MadFall::Harvest::ConsumeDurability(Held, MadFall::GetGameplayDefinitions(), Random);
		Inventory->SetSelectedStack(Held);
		if (bBroke)
		{
			PushMessage(FString::Printf(TEXT("Your %s broke."), *MadFall::GetGameplayDefinitions().GetItemName(Item.Id)), 3.0f);
		}
	}

	ReportNoise();
	ViewModel->PlaySwing();
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayForMaterial(EMadSound::Hit, Into->MaterialClass, (FVector(Target.Voxel) + FVector(0.5)) * MadFall::VoxelSizeUU);
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Tilled %s into %s at %s."), *From->Id.ToString(), *Into->Id.ToString(), *Target.Voxel.ToString());
	UpdateTarget();
	return true;
}

bool AMadPlayerCharacter::Interact()
{
	if (const AMadTrader* Trader = AimedTrader.Get())
	{
		OpenTrade(Trader->GetMarkerVoxel(), Trader->GetTraderId());
		return true;
	}
	if (!bHasTarget)
	{
		return false;
	}

	if (const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>())
	{
		const FMadVoxel Voxel = VoxelWorld->GetVoxel(Target.Voxel.X, Target.Voxel.Y, Target.Voxel.Z);
		if (const FMadBlockDefinitionData* Block = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(Voxel.BlockTypeID))
		{
			if (!Block->ToggleTo.IsNone())
			{
				return ToggleBlock(Target.Voxel, *Block);
			}
			if (Block->bSpawnPoint)
			{
				BedVoxel = Target.Voxel;
				PushMessage(TEXT("You will wake up here."), 2.0f);
				UE_LOG(LogMadFallGameplay, Display, TEXT("Respawn point set at %s."), *Target.Voxel.ToString());
				NotifyQuest(EMadQuestObjectiveType::SetSpawn, Block->Id, Block->Tags, 1, Target.Voxel);
				return true;
			}
		}
	}

	UMadContainerSubsystem* Containers = GetWorld()->GetSubsystem<UMadContainerSubsystem>();
	if (Containers == nullptr || !Containers->IsContainer(Target.Voxel))
	{
		return false;
	}

	// Experience for discovering loot, once per crate: the first open is what
	// rolls it, so whether it was rolled before is the test.
	const FMadContainer* Known = Containers->Find(Target.Voxel);
	const bool bFirstOpen = Known == nullptr || !Known->bRolled;
	const FMadContainer* Container = Containers->Open(Target.Voxel, GetGameStage());
	if (Container == nullptr)
	{
		return false;
	}
	if (bFirstOpen && !Container->LootTable.IsNone())
	{
		AddExperience(5);
	}

	OpenInventory(Target.Voxel);
	if (Container->Contents.IsEmpty())
	{
		PushMessage(TEXT("Empty."), 1.5f);
	}
	return true;
}

bool AMadPlayerCharacter::IsStationNearby(FName Station) const
{
	if (Station.IsNone())
	{
		return true;
	}

	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return false;
	}

	const uint16 StationBlock = UMadVoxelWorldSubsystem::GetBlockRegistry().ResolveRuntimeId(Station);
	if (StationBlock == MadFall::BlockTypeAir || StationBlock == MadFall::BlockTypeUnresolved)
	{
		return false;
	}

	const FIntVector Centre = GetFeetVoxel() + FIntVector(0, 0, 1);
	for (int32 Z = -StationRadius; Z <= StationRadius; ++Z)
	{
		for (int32 Y = -StationRadius; Y <= StationRadius; ++Y)
		{
			for (int32 X = -StationRadius; X <= StationRadius; ++X)
			{
				if (VoxelWorld->GetVoxel(Centre.X + X, Centre.Y + Y, Centre.Z + Z).BlockTypeID == StationBlock)
				{
					return true;
				}
			}
		}
	}
	return false;
}

EMadCraftResult AMadPlayerCharacter::CraftRecipe(FName RecipeId, int32 Times)
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadRecipeDefinition* Recipe = Definitions.FindRecipe(RecipeId);
	if (Recipe == nullptr)
	{
		return EMadCraftResult::UnknownItem;
	}

	// The station the player "is at" is the recipe's own station if one is in
	// reach - so standing between a workbench and a forge works for both.
	const FName Station = IsStationNearby(Recipe->Station) ? Recipe->Station : NAME_None;
	const EMadCraftResult Result = MadFall::Crafting::TakeIngredients(Inventory->GetInventory(), *Recipe, Definitions, Station, Level, Times);
	Inventory->NotifyChanged();

	if (Result != EMadCraftResult::Ok)
	{
		PushMessage(FString::Printf(TEXT("Cannot craft %s: %s."), *MadFall::GetGameplayDefinitions().GetItemName(Recipe->Output.Item), MadFall::Crafting::ToString(Result)), 2.0f);
		return Result;
	}

	// Ingredients are taken now so they cannot be spent twice; the output
	// arrives when the job finishes. Output that does not fit is dropped at the
	// survivor's feet instead of failing the craft after the wait.
	FCraftJob& Job = CraftQueue.AddDefaulted_GetRef();
	Job.Recipe = Recipe->Id;
	Job.Times = FMath::Max(1, Times);
	Job.Total = FMath::Max(0.0f, Recipe->CraftSeconds * Job.Times * CraftTimeScale() * GetPerkMultiplier(FName(TEXT("craft_time"))));
	Job.Remaining = Job.Total;
	PushMessage(FString::Printf(TEXT("Crafting %d x %s (%.0f s)."), Recipe->Output.Count * Job.Times, *MadFall::GetGameplayDefinitions().GetItemName(Recipe->Output.Item), Job.Total), 2.0f);
	UE_LOG(LogMadFallGameplay, Display, TEXT("Craft queued: %d x %s (%.1f s)."), Recipe->Output.Count * Job.Times, *Recipe->Output.Item.ToString(), Job.Total);
	TickCrafting(0.0f);
	return Result;
}

float AMadPlayerCharacter::CraftTimeScale()
{
	return FMath::Max(0.0f, CVarCraftTimeScale.GetValueOnGameThread());
}

void AMadPlayerCharacter::TickCrafting(float DeltaSeconds)
{
	if (CraftQueue.Num() == 0)
	{
		return;
	}

	FCraftJob& Job = CraftQueue[0];
	Job.Remaining -= DeltaSeconds;
	if (Job.Remaining > 0.0f)
	{
		return;
	}

	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadRecipeDefinition* Recipe = Definitions.FindRecipe(Job.Recipe);
	const FMadItemDefinition* Output = Recipe ? Definitions.FindItem(Recipe->Output.Item) : nullptr;
	if (Recipe != nullptr && Output != nullptr)
	{
		TArray<FMadItemStack> Made;
		const int32 Total = Recipe->Output.Count * Job.Times;
		if (Output->MaxStack == 1)
		{
			for (int32 Piece = 0; Piece < Total; ++Piece)
			{
				Made.Add(FMadItemStack::Make(*Output, 1));
			}
		}
		else
		{
			Made.Add(FMadItemStack::Make(*Output, Total));
		}

		GiveOrDrop(Made, GetActorLocation());
		AddExperience(FMath::Max(1, FMath::RoundToInt32(Recipe->CraftSeconds * 3.0f * Job.Times)));
		PushMessage(FString::Printf(TEXT("Crafted %d x %s."), Total, *MadFall::GetGameplayDefinitions().GetItemName(Output->Id)), 2.0f);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Craft finished: %d x %s."), Total, *Output->Id.ToString());
		NotifyQuest(EMadQuestObjectiveType::Craft, Output->Id, Output->Tags, Total);
		if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
		{
			Audio->Play2D(EMadSound::CraftDone, 0.7f);
		}
	}

	CraftQueue.RemoveAt(0);
}

void AMadPlayerCharacter::CancelCrafting()
{
	if (CraftQueue.Num() == 0)
	{
		return;
	}

	TArray<FMadItemStack> Refund;
	GetCraftRefund(Refund);
	CraftQueue.Reset();
	GiveOrDrop(Refund, GetActorLocation());
	PushMessage(TEXT("Crafting cancelled; ingredients returned."), 2.0f);
}

void AMadPlayerCharacter::GetCraftRefund(TArray<FMadItemStack>& OutRefund) const
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	for (const FCraftJob& Job : CraftQueue)
	{
		if (const FMadRecipeDefinition* Recipe = Definitions.FindRecipe(Job.Recipe))
		{
			for (const FMadItemAmount& Ingredient : Recipe->Ingredients)
			{
				if (const FMadItemDefinition* Item = Definitions.FindItem(Ingredient.Item))
				{
					const int32 Count = Ingredient.Count * Job.Times;
					// Equipment ingredients come back one piece per stack, at full
					// durability - the worn original was not tracked through the craft.
					for (int32 Given = 0; Given < Count; Given += Item->MaxStack)
					{
						OutRefund.Add(FMadItemStack::Make(*Item, FMath::Min(Item->MaxStack, Count - Given)));
					}
				}
			}
		}
	}
}

void AMadPlayerCharacter::AddExperience(int32 Amount)
{
	Experience += FMath::Max(0, Amount);
	while (Experience >= GetExperienceForNextLevel())
	{
		Experience -= GetExperienceForNextLevel();
		++Level;
		PushMessage(FString::Printf(TEXT("Level %d."), Level), 3.0f);
	}
}

int32 AMadPlayerCharacter::GetGameStage() const
{
	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	return (Clock ? Clock->GetDay() : 1) + Level;
}

void AMadPlayerCharacter::PushMessage(const FString& Text, float Seconds)
{
	Messages.Add(FMessage{ Text, GetWorld() ? GetWorld()->GetTimeSeconds() + Seconds : 0.0 });
	if (Messages.Num() > 5)
	{
		Messages.RemoveAt(0);
	}
	UE_LOG(LogMadFallGameplay, Log, TEXT("[player] %s"), *Text);
}

FString AMadPlayerCharacter::DescribeStatus() const
{
	const FMadSurvivalStats S = Survival->GetStats();
	TStringBuilder<2048> Out;
	Out.Appendf(TEXT("Player at voxel %s%s, level %d (%d/%d xp), game stage %d\n"),
		*GetFeetVoxel().ToString(), bWaitingForWorld ? TEXT(" (waiting for world)") : TEXT(""),
		Level, Experience, GetExperienceForNextLevel(), GetGameStage());
	Out.Appendf(TEXT("  health %.1f/%.0f  stamina %.1f/%.0f  food %.1f  water %.1f  core %.2f C  infection %.1f  ambient %.1f C\n"),
		S.Health, S.MaxHealth, S.Stamina, S.MaxStamina, S.Food, S.Water, S.CoreTemperature, S.Infection, Survival->GetAmbientTemperature());
	Out.Appendf(TEXT("  breath %.0f  %s (submerged %.0f%%)\n"), S.Breath,
		bSwimming ? (Survival->IsHeadUnderwater() ? TEXT("underwater") : TEXT("swimming")) : TEXT("on land"),
		Survival->GetSubmersion() * 100.0f);
	Out.Appendf(TEXT("  worn: armor %.0f%%  cold +%.0f C  heat +%.0f C\n"),
		Survival->GetArmor() * 100.0f, Survival->GetColdInsulation(), Survival->GetHeatInsulation());
	if (bHasTarget)
	{
		Out.Appendf(TEXT("  target %s (face %s, %.2f voxels)\n"), *Target.Voxel.ToString(), *Target.Normal.ToString(), Target.Distance);
	}
	Out.Append(TEXT("  inventory:\n"));
	Out.Append(Inventory->Describe());
	return FString(Out.ToString());
}

// ===========================================================================
// Input
// ===========================================================================

void AMadPlayerCharacter::EnsureInput()
{
	if (MappingContext != nullptr)
	{
		return;
	}

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("MadPlayerContext"));

	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = Type;
		Actions.Add(Action);
		return Action;
	};

	MoveAction = MakeAction(TEXT("IA_Move"), EInputActionValueType::Axis2D);
	LookAction = MakeAction(TEXT("IA_Look"), EInputActionValueType::Axis2D);
	JumpAction = MakeAction(TEXT("IA_Jump"), EInputActionValueType::Boolean);
	SprintAction = MakeAction(TEXT("IA_Sprint"), EInputActionValueType::Boolean);
	PrimaryAction = MakeAction(TEXT("IA_Primary"), EInputActionValueType::Boolean);
	SecondaryAction = MakeAction(TEXT("IA_Secondary"), EInputActionValueType::Boolean);
	InteractAction = MakeAction(TEXT("IA_Interact"), EInputActionValueType::Boolean);
	ScrollAction = MakeAction(TEXT("IA_Scroll"), EInputActionValueType::Axis1D);
	CraftAction = MakeAction(TEXT("IA_Craft"), EInputActionValueType::Boolean);
	RepairAction = MakeAction(TEXT("IA_Repair"), EInputActionValueType::Boolean);
	DropAction = MakeAction(TEXT("IA_Drop"), EInputActionValueType::Boolean);
	InventoryAction = MakeAction(TEXT("IA_Inventory"), EInputActionValueType::Boolean);
	PauseAction = MakeAction(TEXT("IA_Pause"), EInputActionValueType::Boolean);
	MapAction = MakeAction(TEXT("IA_Map"), EInputActionValueType::Boolean);

	for (int32 Slot = 0; Slot < 9; ++Slot)
	{
		HotbarActions.Add(MakeAction(*FString::Printf(TEXT("IA_Hotbar%d"), Slot + 1), EInputActionValueType::Boolean));
	}

	MapKeys();
}

void AMadPlayerCharacter::MapKeys()
{
	if (MappingContext == nullptr)
	{
		return;
	}
	// Rebuilt whole rather than patched: a rebinding can swap two actions' keys,
	// and a dozen mappings cost nothing to recreate.
	MappingContext->UnmapAll();
	const FMadKeyBindings& Keys = MadFall::Input::GetActive();

	// Movement onto a 2D axis: forward/back drive Y (swizzled from X), back and left are negated.
	auto MapMove = [this](FKey Key, bool bSwizzle, bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(MoveAction, Key);
		if (bSwizzle)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(this));
		}
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(this));
		}
	};
	MapMove(Keys.Get(TEXT("move_forward")), true, false);
	MapMove(Keys.Get(TEXT("move_back")), true, true);
	MapMove(Keys.Get(TEXT("move_right")), false, false);
	MapMove(Keys.Get(TEXT("move_left")), false, true);

	{
		FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(LookAction, EKeys::Mouse2D);
		UInputModifierNegate* NegateY = NewObject<UInputModifierNegate>(this);
		NegateY->bX = false;
		NegateY->bY = true;
		NegateY->bZ = false;
		Mapping.Modifiers.Add(NegateY);
	}

	MappingContext->MapKey(JumpAction, Keys.Get(TEXT("jump")));
	MappingContext->MapKey(SprintAction, Keys.Get(TEXT("sprint")));
	MappingContext->MapKey(InteractAction, Keys.Get(TEXT("interact")));
	MappingContext->MapKey(CraftAction, Keys.Get(TEXT("craft")));
	MappingContext->MapKey(RepairAction, Keys.Get(TEXT("repair")));
	MappingContext->MapKey(DropAction, Keys.Get(TEXT("drop")));
	MappingContext->MapKey(InventoryAction, Keys.Get(TEXT("inventory")));
	MappingContext->MapKey(MapAction, Keys.Get(TEXT("map")));

	// Not rebindable (FMadKeyBindings::IsReserved says why).
	MappingContext->MapKey(PrimaryAction, EKeys::LeftMouseButton);
	MappingContext->MapKey(SecondaryAction, EKeys::RightMouseButton);
	MappingContext->MapKey(ScrollAction, EKeys::MouseWheelAxis);
	MappingContext->MapKey(PauseAction, EKeys::Escape);
	MappingContext->MapKey(PauseAction, EKeys::P);
	const FKey HotbarKeys[] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
	for (int32 Slot = 0; Slot < HotbarActions.Num() && Slot < UE_ARRAY_COUNT(HotbarKeys); ++Slot)
	{
		MappingContext->MapKey(HotbarActions[Slot], HotbarKeys[Slot]);
	}

	if (const APlayerController* PlayerController = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			InputSubsystem->RequestRebuildControlMappings();
		}
	}

	FString Changed;
	for (const TPair<FName, FString>& Pair : Keys.ToOverrides())
	{
		Changed += FString::Printf(TEXT(" %s=%s"), *Pair.Key.ToString(), *Pair.Value);
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Input: keys mapped%s."), Changed.IsEmpty() ? TEXT(" (defaults)") : *(TEXT(",") + Changed));
}

void AMadPlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	EnsureInput();

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (Input == nullptr)
	{
		UE_LOG(LogMadFallGameplay, Error, TEXT("Player input component is not an EnhancedInputComponent; check DefaultInput.ini."));
		return;
	}

	Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AMadPlayerCharacter::OnMove);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &AMadPlayerCharacter::OnLook);
	Input->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
	Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
	Input->BindAction(SprintAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnSprint);
	Input->BindAction(SprintAction, ETriggerEvent::Completed, this, &AMadPlayerCharacter::OnSprint);
	Input->BindAction(PrimaryAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnPrimaryStarted);
	Input->BindAction(PrimaryAction, ETriggerEvent::Completed, this, &AMadPlayerCharacter::OnPrimaryCompleted);
	Input->BindAction(SecondaryAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnSecondary);
	Input->BindAction(InteractAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnInteract);
	Input->BindAction(ScrollAction, ETriggerEvent::Triggered, this, &AMadPlayerCharacter::OnScroll);
	Input->BindAction(CraftAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnToggleCraft);
	Input->BindAction(RepairAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnRepair);
	Input->BindAction(DropAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnDrop);
	Input->BindAction(InventoryAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnToggleInventory);
	Input->BindAction(PauseAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnPause);
	Input->BindAction(MapAction, ETriggerEvent::Started, this, &AMadPlayerCharacter::OnToggleMap);

	for (int32 Slot = 0; Slot < HotbarActions.Num(); ++Slot)
	{
		Input->BindAction(HotbarActions[Slot], ETriggerEvent::Started, this, &AMadPlayerCharacter::OnHotbar, Slot);
	}
}

void AMadPlayerCharacter::OnMove(const FInputActionValue& Value)
{
	if (bWaitingForWorld || RespawnAt >= 0.0f)
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	ClimbInput = static_cast<float>(Axis.Y);
	if (bSwimming)
	{
		// A swimmer goes where they look: pitch counts, so looking down and
		// pushing forward dives. On land the walk stays on the ground plane.
		const FVector Forward = FRotationMatrix(GetControlRotation()).GetUnitAxis(EAxis::X);
		const FRotator Yaw(0.0, GetControlRotation().Yaw, 0.0);
		AddMovementInput(Forward, Axis.Y);
		AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y), Axis.X);
		DiveInput = FMath::Clamp(DiveInput + static_cast<float>(Forward.Z) * static_cast<float>(Axis.Y), -1.0f, 1.0f);
		return;
	}
	const FRotator Yaw(0.0, GetControlRotation().Yaw, 0.0);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y), Axis.X);
}

void AMadPlayerCharacter::OnLook(const FInputActionValue& Value)
{
	// The mouse is a pointer while the inventory screen is up.
	if (bInventoryOpen)
	{
		return;
	}
	static const IConsoleVariable* Sensitivity = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.input.LookSensitivity"));
	static const IConsoleVariable* InvertY = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.input.InvertY"));
	const double Scale = Sensitivity ? Sensitivity->GetFloat() : 1.0;
	const FVector2D Axis = Value.Get<FVector2D>() * Scale;
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput((InvertY && InvertY->GetBool()) ? -Axis.Y : Axis.Y);
}

void AMadPlayerCharacter::OnSprint(const FInputActionValue& Value)
{
	const bool bSprint = Value.Get<bool>() && Survival->GetStats().Stamina > 5.0f;
	if (bSprint)
	{
		ReportNoise();
	}
	GetCharacterMovement()->MaxWalkSpeed = bSprint ? SprintSpeed : WalkSpeed;
	Survival->SetSprinting(bSprint);
}

void AMadPlayerCharacter::OnPrimaryStarted(const FInputActionValue& Value)
{
	if (bInventoryOpen)
	{
		return;   // clicks go to the HUD's slot hit boxes
	}
	bPrimaryHeld = true;
	UsePrimary();
}

void AMadPlayerCharacter::OnPrimaryCompleted(const FInputActionValue& Value)
{
	bPrimaryHeld = false;
}

void AMadPlayerCharacter::OnSecondary(const FInputActionValue& Value)
{
	if (!bInventoryOpen)
	{
		UseSecondary();
	}
}

void AMadPlayerCharacter::OnInteract(const FInputActionValue& Value)
{
	if (bInventoryOpen)
	{
		CloseInventory();
		return;
	}
	Interact();
}

void AMadPlayerCharacter::OnToggleMap(const FInputActionValue& Value)
{
	if (UMadWorldMapSubsystem* Map = GetWorld()->GetSubsystem<UMadWorldMapSubsystem>())
	{
		Map->SetOpen(!Map->IsOpen());
		if (Map->IsOpen())
		{
			CloseInventory();
		}
	}
}

void AMadPlayerCharacter::OnScroll(const FInputActionValue& Value)
{
	const int32 Step = Value.Get<float>() > 0.0f ? -1 : 1;
	if (UMadWorldMapSubsystem* Map = GetWorld()->GetSubsystem<UMadWorldMapSubsystem>(); Map && Map->IsOpen())
	{
		// Wheel up zooms in.
		Map->ZoomBy(Step);
		return;
	}
	if (bInventoryOpen && InventoryCursor.bActive)
	{
		// Holding a stack, the wheel sets how many: up takes more.
		AdjustHeldCount(-Step);
		return;
	}
	if (bInventoryOpen)
	{
		// The wheel scrolls the right-hand column.
		int32 Rows = MadFall::GetGameplayDefinitions().GetPerks().Num();
		if (InventoryTab == EMadInventoryTab::Crafting)
		{
			TArray<FMadRecipeRow> Recipes;
			GetRecipeRows(Recipes);
			Rows = Recipes.Num();
		}
		ColumnScroll = FMath::Clamp(ColumnScroll + Step, 0, FMath::Max(0, Rows - 1));
		return;
	}
	Inventory->SelectSlot(Inventory->GetSelectedSlot() + Step);
}

void AMadPlayerCharacter::OnToggleCraft(const FInputActionValue& Value)
{
	// Crafting is a column of the inventory screen, where the ingredients are in view.
	if (bInventoryOpen && InventoryTab == EMadInventoryTab::Crafting)
	{
		CloseInventory();
		return;
	}
	if (!bInventoryOpen)
	{
		OpenInventory();
	}
	SetInventoryTab(EMadInventoryTab::Crafting);
}

FName AMadPlayerCharacter::GetSelectedRecipe() const
{
	TArray<FMadRecipeRow> Rows;
	GetRecipeRows(Rows);
	if (Rows.ContainsByPredicate([this](const FMadRecipeRow& Row) { return Row.Recipe->Id == SelectedRecipe; }))
	{
		return SelectedRecipe;
	}
	return Rows.Num() > 0 ? Rows[0].Recipe->Id : NAME_None;
}

void AMadPlayerCharacter::GetRecipeRows(TArray<FMadRecipeRow>& Out) const
{
	MadFall::Crafting::ListRecipes(MadFall::GetGameplayDefinitions(), Inventory->GetInventory(), Level,
		[this](FName Station) { return IsStationNearby(Station); }, CraftCategory, bCraftableOnly, Out);
}

void AMadPlayerCharacter::OnHotbar(const FInputActionValue& Value, int32 Slot)
{
	Inventory->SelectSlot(Slot);
}

void AMadPlayerCharacter::OnRepair(const FInputActionValue& Value)
{
	RepairSelected();
}

void AMadPlayerCharacter::OnDrop(const FInputActionValue& Value)
{
	DropSelected();
}

// ===========================================================================
// Perks
// ===========================================================================

EMadPerkResult AMadPlayerCharacter::BuyPerk(FName PerkId)
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const EMadPerkResult Result = MadFall::Perks::TryBuy(PerkRanks, PerkId, Level, Definitions);
	if (Result == EMadPerkResult::Ok)
	{
		ApplyPerkStats();
		PushMessage(FString::Printf(TEXT("%s rank %d."), *Definitions.GetPerkName(PerkId),
			PerkRanks.FindRef(PerkId)), 2.0f);
	}
	else
	{
		PushMessage(FString::Printf(TEXT("Cannot take %s: %s."), *MadFall::GetGameplayDefinitions().GetPerkName(PerkId), MadFall::Perks::ToString(Result)), 2.0f);
	}
	return Result;
}

float AMadPlayerCharacter::GetPerkMultiplier(FName Stat) const
{
	return MadFall::Perks::GetMultiplier(PerkRanks, MadFall::GetGameplayDefinitions(), Stat);
}

int32 AMadPlayerCharacter::GetUnspentPerkPoints() const
{
	return MadFall::Perks::GetUnspentPoints(Level, PerkRanks);
}

void AMadPlayerCharacter::ApplyPerkStats()
{
	const FMadSurvivalStats Defaults;
	Survival->SetMaxVitals(Defaults.MaxHealth * GetPerkMultiplier(FName(TEXT("max_health"))),
		Defaults.MaxStamina * GetPerkMultiplier(FName(TEXT("max_stamina"))));
	const float Drain = MadFall::Difficulty::GetWorldScale(GetWorld(), FName(TEXT("survival_drain")));
	Survival->SetDrainMultipliers(GetPerkMultiplier(FName(TEXT("food_drain"))) * Drain, GetPerkMultiplier(FName(TEXT("water_drain"))) * Drain);
}

// ===========================================================================
// Inventory screen
// ===========================================================================

void AMadPlayerCharacter::OnToggleInventory(const FInputActionValue& Value)
{
	if (bInventoryOpen)
	{
		CloseInventory();
	}
	else
	{
		OpenInventory();
	}
}

void AMadPlayerCharacter::OnPause(const FInputActionValue& Value)
{
	// Escape backs out of whatever is open first; only with nothing open does it pause.
	if (UMadWorldMapSubsystem* Map = GetWorld()->GetSubsystem<UMadWorldMapSubsystem>(); Map && Map->IsOpen())
	{
		Map->SetOpen(false);
		return;
	}
	if (bInventoryOpen)
	{
		CloseInventory();
		return;
	}
	if (UMadMenuSubsystem* Menus = GetWorld()->GetSubsystem<UMadMenuSubsystem>())
	{
		Menus->OpenPauseMenu();
	}
}

void AMadPlayerCharacter::OpenInventory(TOptional<FIntVector> ContainerVoxel)
{
	bInventoryOpen = true;
	OpenContainerVoxel = ContainerVoxel;
	OpenTraderVoxel.Reset();
	InventoryCursor = FInventoryCursor();
	bPrimaryHeld = false;

	if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
	{
		PlayerController->bShowMouseCursor = true;
		PlayerController->bEnableClickEvents = true;
		PlayerController->ClickEventKeys.AddUnique(EKeys::LeftMouseButton);
		PlayerController->ClickEventKeys.AddUnique(EKeys::RightMouseButton);

		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways);
		PlayerController->SetInputMode(Mode);
	}
}

void AMadPlayerCharacter::CloseInventory()
{
	if (!bInventoryOpen)
	{
		return;
	}
	bInventoryOpen = false;
	OpenContainerVoxel.Reset();
	OpenTraderVoxel.Reset();
	InventoryCursor = FInventoryCursor();

	if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
	{
		PlayerController->bShowMouseCursor = false;
		PlayerController->bEnableClickEvents = false;
		PlayerController->SetInputMode(FInputModeGameOnly());
	}
}

void AMadPlayerCharacter::OpenTrade(const FIntVector& TraderMarker, FName TraderId)
{
	UMadTraderSubsystem* Traders = GetWorld()->GetSubsystem<UMadTraderSubsystem>();
	const FMadTraderDefinition* Trader = MadFall::GetGameplayDefinitions().FindTrader(TraderId);
	if (Traders == nullptr || Trader == nullptr || Traders->GetState(TraderMarker, TraderId) == nullptr)
	{
		return;
	}
	OpenInventory();
	OpenTraderVoxel = TraderMarker;
	OpenTraderId = TraderId;
	SetInventoryTab(EMadInventoryTab::Crafting);
	if (!Trader->Greeting.IsEmpty())
	{
		PushMessage(MadFall::Localize(Trader->Greeting), 4.0f);
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Trade opened with %s at %s."), *TraderId.ToString(), *TraderMarker.ToString());
}

bool AMadPlayerCharacter::GetOpenTrade(FMadTraderState*& OutState, const FMadTraderDefinition*& OutTrader) const
{
	UMadTraderSubsystem* Traders = OpenTraderVoxel.IsSet() ? GetWorld()->GetSubsystem<UMadTraderSubsystem>() : nullptr;
	OutTrader = Traders ? MadFall::GetGameplayDefinitions().FindTrader(OpenTraderId) : nullptr;
	OutState = OutTrader ? Traders->GetState(OpenTraderVoxel.GetValue(), OpenTraderId) : nullptr;
	return OutState != nullptr;
}

EMadTradeResult AMadPlayerCharacter::BuyFromTrader(int32 StockSlot, int32 Count)
{
	FMadTraderState* State = nullptr;
	const FMadTraderDefinition* Trader = nullptr;
	if (!GetOpenTrade(State, Trader))
	{
		return EMadTradeResult::NotForSale;
	}
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadItemStack Shelf = State->Stock.IsValidSlot(StockSlot) ? State->Stock.GetSlot(StockSlot) : FMadItemStack();
	int32 Paid = 0;
	const EMadTradeResult Result = MadFall::Trade::Buy(Inventory->GetInventory(), *State, StockSlot, Count <= 0 ? Shelf.Count : Count, *Trader, Definitions, Paid);
	Inventory->NotifyChanged();
	if (Result == EMadTradeResult::Ok)
	{
		PushMessage(FString::Printf(TEXT("Bought %s for %d %s."), *Definitions.GetItemName(Shelf.Item), Paid, *Definitions.GetItemName(Trader->Currency)), 2.0f);
		NotifyQuest(EMadQuestObjectiveType::Trade, Trader->Id, {});
	}
	else
	{
		PushMessage(FString::Printf(TEXT("Cannot buy: %s."), MadFall::Trade::ToString(Result)), 2.0f);
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Trade: buy %s from slot %d: %s (paid %d)."), *Shelf.Item.ToString(), StockSlot, MadFall::Trade::ToString(Result), Paid);
	return Result;
}

EMadTradeResult AMadPlayerCharacter::SellToTrader(int32 BackpackSlot, int32 Count)
{
	FMadTraderState* State = nullptr;
	const FMadTraderDefinition* Trader = nullptr;
	if (!GetOpenTrade(State, Trader))
	{
		return EMadTradeResult::NotBought;
	}
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	FMadInventory& Backpack = Inventory->GetInventory();
	const FMadItemStack Offered = Backpack.IsValidSlot(BackpackSlot) ? Backpack.GetSlot(BackpackSlot) : FMadItemStack();
	int32 Earned = 0;
	const EMadTradeResult Result = MadFall::Trade::Sell(Backpack, BackpackSlot, Count <= 0 ? Offered.Count : Count, *State, *Trader, Definitions, Earned);
	Inventory->NotifyChanged();
	if (Result == EMadTradeResult::Ok)
	{
		PushMessage(FString::Printf(TEXT("Sold %s for %d %s."), *Definitions.GetItemName(Offered.Item), Earned, *Definitions.GetItemName(Trader->Currency)), 2.0f);
		NotifyQuest(EMadQuestObjectiveType::Trade, Trader->Id, {});
	}
	else
	{
		PushMessage(FString::Printf(TEXT("Cannot sell: %s."), MadFall::Trade::ToString(Result)), 2.0f);
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Trade: sell %s from slot %d: %s (earned %d)."), *Offered.Item.ToString(), BackpackSlot, MadFall::Trade::ToString(Result), Earned);
	return Result;
}

void AMadPlayerCharacter::TickInventoryScreen()
{
	if (bInventoryOpen && OpenTraderVoxel.IsSet())
	{
		// Walking away from the trader closes the screen.
		const FVector TraderAt = (FVector(OpenTraderVoxel.GetValue()) + FVector(0.5, 0.5, 1.0)) * MadFall::VoxelSizeUU;
		if (FVector::DistSquared(TraderAt, GetActorLocation()) > FMath::Square(6.0 * MadFall::VoxelSizeUU))
		{
			CloseInventory();
		}
		return;
	}
	if (!bInventoryOpen || !OpenContainerVoxel.IsSet())
	{
		return;
	}

	const UMadContainerSubsystem* Containers = GetWorld()->GetSubsystem<UMadContainerSubsystem>();
	const FVector Centre = (FVector(OpenContainerVoxel.GetValue()) + FVector(0.5)) * MadFall::VoxelSizeUU;
	const double Reach = (GetReach() + 1.5) * MadFall::VoxelSizeUU;

	// A crate broken by a zombie (its contents spill) or walked away from closes the screen.
	if (Containers == nullptr || !Containers->IsContainer(OpenContainerVoxel.GetValue()))
	{
		CloseInventory();
		PushMessage(TEXT("The container is gone."), 2.0f);
	}
	else if (FVector::DistSquared(Centre, Camera->GetComponentLocation()) > FMath::Square(Reach))
	{
		CloseInventory();
	}
}

FMadInventory* AMadPlayerCharacter::GetOpenContainerInventory() const
{
	if (!OpenContainerVoxel.IsSet())
	{
		return nullptr;
	}
	UMadContainerSubsystem* Containers = GetWorld()->GetSubsystem<UMadContainerSubsystem>();
	FMadContainer* Container = Containers ? Containers->Find(OpenContainerVoxel.GetValue()) : nullptr;
	return Container ? &Container->Contents : nullptr;
}

FMadInventory* AMadPlayerCharacter::GetSideInventory(EMadInventorySide Side) const
{
	switch (Side)
	{
	case EMadInventorySide::Backpack: return &Inventory->GetInventory();
	case EMadInventorySide::Worn:     return &Inventory->GetWorn();
	default:                          return GetOpenContainerInventory();
	}
}

EMadSlotMove AMadPlayerCharacter::MoveItem(EMadInventorySide FromSide, int32 FromSlot, EMadInventorySide ToSide, int32 ToSlot, int32 Count)
{
	FMadInventory* From = GetSideInventory(FromSide);
	FMadInventory* To = GetSideInventory(ToSide);
	if (From == nullptr || To == nullptr)
	{
		return EMadSlotMove::InvalidSlot;
	}

	// A worn slot only takes clothing for that part of the body, whichever way
	// the item travels: dropping a coat on the head slot, or swapping a helmet
	// out of the head slot for a pickaxe, both do nothing.
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	auto FitsWornSlot = [&Definitions](const FMadItemStack& Stack, int32 WornSlot)
	{
		if (Stack.IsEmpty())
		{
			return true;
		}
		const FMadItemDefinition* Item = Definitions.FindItem(Stack.Item);
		return Item != nullptr && Item->bHasWear && MadFall::Wear::GetSlotIndex(Item->Wear.Slot) == WornSlot;
	};
	if (FromSlot < 0 || FromSlot >= From->NumSlots() || ToSlot < 0 || ToSlot >= To->NumSlots())
	{
		return EMadSlotMove::InvalidSlot;
	}
	if ((ToSide == EMadInventorySide::Worn && !FitsWornSlot(From->GetSlot(FromSlot), ToSlot))
		|| (FromSide == EMadInventorySide::Worn && ToSide != EMadInventorySide::Worn && !FitsWornSlot(To->GetSlot(ToSlot), FromSlot)))
	{
		return EMadSlotMove::Nothing;
	}

	const EMadSlotMove Result = MadFall::InventoryOps::MoveSlot(*From, FromSlot, *To, ToSlot, Count, Definitions);
	if (FromSide == EMadInventorySide::Worn || ToSide == EMadInventorySide::Worn)
	{
		Survival->SetWear(Inventory->GetWearTotals());
	}
	Inventory->NotifyChanged();
	return Result;
}

int32 AMadPlayerCharacter::QuickMoveItem(EMadInventorySide Side, int32 Slot)
{
	FMadInventory& Backpack = Inventory->GetInventory();
	FMadInventory* Crate = GetOpenContainerInventory();
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();

	int32 Moved = 0;
	if (Side == EMadInventorySide::Worn)
	{
		// Taking clothes off puts them in the backpack.
		Moved = MadFall::InventoryOps::QuickMove(Inventory->GetWorn(), Slot, Backpack, 0, Backpack.NumSlots() - 1, Definitions);
		Survival->SetWear(Inventory->GetWearTotals());
	}
	else if (Side == EMadInventorySide::Backpack && Crate == nullptr && !Backpack.GetSlot(Slot).IsEmpty()
		&& Definitions.FindItem(Backpack.GetSlot(Slot).Item) != nullptr && Definitions.FindItem(Backpack.GetSlot(Slot).Item)->bHasWear)
	{
		// Shift-clicking clothing puts it on, swapping out what was there.
		const int32 WornSlot = MadFall::Wear::GetSlotIndex(Definitions.FindItem(Backpack.GetSlot(Slot).Item)->Wear.Slot);
		const EMadSlotMove Result = MoveItem(EMadInventorySide::Backpack, Slot, EMadInventorySide::Worn, WornSlot, 1);
		Moved = Result == EMadSlotMove::Moved || Result == EMadSlotMove::Swapped ? 1 : 0;
	}
	else if (Side == EMadInventorySide::Container)
	{
		if (Crate != nullptr)
		{
			Moved = MadFall::InventoryOps::QuickMove(*Crate, Slot, Backpack, 0, Backpack.NumSlots() - 1, Definitions);
		}
	}
	else if (Crate != nullptr)
	{
		Moved = MadFall::InventoryOps::QuickMove(Backpack, Slot, *Crate, 0, Crate->NumSlots() - 1, Definitions);
	}
	else if (Slot < UMadInventoryComponent::HotbarSlots)
	{
		Moved = MadFall::InventoryOps::QuickMove(Backpack, Slot, Backpack, UMadInventoryComponent::HotbarSlots, Backpack.NumSlots() - 1, Definitions);
	}
	else
	{
		Moved = MadFall::InventoryOps::QuickMove(Backpack, Slot, Backpack, 0, UMadInventoryComponent::HotbarSlots - 1, Definitions);
	}

	Inventory->NotifyChanged();
	return Moved;
}

int32 AMadPlayerCharacter::TakeAllFromContainer()
{
	UMadContainerSubsystem* Containers = GetWorld()->GetSubsystem<UMadContainerSubsystem>();
	if (Containers == nullptr || !OpenContainerVoxel.IsSet())
	{
		return 0;
	}
	const int32 Moved = Containers->TakeAll(OpenContainerVoxel.GetValue(), Inventory->GetInventory(), GetGameStage());
	Inventory->NotifyChanged();
	if (Moved > 0)
	{
		PushMessage(FString::Printf(TEXT("Took %d item(s)."), Moved), 2.0f);
	}
	return Moved;
}

void AMadPlayerCharacter::ClickInventorySlot(EMadInventorySide Side, int32 Slot, bool bQuick, bool bHalf)
{
	const FMadInventory* Clicked = GetSideInventory(Side);
	if (Clicked == nullptr || Slot < 0 || Slot >= Clicked->NumSlots())
	{
		return;
	}

	if (bQuick)
	{
		InventoryCursor = FInventoryCursor();
		QuickMoveItem(Side, Slot);
		return;
	}

	if (!InventoryCursor.bActive)
	{
		if (!Clicked->GetSlot(Slot).IsEmpty())
		{
			InventoryCursor.bActive = true;
			InventoryCursor.Side = Side;
			InventoryCursor.Slot = Slot;
			const int32 Stack = Clicked->GetSlot(Slot).Count;
			InventoryCursor.Count = bHalf && Stack > 1 ? (Stack + 1) / 2 : 0;
		}
		return;
	}

	const FInventoryCursor Held = InventoryCursor;
	InventoryCursor = FInventoryCursor();
	if (Held.Side == Side && Held.Slot == Slot)
	{
		return;   // put back where it was
	}

	const FMadInventory* Source = GetSideInventory(Held.Side);
	if (Source == nullptr || Held.Slot >= Source->NumSlots())
	{
		return;
	}
	// An item mod put down on a tool is installed, not swapped. Backpack to
	// backpack only: InstallMod takes the mod from the same inventory as the tool.
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	if (Held.Side == EMadInventorySide::Backpack && Side == EMadInventorySide::Backpack
		&& MadFall::Items::IsModInstallDrop(Source->GetSlot(Held.Slot), Clicked->GetSlot(Slot), Definitions))
	{
		const FName ModItem = Source->GetSlot(Held.Slot).Item;
		const FName ToolItem = Clicked->GetSlot(Slot).Item;
		const EMadItemActionResult Result = MadFall::Items::InstallMod(Inventory->GetInventory(), Slot, ModItem, Definitions);
		Inventory->NotifyChanged();
		PushMessage(Result == EMadItemActionResult::Ok
			? FString::Printf(TEXT("Installed %s on %s."), *Definitions.GetItemName(ModItem), *Definitions.GetItemName(ToolItem))
			: FString::Printf(TEXT("Cannot install: %s."), MadFall::Items::ToString(Result)), 2.0f);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Install %s on %s: %s."), *ModItem.ToString(), *ToolItem.ToString(), MadFall::Items::ToString(Result));
		return;
	}

	// Clamped again here: the stack can have shrunk since it was picked up (eaten
	// from the hotbar, used as a crafting ingredient).
	const int32 SourceCount = Source->GetSlot(Held.Slot).Count;
	const int32 Count = Held.Count > 0 && Held.Count < SourceCount ? Held.Count : 0;
	MoveItem(Held.Side, Held.Slot, Side, Slot, Count);
}

int32 AMadPlayerCharacter::GetHeldCount() const
{
	const FMadInventory* Source = InventoryCursor.bActive ? GetSideInventory(InventoryCursor.Side) : nullptr;
	if (Source == nullptr || InventoryCursor.Slot < 0 || InventoryCursor.Slot >= Source->NumSlots())
	{
		return 0;
	}
	const int32 Stack = Source->GetSlot(InventoryCursor.Slot).Count;
	return InventoryCursor.Count > 0 ? FMath::Min(InventoryCursor.Count, Stack) : Stack;
}

void AMadPlayerCharacter::AdjustHeldCount(int32 Delta)
{
	const FMadInventory* Source = InventoryCursor.bActive ? GetSideInventory(InventoryCursor.Side) : nullptr;
	if (Source == nullptr || InventoryCursor.Slot < 0 || InventoryCursor.Slot >= Source->NumSlots())
	{
		return;
	}
	const int32 Stack = Source->GetSlot(InventoryCursor.Slot).Count;
	const int32 Held = FMath::Clamp(GetHeldCount() + Delta, 1, FMath::Max(1, Stack));
	InventoryCursor.Count = Held >= Stack ? 0 : Held;
}

bool AMadPlayerCharacter::SortInventory(EMadInventorySide Side)
{
	FMadInventory* Sorted = GetSideInventory(Side);
	if (Sorted == nullptr || Side == EMadInventorySide::Worn)
	{
		return false;
	}
	// A held slot would point at whatever lands there after the sort.
	InventoryCursor = FInventoryCursor();
	const int32 First = Side == EMadInventorySide::Backpack ? UMadInventoryComponent::HotbarSlots : 0;
	const bool bChanged = MadFall::InventoryOps::SortRange(*Sorted, First, Sorted->NumSlots() - 1, MadFall::GetGameplayDefinitions());
	if (Side == EMadInventorySide::Backpack)
	{
		Inventory->NotifyChanged();
	}
	UE_LOG(LogMadFallGameplay, Display, TEXT("Sorted the %s%s."), Side == EMadInventorySide::Backpack ? TEXT("backpack") : TEXT("container"),
		bChanged ? TEXT("") : TEXT(" (already in order)"));
	return bChanged;
}

EMadItemActionResult AMadPlayerCharacter::RemoveModFromSlot(int32 Slot)
{
	InventoryCursor = FInventoryCursor();
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadInventory& Backpack = Inventory->GetInventory();
	const FMadItemStack Before = Slot >= 0 && Slot < Backpack.NumSlots() ? Backpack.GetSlot(Slot) : FMadItemStack();
	const EMadItemActionResult Result = MadFall::Items::RemoveMod(Inventory->GetInventory(), Slot, INDEX_NONE, Definitions);
	Inventory->NotifyChanged();
	if (Result == EMadItemActionResult::Ok)
	{
		PushMessage(FString::Printf(TEXT("Took %s off %s."), *Definitions.GetItemName(Before.Mods.Last()), *Definitions.GetItemName(Before.Item)), 2.0f);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Remove mod %s from %s: ok."), *Before.Mods.Last().ToString(), *Before.Item.ToString());
	}
	else
	{
		PushMessage(FString::Printf(TEXT("Cannot remove a mod: %s."), MadFall::Items::ToString(Result)), 2.0f);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Remove mod from slot %d: %s."), Slot, MadFall::Items::ToString(Result));
	}
	return Result;
}

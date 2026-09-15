// Copyright MadFall. All Rights Reserved.

#include "MadAnimal.h"

#include "HAL/IConsoleManager.h"

#include "AIController.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "MadAudioSubsystem.h"
#include "MadDifficulty.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadInventory.h"
#include "MadPickupSubsystem.h"
#include "MadPlayerCharacter.h"
#include "MadQuadrupedRig.h"
#include "MadSurvivorComponents.h"
#include "MadTraps.h"
#include "MadVoxelRaycast.h"
#include "MadVoxelWorldSubsystem.h"

int32 AMadAnimal::TotalKills = 0;
int32 AMadAnimal::TotalPlayerHits = 0;

namespace
{
	constexpr float AnimalStepArrivalCm = 30.0f;
	constexpr int32 FleeDistanceVoxels = 12;
	constexpr int32 GrazeRadiusVoxels = 6;

	TAutoConsoleVariable<int32> CVarStroll(
		TEXT("mad.animals.Stroll"),
		1,
		TEXT("1: grazing animals now and then stroll a few voxels. 0: they stand and eat - for tests that aim at one, which a stroll between aim and impact turns into a miss."),
		ECVF_Default);
	constexpr int32 PathNodes = 600;



	/** Capsule that fits the rig: as wide as the body is thick, as tall as the animal stands. */
	void ComputeCapsule(const FMadAnimalDefinition& Definition, float& OutRadius, float& OutHalfHeight)
	{
		const FVector Body = Definition.BodySize * Definition.Scale;
		const float Standing = UMadQuadrupedRigComponent::GetStandingHeight(Body, Definition.LegLength * Definition.Scale);
		// A capsule is round; a long animal gets a radius between its width and
		// half its length, so it fits through a one-voxel gap and still blocks a swing.
		OutRadius = FMath::Clamp(static_cast<float>(FMath::Max(Body.Y * 0.5, Body.X * 0.3)), 10.0f, 45.0f);
		OutHalfHeight = FMath::Max(OutRadius, Standing * 0.5f);
	}
}

EMadAnimalState MadFall::Animals::Decide(const FMadAnimalDefinition& Definition, const FMadAnimalSenses& Senses)
{
	const bool bHurt = Senses.SecondsSinceHurt >= 0.0f && Senses.SecondsSinceHurt < HurtMemorySeconds;
	if (!Senses.bPlayerPresent)
	{
		return EMadAnimalState::Graze;
	}

	// An animal that cannot hurt anyone runs, whatever its temperament says.
	const bool bCanFight = Definition.AttackDamage > 0.0f && Definition.Behaviour != EMadAnimalBehaviour::Skittish;
	auto Fight = [&Senses]()
	{
		return Senses.DistanceVoxels <= AttackReachVoxels ? EMadAnimalState::Attack : EMadAnimalState::Chase;
	};

	if (!bCanFight)
	{
		const bool bClose = Senses.bSeesPlayer && Senses.DistanceVoxels <= Definition.FleeRange;
		const bool bHeard = Senses.bHearsPlayer && Senses.DistanceVoxels <= Definition.HearingRange;
		return bHurt || bClose || bHeard ? EMadAnimalState::Flee : EMadAnimalState::Graze;
	}

	// A fighter gives up once the survivor is well away.
	const bool bInRange = Senses.DistanceVoxels <= Definition.SightRange * 1.5f;
	if (bHurt && bInRange)
	{
		return Fight();
	}
	if (Definition.Behaviour == EMadAnimalBehaviour::Aggressive && Senses.bSeesPlayer && Senses.DistanceVoxels <= Definition.SightRange)
	{
		return Fight();
	}
	return EMadAnimalState::Graze;
}

FIntVector MadFall::Animals::PickFleeGoal(const FIntVector& Feet, const FIntVector& Threat, int32 Distance, float Jitter)
{
	FVector2D Away(static_cast<float>(Feet.X - Threat.X), static_cast<float>(Feet.Y - Threat.Y));
	if (Away.IsNearlyZero())
	{
		Away = FVector2D(1.0f, 0.0f);
	}
	Away.Normalize();
	const FVector2D Side(-Away.Y, Away.X);
	const FVector2D Direction = (Away + Side * FMath::Clamp(Jitter, -1.0f, 1.0f) * 0.5f).GetSafeNormal();
	return Feet + FIntVector(FMath::RoundToInt32(Direction.X * Distance), FMath::RoundToInt32(Direction.Y * Distance), 0);
}

AMadAnimal::AMadAnimal()
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(30.0f, 45.0f);

	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
	AIControllerClass = AAIController::StaticClass();

	bUseControllerRotationYaw = false;
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.0, 540.0, 0.0);
	Movement->MaxWalkSpeed = 120.0f;
	Movement->MaxStepHeight = 45.0f;
	Movement->JumpZVelocity = 560.0f;

	Body = CreateDefaultSubobject<UMadQuadrupedRigComponent>(TEXT("Body"));
	Body->SetupAttachment(GetCapsuleComponent());
	Body->SetRelativeLocation(FVector(0.0, 0.0, -45.0));
}

void AMadAnimal::BeginPlay()
{
	Super::BeginPlay();
	Random.Initialize(static_cast<int32>(GetUniqueID() * 2654435761u));
	LastProgressLocation = GetActorLocation();
	ThinkTimer = Random.FRandRange(0.0f, 0.3f);
	GrazeTimer = Random.FRandRange(0.0f, 4.0f);
}

void AMadAnimal::InitialiseFromDefinition(const FMadAnimalDefinition& InDefinition)
{
	Definition = InDefinition;
	Health = Definition.Health;

	float Radius = 30.0f;
	float HalfHeight = 45.0f;
	ComputeCapsule(Definition, Radius, HalfHeight);
	GetCapsuleComponent()->SetCapsuleSize(Radius, HalfHeight);
	GetCharacterMovement()->MaxWalkSpeed = Definition.WalkSpeed * 100.0f;

	Body->SetRelativeLocation(FVector(0.0, 0.0, -HalfHeight));
	Body->SetShape(Definition.BodySize * Definition.Scale, Definition.LegLength * Definition.Scale, Definition.NeckLength * Definition.Scale);
	Body->SetColour(Definition.Tint);
	Body->SetGallopSpeed(Definition.RunSpeed * 100.0f);
	Body->SetModel(Definition.Model);
}

FIntVector AMadAnimal::GetFeetVoxel() const
{
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	return MadFall::WorldCmToVoxel(GetActorLocation() - FVector(0.0, 0.0, HalfHeight - 5.0));
}

void AMadAnimal::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	MAD_FRAME_SCOPE(Zombies);

	if (State == EMadAnimalState::Dead)
	{
		DespawnTimer -= DeltaSeconds;
		if (DespawnTimer <= 0.0f)
		{
			Destroy();
		}
		return;
	}

	AttackCooldown -= DeltaSeconds;
	RepathTimer -= DeltaSeconds;
	GrazeTimer -= DeltaSeconds;
	ThinkTimer -= DeltaSeconds;
	if (ThinkTimer <= 0.0f)
	{
		ThinkTimer = 0.3f + Random.FRandRange(0.0f, 0.1f);
		Think();
	}

	if (State == EMadAnimalState::Graze || State == EMadAnimalState::Flee || State == EMadAnimalState::Chase)
	{
		FollowPath(DeltaSeconds);
	}
}

FMadAnimalSenses AMadAnimal::Sense() const
{
	FMadAnimalSenses Senses;
	const UWorld* World = GetWorld();
	const AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(World);
	if (Player == nullptr || Player->IsDown())
	{
		return Senses;
	}

	Senses.bPlayerPresent = true;
	Senses.DistanceVoxels = static_cast<float>(FVector::Dist(Player->GetActorLocation(), GetActorLocation()) / MadFall::VoxelSizeUU);
	if (LastHurtTime >= 0.0)
	{
		Senses.SecondsSinceHurt = static_cast<float>(World->GetTimeSeconds() - LastHurtTime);
	}

	if (Senses.DistanceVoxels <= FMath::Max(Definition.SightRange, Definition.FleeRange))
	{
		if (const UMadVoxelWorldSubsystem* VoxelWorld = World->GetSubsystem<UMadVoxelWorldSubsystem>())
		{
			const FVector Eye = (GetActorLocation() + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.6)) / MadFall::VoxelSizeUU;
			const FVector PlayerEye = (Player->GetActorLocation() + FVector(0.0, 0.0, 60.0)) / MadFall::VoxelSizeUU;
			FMadVoxelHit Hit;
			Senses.bSeesPlayer = !MadFall::VoxelRaycast(Eye, PlayerEye - Eye, (PlayerEye - Eye).Size() - 0.5f,
				[VoxelWorld](const FIntVector& V) { return VoxelWorld->GetVoxel(V.X, V.Y, V.Z).IsSolid(); }, Hit);
		}
	}
	Senses.bHearsPlayer = Senses.DistanceVoxels <= Definition.HearingRange && Player->IsNoisy(1.5);
	return Senses;
}

void AMadAnimal::Think()
{
	const FMadAnimalSenses Senses = Sense();
	const EMadAnimalState Next = MadFall::Animals::Decide(Definition, Senses);
	const EMadAnimalState Previous = State;
	State = Next;

	AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(GetWorld());
	const bool bRunning = Next == EMadAnimalState::Flee || Next == EMadAnimalState::Chase || Next == EMadAnimalState::Attack;
	float TrapSlow = 1.0f;
	if (const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>())
	{
		const FMadTrapContact Contact = MadFall::Traps::FindContact(*VoxelWorld, GetActorLocation(), GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		if (Contact.IsValid())
		{
			TrapSlow = Contact.Block->TrapSlow;
			if (Contact.Block->TrapDamage > 0.0f && GetWorld()->GetTimeSeconds() >= NextTrapHitTime)
			{
				NextTrapHitTime = GetWorld()->GetTimeSeconds() + Contact.Block->TrapSeconds;
				++MadFall::Traps::TotalHits();
				ReceiveHit(Contact.Block->TrapDamage, FName(TEXT("madfall:pierce")), nullptr);
			}
		}
	}
	GetCharacterMovement()->MaxWalkSpeed = (bRunning ? Definition.RunSpeed : Definition.WalkSpeed) * 100.0f * TrapSlow;

	switch (Next)
	{
	case EMadAnimalState::Flee:
		if (Player != nullptr && (Previous != EMadAnimalState::Flee || StepIndex >= Path.Steps.Num() || RepathTimer <= 0.0f))
		{
			RequestPath(MadFall::Animals::PickFleeGoal(GetFeetVoxel(), Player->GetFeetVoxel(), FleeDistanceVoxels, Random.FRandRange(-1.0f, 1.0f)));
			if (Previous != EMadAnimalState::Flee)
			{
				UE_LOG(LogMadFallGameplay, Verbose, TEXT("%s bolts from the survivor."), *Definition.Id.ToString());
			}
		}
		break;

	case EMadAnimalState::Chase:
		if (Player != nullptr)
		{
			const FIntVector Goal = Player->GetFeetVoxel();
			const bool bGoalMoved = FMath::Abs(Goal.X - PathGoal.X) + FMath::Abs(Goal.Y - PathGoal.Y) + FMath::Abs(Goal.Z - PathGoal.Z) >= 2;
			if (RepathTimer <= 0.0f || bGoalMoved || StepIndex >= Path.Steps.Num())
			{
				RequestPath(Goal);
			}
		}
		break;

	case EMadAnimalState::Attack:
		if (Player != nullptr)
		{
			TryAttackPlayer(*Player);
		}
		break;

	case EMadAnimalState::Graze:
	default:
		if (Previous != EMadAnimalState::Graze)
		{
			// Calm again: stop where it is rather than finish a flight path.
			Path = FMadVoxelPath();
			StepIndex = 0;
			GrazeTimer = Random.FRandRange(2.0f, 5.0f);
		}
		if (StepIndex >= Path.Steps.Num() && GrazeTimer <= 0.0f)
		{
			// Mostly stand and eat; now and then stroll a few voxels.
			if (Random.FRand() < 0.4f && CVarStroll.GetValueOnGameThread() != 0)
			{
				const FIntVector Feet = GetFeetVoxel();
				RequestPath(Feet + FIntVector(Random.RandRange(-GrazeRadiusVoxels, GrazeRadiusVoxels), Random.RandRange(-GrazeRadiusVoxels, GrazeRadiusVoxels), 0));
			}
			GrazeTimer = Random.FRandRange(4.0f, 10.0f);
		}
		break;
	}

	Body->SetGrazing(State == EMadAnimalState::Graze && StepIndex >= Path.Steps.Num());
}

void AMadAnimal::RequestPath(const FIntVector& Goal)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return;
	}

	// Unloaded space reads as solid so no path leads off the loaded world.
	auto GetVoxel = [VoxelWorld](const FIntVector& V)
	{
		if (!VoxelWorld->IsVoxelLoaded(V.X, V.Y, V.Z))
		{
			FMadVoxel Rock;
			Rock.BlockTypeID = MadFall::BlockTypeUnresolved;
			Rock.Density = 255;
			Rock.Damage = 0;
			Rock.Rotation = 0;
			Rock.Flags = 0;
			return Rock;
		}
		return MadFall::Traps::ForPathing(VoxelWorld->GetVoxel(V.X, V.Y, V.Z));
	};
	auto CannotBreak = [](const FIntVector&, const FMadVoxel&) { return -1.0f; };

	FMadPathSettings Settings;
	Settings.MaxNodes = PathNodes;
	Settings.bAllowDigging = false;

	MadFall::Pathfinding::FindPath(GetFeetVoxel(), Goal, Settings, GetVoxel, CannotBreak, Path);
	StepIndex = 0;
	PathGoal = Goal;
	RepathTimer = 1.5f + Random.FRandRange(0.0f, 1.0f);
}

void AMadAnimal::FollowPath(float DeltaSeconds)
{
	if (StepIndex >= Path.Steps.Num())
	{
		return;
	}

	const FMadPathStep& Step = Path.Steps[StepIndex];
	const FVector StepCentre((Step.Feet.X + 0.5) * MadFall::VoxelSizeUU, (Step.Feet.Y + 0.5) * MadFall::VoxelSizeUU, GetActorLocation().Z);
	const FVector ToStep = StepCentre - GetActorLocation();
	const FIntVector Feet = GetFeetVoxel();

	if (ToStep.Size2D() < AnimalStepArrivalCm && FMath::Abs(Feet.Z - Step.Feet.Z) <= 1)
	{
		++StepIndex;
		StuckTimer = 0.0f;
		LastProgressLocation = GetActorLocation();
		return;
	}

	AddMovementInput(ToStep.GetSafeNormal2D(), 1.0f);
	if (Step.Feet.Z > Feet.Z && GetCharacterMovement()->IsMovingOnGround())
	{
		Jump();
	}

	StuckTimer += DeltaSeconds;
	if (StuckTimer > 1.5f)
	{
		if (FVector::Dist2D(GetActorLocation(), LastProgressLocation) < 25.0f)
		{
			// Blocked by something the path did not know about: give up on this path.
			Jump();
			Path = FMadVoxelPath();
			StepIndex = 0;
			RepathTimer = 0.0f;
		}
		StuckTimer = 0.0f;
		LastProgressLocation = GetActorLocation();
	}
}

void AMadAnimal::TryAttackPlayer(AMadPlayerCharacter& Player)
{
	const FVector Facing = (Player.GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
	if (!Facing.IsNearlyZero())
	{
		SetActorRotation(Facing.Rotation());
	}
	if (AttackCooldown > 0.0f)
	{
		return;
	}
	AttackCooldown = Definition.AttackSeconds;

	Body->PlayAttack();
	Player.GetSurvival()->ApplyAttackDamage(Definition.AttackDamage * MadFall::Difficulty::GetWorldScale(GetWorld(), FName(TEXT("animal_damage"))));
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayAt(EMadSound::FleshHit, Player.GetActorLocation());
	}
	++TotalPlayerHits;
	UE_LOG(LogMadFallGameplay, Log, TEXT("%s hit the survivor for %.0f."), *Definition.Id.ToString(), Definition.AttackDamage);
}

bool AMadAnimal::ReceiveHit(float Amount, FName DamageType, AActor* Attacker)
{
	if (IsDead() || Amount <= 0.0f)
	{
		return false;
	}

	Body->PlayHit();
	Health = FMath::Max(0.0f, Health - Amount);
	if (Cast<AMadPlayerCharacter>(Attacker) != nullptr)
	{
		LastHurtTime = GetWorld()->GetTimeSeconds();
		// React this frame, not on the next think: a hit deer is already running.
		ThinkTimer = 0.0f;
	}

	if (Health <= 0.0f)
	{
		Die(Attacker);
		return true;
	}
	return false;
}

void AMadAnimal::Die(AActor* Killer)
{
	State = EMadAnimalState::Dead;
	DespawnTimer = 8.0f;
	++TotalKills;

	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetCharacterMovement()->DisableMovement();
	Body->PlayDeath();

	if (AMadPlayerCharacter* Player = Cast<AMadPlayerCharacter>(Killer))
	{
		Player->AddExperience(Definition.Experience);
		Player->NotifyQuest(EMadQuestObjectiveType::KillAnimal, Definition.Id, Definition.Tags);

		const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
		if (const FMadLootTableDefinition* Table = Definitions.FindLootTable(Definition.LootTable))
		{
			FRandomStream LootRandom(static_cast<int32>(GetUniqueID() * 7919));
			TArray<FMadItemStack> Drops;
			MadFall::Loot::Roll(*Table, Definitions, FMadLootContext{ 1, Player->GetGameStage() }, LootRandom, Drops);
			if (UMadPickupSubsystem* Pickups = GetWorld()->GetSubsystem<UMadPickupSubsystem>())
			{
				// At the feet, so the bag lands where the carcass lies.
				Pickups->Drop(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.5), Drops);
			}
		}
	}

	UE_LOG(LogMadFallGameplay, Display, TEXT("Animal %s died at %s."), *Definition.Id.ToString(), *GetFeetVoxel().ToString());
}

FString AMadAnimal::DescribeStatus() const
{
	static const TCHAR* StateNames[] = { TEXT("idle"), TEXT("graze"), TEXT("flee"), TEXT("chase"), TEXT("attack"), TEXT("dead") };
	const AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(GetWorld());
	const float Distance = Player ? static_cast<float>(FVector::Dist(Player->GetActorLocation(), GetActorLocation()) / MadFall::VoxelSizeUU) : -1.0f;
	return FString::Printf(TEXT("%s at %s: %s, %.0f hp, %.1f voxels from the survivor"),
		*Definition.Id.ToString(), *GetFeetVoxel().ToString(), StateNames[static_cast<int32>(State)], Health, Distance);
}

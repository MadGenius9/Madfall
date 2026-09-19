// Copyright MadFall. All Rights Reserved.

#include "MadZombie.h"

#include "MadFrameBudget.h"
#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadBlockDamage.h"
#include "MadBlockRegistry.h"
#include "MadDifficulty.h"
#include "MadFallGameplay.h"
#include "MadFallStats.h"
#include "MadInventory.h"
#include "MadHordeSubsystem.h"
#include "MadPickupSubsystem.h"
#include "MadProjectile.h"
#include "MadTraps.h"
#include "MadPlayerCharacter.h"
#include "MadStructuralSubsystem.h"
#include "MadSurvivalAttributeSet.h"
#include "MadAudioSubsystem.h"
#include "MadHumanoidRig.h"
#include "MadSurvivorComponents.h"
#include "MadVoxelRaycast.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"

namespace
{
	/**
	 * How much slower a body moves through water: waist deep is a wade, over
	 * the head is worse. A horde crossing a moat should arrive late and strung
	 * out rather than as a wall, which is the whole point of digging one.
	 */
	/**
	 * Out of its depth: the voxel at the head is liquid too, so the thing is
	 * fully under rather than wading.
	 *
	 * WHY IT MATTERS: water already costs a zombie pathing and half its speed,
	 * which makes a shallow moat a delay. Nothing made a deep one a decision.
	 * A shambler that is completely submerged cannot swing at anything and
	 * turns back for the shallows, so digging three voxels down buys a wall
	 * while digging one buys time - and the horde still comes round.
	 */
	bool IsOutOfDepth(const UWorld* World, const FIntVector& Feet)
	{
		const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
		if (VoxelWorld == nullptr)
		{
			return false;
		}
		auto IsLiquid = [VoxelWorld](const FIntVector& V)
		{
			const FMadBlockDefinitionData* Block = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(
				VoxelWorld->GetVoxel(V.X, V.Y, V.Z).BlockTypeID);
			return Block != nullptr && Block->bLiquid;
		};
		return IsLiquid(Feet) && IsLiquid(Feet + FIntVector(0, 0, 1));
	}

	float WaterSlowFactor(const UWorld* World, const FIntVector& Feet)
	{
		const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
		if (VoxelWorld == nullptr)
		{
			return 1.0f;
		}
		auto IsLiquid = [VoxelWorld](const FIntVector& V)
		{
			const FMadBlockDefinitionData* Block = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(
				VoxelWorld->GetVoxel(V.X, V.Y, V.Z).BlockTypeID);
			return Block != nullptr && Block->bLiquid;
		};
		if (!IsLiquid(Feet))
		{
			return 1.0f;
		}
		return IsLiquid(Feet + FIntVector(0, 0, 1)) ? 0.45f : 0.6f;
	}
}

DECLARE_CYCLE_STAT(TEXT("Zombie Think"), STAT_MadZombieThink, STATGROUP_MadFallStructural);

int32 AMadZombie::TotalBlocksHit = 0;
int32 AMadZombie::TotalPlayerHits = 0;
int32 AMadZombie::TotalPaths = 0;
int32 AMadZombie::TotalKills = 0;
int32 AMadZombie::TotalUndermines = 0;
int32 AMadZombie::TotalBreaches = 0;
int32 AMadZombie::TotalSpits = 0;
int32 AMadZombie::TotalScreams = 0;
int32 AMadZombie::TotalClimbs = 0;
int32 AMadZombie::TotalDigDowns = 0;

namespace
{
	/** Unloaded space reads as solid rock so paths never lead into it. */
	FMadVoxel GetVoxelForPathing(const UMadVoxelWorldSubsystem& VoxelWorld, const FIntVector& V)
	{
		if (!VoxelWorld.IsVoxelLoaded(V.X, V.Y, V.Z))
		{
			FMadVoxel Rock;
			Rock.BlockTypeID = MadFall::BlockTypeUnresolved;
			Rock.Density = 255;
			Rock.Damage = 0;
			Rock.Rotation = 0;
			Rock.Flags = 0;
			return Rock;
		}
		// Traps, open doors and ladders are open space: zombies walk into them.
		return MadFall::Traps::ForPathing(VoxelWorld.GetVoxel(V.X, V.Y, V.Z));
	}

	/** Seconds this zombie needs to break a voxel, or -1 if it cannot. */
	float GetBreakSeconds(const FMadZombieDefinition& Definition, const FMadVoxel& Voxel)
	{
		const FMadBlockDefinitionData* Def = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(Voxel.BlockTypeID);
		const float DamagePerSecond = Definition.BlockDamage / FMath::Max(0.1f, Definition.AttackSeconds);
		if (Def == nullptr || DamagePerSecond <= 0.0f)
		{
			return -1.0f;
		}
		const float Resistance = MadFall::BlockDamage::GetResistance(*Def, Definition.DamageType);
		if (Resistance <= 0.0f)
		{
			return -1.0f;
		}
		return Def->Hardness * (1.0f - Voxel.Damage / 255.0f) / (DamagePerSecond * Resistance);
	}
}

namespace
{
	TAutoConsoleVariable<int32> CVarPathNodes(
		TEXT("mad.ai.PathNodes"),
		1500,
		TEXT("A* node budget for one zombie path request."),
		ECVF_Default);

	constexpr float AttackReachCm = 150.0f;
	constexpr float ZombieStepArrivalCm = 30.0f;


}

AMadZombie::AMadZombie()
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(34.0f, 90.0f);

	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
	AIControllerClass = AAIController::StaticClass();

	bUseControllerRotationYaw = false;
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.0, 360.0, 0.0);
	Movement->MaxWalkSpeed = 140.0f;
	Movement->MaxStepHeight = 60.0f;
	// Climbing is flying up a column: slow, so a climber on a wall is a target, not a blur.
	Movement->MaxFlySpeed = 140.0f;
	Movement->BrakingDecelerationFlying = 800.0f;
	Movement->JumpZVelocity = 520.0f;
	// Crowd avoidance (RVO): without it a horde converging on the survivor
	// packed into one column of overlapping capsules, a lump rather than a
	// crowd. RVO bends each walker's velocity around its neighbours. Cost: the
	// engine's avoidance manager, a grid query per zombie per update. Walking
	// only - a climber on a wall is not avoiding anything.
	Movement->bUseRVOAvoidance = true;
	Movement->AvoidanceConsiderationRadius = 160.0f;
	Movement->AvoidanceWeight = 0.5f;

	Body = CreateDefaultSubobject<UMadHumanoidRigComponent>(TEXT("Body"));
	Body->SetupAttachment(GetCapsuleComponent());
	// The rig's origin is its feet: the bottom of the 90 cm half-height capsule.
	Body->SetRelativeLocation(FVector(0.0, 0.0, -90.0));

	AbilitySystem = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystem"));
	Attributes = CreateDefaultSubobject<UMadSurvivalAttributeSet>(TEXT("Attributes"));
}

void AMadZombie::BeginPlay()
{
	Super::BeginPlay();

	AbilitySystem->InitAbilityActorInfo(this, this);

	Random.Initialize(static_cast<int32>(GetUniqueID()));
	LastProgressLocation = GetActorLocation();
	ThinkTimer = Random.FRandRange(0.0f, 0.3f);
}

void AMadZombie::InitialiseFromDefinition(const FMadZombieDefinition& InDefinition, bool bInHorde)
{
	Definition = InDefinition;
	VariantId = InDefinition.Id;
	bHorde = bInHorde;

	const float Health = Definition.Health * MadFall::Difficulty::GetWorldScale(GetWorld(), FName(TEXT("zombie_health")));
	AbilitySystem->SetNumericAttributeBase(UMadSurvivalAttributeSet::GetMaxHealthAttribute(), Health);
	AbilitySystem->SetNumericAttributeBase(UMadSurvivalAttributeSet::GetHealthAttribute(), Health);

	SetActorScale3D(FVector(Definition.Scale));
	// The definition's tint is the skin. Clothes are what people wore, faded and
	// picked per zombie, so a horde is a crowd rather than a row of copies: the
	// first version derived them from the skin, and every zombie wore the same
	// green-black. Linear colours, dark, since dirt and blood are drawn on top.
	if (Body != nullptr)
	{
		static const FLinearColor Shirts[] = {
			FLinearColor(0.05f, 0.08f, 0.16f),   // denim
			FLinearColor(0.16f, 0.15f, 0.13f),   // grey-white, filthy
			FLinearColor(0.14f, 0.03f, 0.03f),   // red flannel
			FLinearColor(0.07f, 0.08f, 0.04f),   // olive
			FLinearColor(0.10f, 0.06f, 0.03f),   // brown
			FLinearColor(0.03f, 0.05f, 0.09f),   // navy
			FLinearColor(0.12f, 0.10f, 0.03f),   // mustard
		};
		static const FLinearColor Trousers[] = {
			FLinearColor(0.03f, 0.05f, 0.11f),   // jeans
			FLinearColor(0.05f, 0.045f, 0.04f),  // dark grey
			FLinearColor(0.09f, 0.07f, 0.04f),   // khaki
			FLinearColor(0.02f, 0.02f, 0.02f),   // black
		};
		static const FLinearColor Hair[] = {
			FLinearColor(0.02f, 0.015f, 0.01f),
			FLinearColor(0.07f, 0.04f, 0.02f),
			FLinearColor(0.15f, 0.11f, 0.05f),
			FLinearColor(0.08f, 0.08f, 0.08f),
		};
		const uint32 Hash = HashCombine(GetTypeHash(GetUniqueID()), GetTypeHash(Definition.Id));
		Body->SetColours(Definition.Tint, Shirts[Hash % UE_ARRAY_COUNT(Shirts)], Trousers[(Hash >> 8) % UE_ARRAY_COUNT(Trousers)],
			Hair[(Hash >> 16) % UE_ARRAY_COUNT(Hair)]);
		Body->SetSeed(static_cast<int32>(Hash & 0x7FFFFFFF));
	}
	GetCharacterMovement()->MaxWalkSpeed = (bHorde ? Definition.RunSpeed : Definition.WalkSpeed) * 100.0f;
}

float AMadZombie::GetHealth() const
{
	return Attributes ? Attributes->GetHealth() : 0.0f;
}

FIntVector AMadZombie::GetFeetVoxel() const
{
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	return MadFall::WorldCmToVoxel(GetActorLocation() - FVector(0.0, 0.0, HalfHeight - 5.0));
}

// ===========================================================================
// Tick
// ===========================================================================

void AMadZombie::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	MAD_FRAME_SCOPE(Zombies);

	// Gone through the floor of the world: a body that falls past the bedrock
	// keeps falling, keeps ticking and keeps its place under the spawn cap
	// forever. Nothing put it there in normal play - it takes a hole with no
	// bottom, which a carved-out moat or an unloaded chunk can make - but the
	// cost of missing it is a slot lost for the rest of the session.
	if (GetActorLocation().Z < static_cast<double>(MadFall::WorldMinZ - 8) * MadFall::VoxelSizeUU)
	{
		UE_LOG(LogMadFallGameplay, Verbose, TEXT("%s fell out of the world at %s; removing it."),
			*GetName(), *GetActorLocation().ToCompactString());
		Destroy();
		return;
	}
	if (State == EMadZombieState::Dead)
	{
		DespawnTimer -= DeltaSeconds;
		if (DespawnTimer <= 0.0f)
		{
			Destroy();
		}
		return;
	}

	AttackCooldown -= DeltaSeconds;
	RangedCooldown -= DeltaSeconds;
	ScreamTimer -= DeltaSeconds;
	RepathTimer -= DeltaSeconds;
	ThinkTimer -= DeltaSeconds;

	if (ThinkTimer <= 0.0f)
	{
		ThinkTimer = 0.25f + Random.FRandRange(0.0f, 0.1f);
		Think();
	}

	// Now and then, a groan - more often when it has seen you.
	GroanTimer -= DeltaSeconds;
	if (GroanTimer <= 0.0f)
	{
		const bool bHunting = State == EMadZombieState::Chase || State == EMadZombieState::Attack || State == EMadZombieState::Dig;
		GroanTimer = bHunting ? Random.FRandRange(3.5f, 7.0f) : Random.FRandRange(8.0f, 16.0f);
		if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
		{
			Audio->PlayAt(EMadSound::ZombieGroan, GetActorLocation() + FVector(0.0, 0.0, 60.0), bHunting ? 1.0f : 0.6f);
		}
	}

	if (State == EMadZombieState::Chase || State == EMadZombieState::Wander || State == EMadZombieState::Dig)
	{
		FollowPath(DeltaSeconds);
	}
	else if (GetCharacterMovement()->MovementMode == MOVE_Flying)
	{
		// Stopped mid-climb to attack or idle: nothing holds a zombie on a wall.
		GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	}
}

void AMadZombie::Sense()
{
	AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(GetWorld());
	if (Player == nullptr || Player->IsDown())
	{
		Target.Reset();
		return;
	}

	const FVector Offset = Player->GetActorLocation() - GetActorLocation();
	const float DistanceVoxels = Offset.Size() / MadFall::VoxelSizeUU;

	bool bAware = bHorde;
	bSeesTarget = false;

	if (DistanceVoxels <= Definition.SightRange)
	{
		// Line of sight through the voxel data. Everything solid blocks it,
		// including glass, until blocks carry a see-through flag.
		const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
		if (VoxelWorld != nullptr)
		{
			const FVector Eye = (GetActorLocation() + FVector(0.0, 0.0, 70.0)) / MadFall::VoxelSizeUU;
			const FVector PlayerEye = (Player->GetActorLocation() + FVector(0.0, 0.0, 70.0)) / MadFall::VoxelSizeUU;
			FMadVoxelHit Hit;
			const bool bBlocked = MadFall::VoxelRaycast(Eye, PlayerEye - Eye, (PlayerEye - Eye).Size() - 0.5f,
				[VoxelWorld](const FIntVector& V) { return VoxelWorld->GetVoxel(V.X, V.Y, V.Z).IsSolid(); }, Hit);
			bSeesTarget = !bBlocked;
			bAware = bAware || bSeesTarget;
		}
	}

	if (!bAware && DistanceVoxels <= Definition.HearingRange && Player->IsNoisy(1.5))
	{
		bAware = true;
	}

	if (bAware)
	{
		Target = Player;
		LastKnownTarget = Player->GetFeetVoxel();
		bHasLastKnown = true;
	}
	else if (Target.IsValid() && DistanceVoxels > Definition.SightRange * 1.5f)
	{
		// Out of range and unseen: forget the player, keep heading for where they were.
		Target.Reset();
	}
}

void AMadZombie::Think()
{
	SCOPE_CYCLE_COUNTER(STAT_MadZombieThink);

	Sense();

	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	const bool bNight = Clock != nullptr && Clock->IsNight();
	const bool bRunning = bHorde || (bNight && Target.IsValid());
	UpdateTrap();
	GetCharacterMovement()->MaxWalkSpeed = (bRunning ? Definition.RunSpeed : Definition.WalkSpeed) * 100.0f * TrapSlow
		* WaterSlowFactor(GetWorld(), GetFeetVoxel());

	if (AMadPlayerCharacter* Player = Target.Get())
	{
		const FVector Offset = Player->GetActorLocation() - GetActorLocation();
		const float DistanceVoxels = static_cast<float>(Offset.Size() / MadFall::VoxelSizeUU);
		if (Definition.HasScream() && bSeesTarget && ScreamTimer <= 0.0f)
		{
			Scream(*Player);
		}
		// A spitter stands off and spits rather than walking into reach.
		if (Definition.HasRanged() && TrySpit(*Player, DistanceVoxels))
		{
			State = EMadZombieState::Attack;
			return;
		}
		if (Offset.Size2D() <= AttackReachCm * GetActorScale3D().X && FMath::Abs(Offset.Z) < 180.0f)
		{
			State = EMadZombieState::Attack;
			// Not while under: a swing from the bottom of a moat reaches nothing.
			if (!IsOutOfDepth(GetWorld(), GetFeetVoxel()))
			{
				TryAttackPlayer();
			}
			return;
		}

		if (State != EMadZombieState::Dig)
		{
			State = EMadZombieState::Chase;
		}

		// Out of its depth: wade back to the nearest ground rather than press on
		// across the bottom of the moat. The dry way round still exists - water
		// costs a path, it does not forbid one - so a horde arrives late and by
		// the bank instead of walking through the water as if it were a field.
		if (IsOutOfDepth(GetWorld(), GetFeetVoxel()))
		{
			const FIntVector Feet = GetFeetVoxel();
			const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
			FIntVector Shallow = Feet;
			float Best = MAX_FLT;
			for (int32 Radius = 1; Radius <= 6 && Best == MAX_FLT; ++Radius)
			{
				for (int32 DY = -Radius; DY <= Radius; ++DY)
				{
					for (int32 DX = -Radius; DX <= Radius; ++DX)
					{
						if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != Radius)
						{
							continue;
						}
						const FIntVector At(Feet.X + DX, Feet.Y + DY, Feet.Z);
						if (VoxelWorld == nullptr || IsOutOfDepth(GetWorld(), At))
						{
							continue;
						}
						// Nearest to the survivor among the shallow neighbours, so a
						// zombie that can get out on their side does.
						const float Score = static_cast<float>(FVector::Dist2D(
							FVector(At) * MadFall::VoxelSizeUU, Player->GetActorLocation()));
						if (Score < Best)
						{
							Best = Score;
							Shallow = At;
						}
					}
				}
			}
			if (Shallow != Feet)
			{
				AddMovementInput((FVector(Shallow - Feet)).GetSafeNormal2D(), 1.0f);
				return;
			}
		}

		const FIntVector Goal = Player->GetFeetVoxel();
		const bool bGoalMoved = FMath::Abs(Goal.X - PathGoal.X) + FMath::Abs(Goal.Y - PathGoal.Y) + FMath::Abs(Goal.Z - PathGoal.Z) >= 3;

		// A path that ran out is worth replacing at once. A path that never
		// existed is not: StepIndex >= Steps.Num() is trivially true for an
		// empty path, so a zombie whose target it cannot reach - across a deep
		// moat, inside a sealed room - used to run a full search every time it
		// thought, for as long as it stood there (measured: 158 searches in 50
		// seconds for one zombie at a moat's edge, against 3 for one walking
		// the same distance over land). Requiring steps to exist puts that case
		// back behind RepathTimer, where the rest of the repathing already is.
		const bool bPathSpent = Path.Steps.Num() > 0 && StepIndex >= Path.Steps.Num();
		if (RepathTimer <= 0.0f || bGoalMoved || bPathSpent)
		{
			RequestPath(Goal, /*bAllowDigging*/ true);

			// No path gets within reach and the target is above: if their support
			// is within reach from here, go for it instead of walking a partial
			// path that ends in the same place.
			if (!Path.bReachesGoal && !TryUndermine(Goal) && Path.Steps.Num() <= 1)
			{
				// Already standing where the best partial path ends - against the
				// wall between it and the target: break through it.
				TryBreach(Goal);
			}
		}
		return;
	}

	if (bHasLastKnown)
	{
		State = EMadZombieState::Chase;
		const FIntVector Feet = GetFeetVoxel();
		if (FMath::Abs(Feet.X - LastKnownTarget.X) + FMath::Abs(Feet.Y - LastKnownTarget.Y) <= 2)
		{
			bHasLastKnown = false;
		}
		else if (PathGoal != LastKnownTarget || StepIndex >= Path.Steps.Num())
		{
			RequestPath(LastKnownTarget, /*bAllowDigging*/ false);
		}
		return;
	}

	// Nothing known: wander now and then.
	if (StepIndex >= Path.Steps.Num() && RepathTimer <= 0.0f)
	{
		State = EMadZombieState::Wander;
		RequestPath(PickWanderGoal(), /*bAllowDigging*/ false);
		RepathTimer = Random.FRandRange(6.0f, 12.0f);
	}
}

FIntVector AMadZombie::PickWanderGoal()
{
	const FIntVector Feet = GetFeetVoxel();
	return Feet + FIntVector(Random.RandRange(-8, 8), Random.RandRange(-8, 8), 0);
}

// ===========================================================================
// Paths
// ===========================================================================

void AMadZombie::RequestPath(const FIntVector& Goal, bool bAllowDigging)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return;
	}

	auto GetVoxel = [VoxelWorld](const FIntVector& V) { return GetVoxelForPathing(*VoxelWorld, V); };
	auto BreakSeconds = [this](const FIntVector&, const FMadVoxel& Voxel) { return GetBreakSeconds(Definition, Voxel); };

	FMadPathSettings Settings;
	Settings.MaxNodes = FMath::Max(100, CVarPathNodes.GetValueOnGameThread());
	Settings.bAllowDigging = bAllowDigging;
	Settings.bClimbWalls = Definition.bClimbsWalls;
	Settings.IsClimbable = [VoxelWorld](const FIntVector& V)
	{
		if (!VoxelWorld->IsVoxelLoaded(V.X, V.Y, V.Z))
		{
			return false;
		}
		const FMadBlockDefinitionData* Block = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(VoxelWorld->GetVoxel(V.X, V.Y, V.Z).BlockTypeID);
		return Block != nullptr && Block->bClimbable;
	};

	Settings.IsLiquid = [VoxelWorld](const FIntVector& V)
	{
		if (!VoxelWorld->IsVoxelLoaded(V.X, V.Y, V.Z))
		{
			return false;
		}
		const FMadBlockDefinitionData* Block = UMadVoxelWorldSubsystem::GetBlockRegistry().FindDefinition(VoxelWorld->GetVoxel(V.X, V.Y, V.Z).BlockTypeID);
		return Block != nullptr && Block->bLiquid;
	};

	MadFall::Pathfinding::FindPath(GetFeetVoxel(), Goal, Settings, GetVoxel, BreakSeconds, Path);
	StepIndex = 0;
	PathGoal = Goal;
	RepathTimer = 2.0f + Random.FRandRange(0.0f, 1.0f);
	++TotalPaths;
}

bool AMadZombie::TryUndermine(const FIntVector& Goal)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return false;
	}

	FIntVector Block;
	const bool bFound = MadFall::Pathfinding::FindUndermineTarget(GetFeetVoxel(), Goal,
		[VoxelWorld](const FIntVector& V) { return GetVoxelForPathing(*VoxelWorld, V); },
		[this](const FIntVector&, const FMadVoxel& Voxel) { return GetBreakSeconds(Definition, Voxel); },
		Block);
	if (!bFound)
	{
		return false;
	}

	// One step, standing still, breaking the support. FollowPath digs it like any
	// other blocked step; the next think re-plans once it is gone or the target falls.
	FMadPathStep Step;
	Step.Feet = GetFeetVoxel();
	Step.BlocksToBreak.Add(Block);
	Path = FMadVoxelPath();
	Path.Steps.Add(Step);
	StepIndex = 0;
	PathGoal = Goal;

	if (Block != LastUndermineBlock)
	{
		LastUndermineBlock = Block;
		++TotalUndermines;
		UE_LOG(LogMadFallGameplay, Verbose, TEXT("%s undermines %s under a target at %s."), *GetName(), *Block.ToString(), *Goal.ToString());
	}
	return true;
}

bool AMadZombie::TryBreach(const FIntVector& Goal)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return false;
	}

	FIntVector Block;
	const bool bFound = MadFall::Pathfinding::FindBreachTarget(GetFeetVoxel(), Goal,
		[VoxelWorld](const FIntVector& V) { return GetVoxelForPathing(*VoxelWorld, V); },
		[this](const FIntVector&, const FMadVoxel& Voxel) { return GetBreakSeconds(Definition, Voxel); },
		Block);
	if (!bFound)
	{
		return false;
	}

	// As undermining: one standing step that breaks the block. Once the column is
	// open the next plan walks through it, or breaches the next layer.
	FMadPathStep Step;
	Step.Feet = GetFeetVoxel();
	Step.BlocksToBreak.Add(Block);
	Path = FMadVoxelPath();
	Path.Steps.Add(Step);
	StepIndex = 0;
	PathGoal = Goal;

	if (Block != LastBreachBlock)
	{
		LastBreachBlock = Block;
		++TotalBreaches;
		UE_LOG(LogMadFallGameplay, Verbose, TEXT("%s breaches %s toward a target at %s."), *GetName(), *Block.ToString(), *Goal.ToString());
	}
	return true;
}

void AMadZombie::FollowPath(float DeltaSeconds)
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bClimbStep = StepIndex < Path.Steps.Num() && Path.Steps[StepIndex].Move == EMadPathMove::Climb;
	if (!bClimbStep && Movement->MovementMode == MOVE_Flying)
	{
		// Off the ladder or over the top: gravity again.
		Movement->SetMovementMode(MOVE_Falling);
	}
	if (StepIndex >= Path.Steps.Num())
	{
		return;
	}

	UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	UMadStructuralSubsystem* Structural = GetWorld()->GetSubsystem<UMadStructuralSubsystem>();
	if (VoxelWorld == nullptr || Structural == nullptr)
	{
		return;
	}

	const FMadPathStep& Step = Path.Steps[StepIndex];

	// --- dig whatever is still in the way -----------------------------------------
	for (const FIntVector& Block : Step.BlocksToBreak)
	{
		if (!VoxelWorld->GetVoxel(Block.X, Block.Y, Block.Z).IsSolid())
		{
			continue;
		}
		if (Step.Move == EMadPathMove::DigDown && State != EMadZombieState::Dig)
		{
			++TotalDigDowns;
		}

		State = EMadZombieState::Dig;
		const FVector BlockCentre = (FVector(Block) + FVector(0.5)) * MadFall::VoxelSizeUU;
		const FVector Facing = (BlockCentre - GetActorLocation()).GetSafeNormal2D();
		if (!Facing.IsNearlyZero())
		{
			SetActorRotation(Facing.Rotation());
		}

		if (AttackCooldown <= 0.0f)
		{
			AttackCooldown = Definition.AttackSeconds;
			// Read before the hit: a block the hit destroys is already air after it.
			const FName StruckClass = UMadVoxelWorldSubsystem::GetBlockRegistry().GetBlockView(
				VoxelWorld->GetVoxel(Block.X, Block.Y, Block.Z).BlockTypeID).MaterialClass;
			const FMadBlockDamageResult Result = Structural->ApplyBlockDamage(Block, Definition.BlockDamage, Definition.DamageType);
			if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
			{
				Audio->PlayForMaterial(Result.bDestroyed ? EMadSound::Break : EMadSound::Hit, StruckClass, BlockCentre);
			}
			Body->PlayAttack();
			++TotalBlocksHit;
		}
		StuckTimer = 0.0f;
		LastProgressLocation = GetActorLocation();
		return;
	}

	if (State == EMadZombieState::Dig)
	{
		State = Target.IsValid() ? EMadZombieState::Chase : EMadZombieState::Wander;
	}

	const FIntVector Feet = GetFeetVoxel();

	// --- climb: fly up or down the column ------------------------------------------
	if (Step.Move == EMadPathMove::Climb)
	{
		if (Movement->MovementMode != MOVE_Flying)
		{
			Movement->SetMovementMode(MOVE_Flying);
		}
		const FVector ClimbTo((Step.Feet.X + 0.5) * MadFall::VoxelSizeUU, (Step.Feet.Y + 0.5) * MadFall::VoxelSizeUU,
			Step.Feet.Z * MadFall::VoxelSizeUU + GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 4.0);
		const FVector ToClimb = ClimbTo - GetActorLocation();
		if (FMath::Abs(ToClimb.Z) < 12.0f)
		{
			++StepIndex;
			++TotalClimbs;
			StuckTimer = 0.0f;
			LastProgressLocation = GetActorLocation();
			return;
		}
		AddMovementInput(ToClimb.GetSafeNormal(), 1.0f);
		StuckTimer = 0.0f;
		return;
	}

	// --- walk to the step --------------------------------------------------------
	const FVector StepCentre((Step.Feet.X + 0.5) * MadFall::VoxelSizeUU, (Step.Feet.Y + 0.5) * MadFall::VoxelSizeUU, GetActorLocation().Z);
	const FVector ToStep = StepCentre - GetActorLocation();

	if (ToStep.Size2D() < ZombieStepArrivalCm && FMath::Abs(Feet.Z - Step.Feet.Z) <= 1)
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

	// --- stuck detection ------------------------------------------------------
	StuckTimer += DeltaSeconds;
	if (StuckTimer > 1.5f)
	{
		if (FVector::Dist2D(GetActorLocation(), LastProgressLocation) < 25.0f)
		{
			// Something the path did not account for (another zombie, a block
			// placed since): jump, and re-plan on the next think.
			Jump();
			RepathTimer = 0.0f;
		}
		StuckTimer = 0.0f;
		LastProgressLocation = GetActorLocation();
	}
}

// ===========================================================================
// Combat
// ===========================================================================

void AMadZombie::TryAttackPlayer()
{
	AMadPlayerCharacter* Player = Target.Get();
	if (Player == nullptr || AttackCooldown > 0.0f)
	{
		return;
	}

	AttackCooldown = Definition.AttackSeconds;
	SetActorRotation((Player->GetActorLocation() - GetActorLocation()).GetSafeNormal2D().Rotation());

	UMadSurvivalComponent* Survival = Player->GetSurvival();
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayAt(EMadSound::ZombieAttack, GetActorLocation() + FVector(0.0, 0.0, 60.0));
	}
	Body->PlayAttack();
	Survival->ApplyAttackDamage(Definition.AttackDamage * MadFall::Difficulty::GetWorldScale(GetWorld(), FName(TEXT("zombie_damage"))));
	if (Definition.InfectionPerHit > 0.0f)
	{
		Survival->ApplyEffects({ { FName(TEXT("infection")), Definition.InfectionPerHit } });
	}
	++TotalPlayerHits;
}

void AMadZombie::UpdateTrap()
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadTrapContact Contact = VoxelWorld
		? MadFall::Traps::FindContact(*VoxelWorld, GetActorLocation(), GetCapsuleComponent()->GetScaledCapsuleHalfHeight())
		: FMadTrapContact();
	TrapSlow = Contact.IsValid() ? Contact.Block->TrapSlow : 1.0f;
	if (!Contact.IsValid() || Contact.Block->TrapDamage <= 0.0f || GetWorld()->GetTimeSeconds() < NextTrapHitTime)
	{
		return;
	}
	NextTrapHitTime = GetWorld()->GetTimeSeconds() + Contact.Block->TrapSeconds;
	++MadFall::Traps::TotalHits();
	// The trap wears first: the blow that kills the last zombie can break it too.
	if (UMadStructuralSubsystem* Structural = GetWorld()->GetSubsystem<UMadStructuralSubsystem>(); Structural && Contact.Block->TrapWear > 0.0f)
	{
		Structural->ApplyBlockDamage(Contact.Voxel, Contact.Block->TrapWear, FName(TEXT("madfall:blunt")));
	}
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayAt(EMadSound::FleshHit, GetActorLocation(), 0.7f);
	}
	ReceiveHit(Contact.Block->TrapDamage, FName(TEXT("madfall:pierce")), nullptr);
}

void AMadZombie::AlertTo(AMadPlayerCharacter& Player)
{
	if (IsDead())
	{
		return;
	}
	Target = &Player;
	LastKnownTarget = Player.GetFeetVoxel();
	bHasLastKnown = true;
	RepathTimer = 0.0f;
}

bool AMadZombie::TrySpit(AMadPlayerCharacter& Player, float DistanceVoxels)
{
	// Close enough to claw is close enough to claw; out of range or sight, walk.
	if (!bSeesTarget || DistanceVoxels > Definition.RangedRange || DistanceVoxels < 2.5f)
	{
		return false;
	}
	const FVector Facing = (Player.GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
	if (!Facing.IsNearlyZero())
	{
		SetActorRotation(Facing.Rotation());
	}
	if (RangedCooldown > 0.0f)
	{
		return true;   // holding its ground between spits
	}
	RangedCooldown = Definition.RangedSeconds;

	constexpr float Gravity = 0.3f;
	const FVector Mouth = GetActorLocation() + FVector(0.0, 0.0, 70.0 * GetActorScale3D().Z) + Facing * 40.0;
	const FVector Aim = Player.GetActorLocation() + FVector(0.0, 0.0, 40.0);
	const float Speed = Definition.RangedSpeed * MadFall::VoxelSizeUU;
	const float Flight = FVector::Dist(Aim, Mouth) / Speed;
	// Lob to cancel the fall over the flight: aim above by the drop.
	const FVector Velocity = (Aim - Mouth).GetSafeNormal() * Speed
		+ FVector(0.0, 0.0, 0.5 * MadFall::Projectile::GravityCm * Gravity * Flight);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AMadProjectile* Spit = GetWorld()->SpawnActor<AMadProjectile>(AMadProjectile::StaticClass(), Mouth, Velocity.Rotation(), Params))
	{
		const float Damage = Definition.RangedDamage * MadFall::Difficulty::GetWorldScale(GetWorld(), FName(TEXT("zombie_damage")));
		Spit->Launch(this, Velocity, Gravity, Damage, Definition.DamageType, NAME_None, 0.0f);
		Spit->SetColour(FLinearColor(0.35f, 0.6f, 0.05f));
	}
	Body->PlayAttack();
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayAt(EMadSound::ZombieSpit, Mouth);
	}
	++TotalSpits;
	return true;
}

void AMadZombie::Scream(AMadPlayerCharacter& Player)
{
	ScreamTimer = Definition.ScreamSeconds;
	++TotalScreams;
	Body->PlayAttack();
	if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
	{
		Audio->PlayAt(EMadSound::ZombieScream, GetActorLocation() + FVector(0.0, 0.0, 80.0), 1.4f);
	}
	int32 Alerted = 0;
	int32 Called = 0;
	if (UMadHordeSubsystem* Horde = GetWorld()->GetSubsystem<UMadHordeSubsystem>())
	{
		Alerted = Horde->AlertNear(GetActorLocation(), Definition.ScreamRadius, Player);
		Called = Definition.ScreamSummons > 0 ? Horde->CallReinforcements(Player, Definition.ScreamSummons) : 0;
	}
	// A scream is also a noise the survivor makes others hear: everything near knows.
	UE_LOG(LogMadFallGameplay, Display, TEXT("%s screams: %d zombie(s) alerted, %d called."), *VariantId.ToString(), Alerted, Called);
}

float AMadZombie::GetDamageMultiplier(FName DamageType) const
{
	return Definition.GetDamageMultiplier(DamageType);
}

bool AMadZombie::ReceiveHit(float Amount, FName DamageType, AActor* Attacker)
{
	// The console's kill-all is not a damage type anything resists.
	if (DamageType != FName(TEXT("madfall:admin")))
	{
		Amount *= GetDamageMultiplier(DamageType);
	}
	if (IsDead() || Amount <= 0.0f)
	{
		return false;
	}

	Body->PlayHit();
	const float NewHealth = FMath::Max(0.0f, GetHealth() - Amount);
	AbilitySystem->SetNumericAttributeBase(UMadSurvivalAttributeSet::GetHealthAttribute(), NewHealth);

	// Being hit is the loudest possible noise.
	if (AMadPlayerCharacter* Player = Cast<AMadPlayerCharacter>(Attacker))
	{
		Target = Player;
		LastKnownTarget = Player->GetFeetVoxel();
		bHasLastKnown = true;
	}

	if (NewHealth <= 0.0f)
	{
		Die(Attacker);
		return true;
	}
	return false;
}

void AMadZombie::Die(AActor* Killer)
{
	State = EMadZombieState::Dead;
	DespawnTimer = 4.0f;
	++TotalKills;

	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetCharacterMovement()->DisableMovement();
	// A corpse lies there four seconds; the living must not steer around it.
	GetCharacterMovement()->SetAvoidanceEnabled(false);
	Body->PlayDeath();

	if (AMadPlayerCharacter* Player = Cast<AMadPlayerCharacter>(Killer))
	{
		Player->AddExperience(Definition.Experience);
		Player->NotifyQuest(EMadQuestObjectiveType::KillZombie, VariantId, Definition.Tags);
		if (!PoiPrefabId.IsNone())
		{
			Player->NotifyQuest(EMadQuestObjectiveType::ClearPoi, PoiPrefabId, PoiTags);
		}

		// Loot drops as a bag where the zombie fell: walking over to it is part
		// of the risk of killing something in the middle of a horde.
		const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
		if (const FMadLootTableDefinition* Table = Definitions.FindLootTable(Definition.LootTable))
		{
			FRandomStream LootRandom(static_cast<int32>(GetUniqueID() * 7919));
			TArray<FMadItemStack> Drops;
			MadFall::Loot::Roll(*Table, Definitions, FMadLootContext{ 1, Player->GetGameStage() }, LootRandom, Drops);
			if (UMadPickupSubsystem* Pickups = GetWorld()->GetSubsystem<UMadPickupSubsystem>())
			{
				Pickups->Drop(GetActorLocation(), Drops);
			}
		}
	}

	UE_LOG(LogMadFallGameplay, Log, TEXT("%s died."), *VariantId.ToString());
}

FString AMadZombie::DescribeStatus() const
{
	static const TCHAR* StateNames[] = { TEXT("idle"), TEXT("wander"), TEXT("chase"), TEXT("dig"), TEXT("attack"), TEXT("dead") };
	return FString::Printf(TEXT("%s%s at %s: %s, %.0f hp, path %d/%d%s%s"),
		*VariantId.ToString(), bHorde ? TEXT(" (horde)") : TEXT(""), *GetFeetVoxel().ToString(),
		StateNames[static_cast<int32>(State)], GetHealth(), StepIndex, Path.Steps.Num(),
		Path.bReachesGoal ? TEXT(" reaches") : TEXT(" partial"), Path.RequiresDigging() ? TEXT(", digs") : TEXT(""));
}

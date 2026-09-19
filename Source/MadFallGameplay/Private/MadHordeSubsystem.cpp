// Copyright MadFall. All Rights Reserved.

#include "MadHordeSubsystem.h"

#include "MadPrefabRegistry.h"

#include "MadDifficulty.h"
#include "MadTraps.h"

#include "MadAudioSubsystem.h"

#include "MadFrameBudget.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadFallGameplay.h"
#include "MadGameplayDefinitions.h"
#include "MadPlayerCharacter.h"
#include "MadPoiPlanner.h"
#include "MadVoxelPathfinder.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "MadWorldGenerator.h"
#include "MadZombie.h"
#include "Misc/DefaultValueHelper.h"

namespace
{
	TAutoConsoleVariable<int32> CVarMaxZombies(
		TEXT("mad.ai.MaxZombies"),
		40,
		TEXT("Hard cap on live zombies."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWaveSeconds(
		TEXT("mad.horde.WaveSeconds"),
		30.0f,
		TEXT("Seconds between horde waves."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarSpawnSleepers(
		TEXT("mad.ai.Sleepers"),
		1,
		TEXT("Spawn POI sleeper zombies when the player approaches."),
		ECVF_Default);

	const FName HordeGroup(TEXT("madfall:zombies/horde"));
	const FName WanderGroup(TEXT("madfall:zombies/wander"));
	const FName SpawnMarker(TEXT("spawn"));

	constexpr int32 SleeperWakeRadius = 40;

}

bool UMadHordeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadHordeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadHordeSubsystem, STATGROUP_Tickables);
}

int32 UMadHordeSubsystem::HordeSizeForGameStage(int32 GameStage)
{
	// Day 7, level ~5: 17. Day 28, level ~20: 44. Linear is readable and easy to
	// tune; the live cap spreads a large horde over waves anyway.
	return 8 + FMath::RoundToInt32(GameStage * 0.75f);
}

int32 UMadHordeSubsystem::NumAlive() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AMadZombie>& Zombie : Alive)
	{
		if (Zombie.IsValid() && !Zombie->IsDead())
		{
			++Count;
		}
	}
	return Count;
}

void UMadHordeSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Horde);

	Alive.RemoveAll([](const TWeakObjectPtr<AMadZombie>& Zombie) { return !Zombie.IsValid(); });

	AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(GetWorld());
	if (Player == nullptr || Player->IsWaitingForWorld())
	{
		return;
	}

	TickHorde(DeltaTime, *Player);
	TickSleepers(*Player);
	TickWanderers(DeltaTime, *Player);
}

void UMadHordeSubsystem::TickHorde(float DeltaTime, AMadPlayerCharacter& Player)
{
	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	const bool bHordeNight = Clock != nullptr && Clock->IsHordeNight();

	if (bHordeNight && !bHordeActive)
	{
		bHordeActive = true;
		HordeNightDay = Clock->GetNightDay();
		HordeSpawned = 0;
		HordeTarget = FMath::Max(1, FMath::RoundToInt32(HordeSizeForGameStage(Player.GetGameStage())
			* MadFall::Difficulty::GetWorldScale(GetWorld(), FName(TEXT("horde_size")))));
		WaveTimer = 0.0f;
		UE_LOG(LogMadFallGameplay, Display, TEXT("Horde night %d begins: %d zombies at game stage %d."),
			HordeNightDay, HordeTarget, Player.GetGameStage());
		Player.PushMessage(TEXT("HORDE NIGHT. They know where you are."), 6.0f);
		if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
		{
			Audio->Play2D(EMadSound::HordeHorn);
		}
	}
	else if (!bHordeNight && bHordeActive && Clock != nullptr)
	{
		bHordeActive = false;
		for (const TWeakObjectPtr<AMadZombie>& Zombie : Alive)
		{
			if (Zombie.IsValid())
			{
				Zombie->SetHorde(false);
			}
		}
		UE_LOG(LogMadFallGameplay, Display, TEXT("Horde night %d is over: %d spawned."), HordeNightDay, HordeSpawned);
		Player.PushMessage(TEXT("Dawn. You survived the horde."), 5.0f);
	}

	if (!bHordeActive || HordeSpawned >= HordeTarget)
	{
		return;
	}

	WaveTimer -= DeltaTime;
	if (WaveTimer > 0.0f)
	{
		return;
	}
	WaveTimer = CVarWaveSeconds.GetValueOnGameThread();

	const int32 WaveSize = FMath::Min(HordeTarget - HordeSpawned, FMath::Max(4, HordeTarget / 4));
	HordeSpawned += SpawnWave(Player, WaveSize, /*bHorde*/ true, HordeGroup, 22, 34);
}

int32 UMadHordeSubsystem::ForceWave()
{
	AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(GetWorld());
	if (Player == nullptr)
	{
		return 0;
	}
	const int32 Size = FMath::Max(4, HordeSizeForGameStage(Player->GetGameStage()) / 4);
	return SpawnWave(*Player, Size, /*bHorde*/ true, HordeGroup, 12, 20);
}

void UMadHordeSubsystem::TickSleepers(AMadPlayerCharacter& Player)
{
	SleeperTimer -= GetWorld()->GetDeltaSeconds();
	if (SleeperTimer > 0.0f || CVarSpawnSleepers.GetValueOnGameThread() == 0)
	{
		return;
	}
	SleeperTimer = 2.0f;

	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	if (Generator == nullptr)
	{
		return;
	}

	const FIntVector Feet = Player.GetFeetVoxel();
	const FMadPoiPlanner& Planner = Generator->GetPoiPlanner();
	const int32 Day = Clock ? Clock->GetDay() : 1;

	TSet<FIntPoint> SeenCells;
	TArray<FMadPoiInstance> Pois;
	TArray<FMadPoiWorldMarker> Markers;

	for (int32 DY = -1; DY <= 1; ++DY)
	{
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			const FMadChunkCoord Coord = MadFall::WorldToChunk(Feet.X + DX * MadFall::ChunkSize, Feet.Y + DY * MadFall::ChunkSize, Feet.Z);
			Planner.GetPoisOverlappingChunk(*Generator, Coord, Pois);

			for (const FMadPoiInstance& Poi : Pois)
			{
				bool bAlreadySeen = false;
				SeenCells.Add(Poi.Cell, &bAlreadySeen);
				if (bAlreadySeen)
				{
					continue;
				}

				Planner.GetWorldMarkers(Poi, Markers);
				for (const FMadPoiWorldMarker& Marker : Markers)
				{
					if (Marker.Type != SpawnMarker || Marker.SpawnGroup.IsNone())
					{
						continue;
					}
					const FIntVector Offset = Marker.WorldPosition - Feet;
					if (FMath::Abs(Offset.X) > SleeperWakeRadius || FMath::Abs(Offset.Y) > SleeperWakeRadius || FMath::Abs(Offset.Z) > 16)
					{
						continue;
					}
					if (const int32* LastDay = SleeperSpawnDay.Find(Marker.WorldPosition); LastDay && *LastDay == Day)
					{
						continue;
					}
					if (!VoxelWorld->IsVoxelLoaded(Marker.WorldPosition.X, Marker.WorldPosition.Y, Marker.WorldPosition.Z))
					{
						continue;
					}

					SleeperSpawnDay.Add(Marker.WorldPosition, Day);

					// The prefab's own tags, so a job can ask for "a military
					// site" rather than naming one building.
					const FMadPrefabRegistry& PrefabRegistry = UMadVoxelWorldSubsystem::GetPrefabRegistry();
					const int32 PrefabIndex = PrefabRegistry.FindIndex(Marker.PrefabId);
					static const TArray<FName> NoTags;
					const TArray<FName>& PrefabTags = PrefabIndex != INDEX_NONE ? PrefabRegistry.Get(PrefabIndex).Tags : NoTags;

					int32 Spawned = 0;
					for (int32 Index = 0; Index < FMath::Max(1, Marker.Count); ++Index)
					{
						const FMadZombieDefinition* Variant = PickVariant(Marker.SpawnGroup, Player.GetGameStage());
						FIntVector SpawnAt;
						AMadZombie* Woken = nullptr;
						if (Variant && FindStandableNear(Marker.WorldPosition.X + Index % 2, Marker.WorldPosition.Y + Index / 2, Marker.WorldPosition.Z, SpawnAt))
						{
							Woken = SpawnZombie(*Variant, SpawnAt, /*bHorde*/ false);
						}
						if (Woken)
						{
							Woken->SetPoiPrefab(Marker.PrefabId, PrefabTags);
							++Spawned;
						}
					}
					UE_LOG(LogMadFallGameplay, Log, TEXT("POI %s woke %d sleeper(s) at %s."),
						*Marker.PrefabId.ToString(), Spawned, *Marker.WorldPosition.ToString());
				}
			}
		}
	}
}

void UMadHordeSubsystem::TickWanderers(float DeltaTime, AMadPlayerCharacter& Player)
{
	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	if (Clock == nullptr || !Clock->IsNight() || bHordeActive)
	{
		return;
	}

	WanderTimer -= DeltaTime;
	if (WanderTimer > 0.0f)
	{
		return;
	}
	WanderTimer = Random.FRandRange(45.0f, 90.0f);

	if (NumAlive() < CVarMaxZombies.GetValueOnGameThread() / 3)
	{
		SpawnWave(Player, Random.RandRange(1, 3), /*bHorde*/ false, WanderGroup, 28, 40);
	}
}

// ===========================================================================
// Spawning
// ===========================================================================

const FMadZombieDefinition* UMadHordeSubsystem::PickVariant(FName Group, int32 GameStage)
{
	TArray<const FMadZombieDefinition*> Candidates;
	MadFall::GetGameplayDefinitions().GetZombiesInGroup(Group, GameStage, Candidates);
	if (Candidates.Num() == 0)
	{
		return nullptr;
	}

	float Total = 0.0f;
	for (const FMadZombieDefinition* Candidate : Candidates)
	{
		Total += Candidate->SpawnWeight;
	}
	float Pick = Random.FRandRange(0.0f, Total);
	for (const FMadZombieDefinition* Candidate : Candidates)
	{
		if (Pick < Candidate->SpawnWeight)
		{
			return Candidate;
		}
		Pick -= Candidate->SpawnWeight;
	}
	return Candidates.Last();
}

bool UMadHordeSubsystem::FindStandableNear(int32 X, int32 Y, int32 AroundZ, FIntVector& OutFeet) const
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return false;
	}

	auto GetVoxel = [VoxelWorld](const FIntVector& V) { return VoxelWorld->GetVoxel(V.X, V.Y, V.Z); };

	// Search outward from the reference height, up first: a POI's spawn floor
	// or the surface near the player is almost always within a few voxels.
	for (int32 Offset = 0; Offset <= 24; ++Offset)
	{
		for (int32 Sign : { 1, -1 })
		{
			const FIntVector Feet(X, Y, AroundZ + Offset * Sign);
			if (!VoxelWorld->IsVoxelLoaded(Feet.X, Feet.Y, Feet.Z) || !VoxelWorld->IsVoxelLoaded(Feet.X, Feet.Y, Feet.Z - 1))
			{
				continue;
			}
			if (MadFall::Pathfinding::IsStandable(Feet, GetVoxel) && !GetVoxel(Feet).HasFlag(EMadVoxelFlags::Liquid))
			{
				OutFeet = Feet;
				return true;
			}
		}
	}
	return false;
}

AMadZombie* UMadHordeSubsystem::SpawnZombie(const FMadZombieDefinition& Definition, const FIntVector& Feet, bool bHorde)
{
	if (NumAlive() >= CVarMaxZombies.GetValueOnGameThread())
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

	const FVector Location((Feet.X + 0.5) * MadFall::VoxelSizeUU, (Feet.Y + 0.5) * MadFall::VoxelSizeUU,
		Feet.Z * MadFall::VoxelSizeUU + 92.0 * Definition.Scale + 4.0);

	AMadZombie* Zombie = GetWorld()->SpawnActor<AMadZombie>(AMadZombie::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (Zombie == nullptr)
	{
		return nullptr;
	}

	Zombie->InitialiseFromDefinition(Definition, bHorde);
	Alive.Add(Zombie);
	++TotalSpawned;
	return Zombie;
}

int32 UMadHordeSubsystem::SpawnWave(AMadPlayerCharacter& Player, int32 Count, bool bHorde, FName Group, int32 MinRing, int32 MaxRing)
{
	const FIntVector Feet = Player.GetFeetVoxel();
	int32 Spawned = 0;

	// A few attempts per zombie: a ring position can be over water, inside a
	// hill with no air above, or in a chunk that has not loaded.
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FMadZombieDefinition* Variant = PickVariant(Group, Player.GetGameStage());
		if (Variant == nullptr)
		{
			break;
		}

		for (int32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			const float Angle = Random.FRandRange(0.0f, UE_TWO_PI);
			const float Radius = Random.FRandRange(static_cast<float>(MinRing), static_cast<float>(MaxRing));
			const int32 X = Feet.X + FMath::RoundToInt32(FMath::Cos(Angle) * Radius);
			const int32 Y = Feet.Y + FMath::RoundToInt32(FMath::Sin(Angle) * Radius);

			FIntVector SpawnAt;
			if (FindStandableNear(X, Y, Feet.Z, SpawnAt) && SpawnZombie(*Variant, SpawnAt, bHorde))
			{
				++Spawned;
				break;
			}
		}
	}

	UE_LOG(LogMadFallGameplay, Display, TEXT("Spawned %d of %d %s zombie(s) around %s."),
		Spawned, Count, bHorde ? TEXT("horde") : TEXT("wandering"), *Feet.ToString());
	return Spawned;
}

int32 UMadHordeSubsystem::AlertNear(const FVector& Location, float RadiusVoxels, AMadPlayerCharacter& Player)
{
	const double RadiusSq = FMath::Square(static_cast<double>(RadiusVoxels) * MadFall::VoxelSizeUU);
	int32 Alerted = 0;
	for (const TWeakObjectPtr<AMadZombie>& Weak : Alive)
	{
		AMadZombie* Zombie = Weak.Get();
		if (Zombie != nullptr && !Zombie->IsDead() && FVector::DistSquared(Zombie->GetActorLocation(), Location) <= RadiusSq)
		{
			Zombie->AlertTo(Player);
			++Alerted;
		}
	}
	return Alerted;
}

int32 UMadHordeSubsystem::CallReinforcements(AMadPlayerCharacter& Player, int32 Count)
{
	const int32 Before = Alive.Num();
	const int32 Spawned = SpawnWave(Player, Count, /*bHorde*/ false, WanderGroup, 24, 40);
	// Called zombies know where the survivor is; they did not stumble on them.
	for (int32 Index = Before; Index < Alive.Num(); ++Index)
	{
		if (AMadZombie* Zombie = Alive[Index].Get())
		{
			Zombie->AlertTo(Player);
		}
	}
	return Spawned;
}

int32 UMadHordeSubsystem::KillAll()
{
	int32 Killed = 0;
	for (const TWeakObjectPtr<AMadZombie>& Zombie : Alive)
	{
		if (Zombie.IsValid() && !Zombie->IsDead())
		{
			Zombie->ReceiveHit(1.0e6f, FName(TEXT("madfall:admin")), nullptr);
			++Killed;
		}
	}
	return Killed;
}

FString UMadHordeSubsystem::DescribeStatus() const
{
	FString Out = FString::Printf(TEXT("Zombies: %d alive (cap %d), %lld spawned total; horde %s (%d/%d)\n")
		TEXT("  lifetime: %d paths, %d block hits, %d player hits, %d kills, %d undermines, %d breaches, %d spits, %d screams, %d trap hits, %d climbed, %d dug down"),
		NumAlive(), CVarMaxZombies.GetValueOnGameThread(), TotalSpawned,
		bHordeActive ? TEXT("ACTIVE") : TEXT("inactive"), HordeSpawned, HordeTarget,
		AMadZombie::TotalPaths, AMadZombie::TotalBlocksHit, AMadZombie::TotalPlayerHits, AMadZombie::TotalKills, AMadZombie::TotalUndermines, AMadZombie::TotalBreaches, AMadZombie::TotalSpits, AMadZombie::TotalScreams, MadFall::Traps::TotalHits(), AMadZombie::TotalClimbs, AMadZombie::TotalDigDowns);

	int32 Listed = 0;
	for (const TWeakObjectPtr<AMadZombie>& Zombie : Alive)
	{
		if (Zombie.IsValid() && Listed++ < 12)
		{
			Out += TEXT("\n  ") + Zombie->DescribeStatus();
		}
	}
	return Out;
}

// ===========================================================================
// Console
// ===========================================================================

namespace
{
	UMadHordeSubsystem* GetHorde(UWorld* World)
	{
		UMadHordeSubsystem* Horde = World ? World->GetSubsystem<UMadHordeSubsystem>() : nullptr;
		if (Horde == nullptr)
		{
			UE_LOG(LogMadFallGameplay, Error, TEXT("No horde director in this world (game worlds only)."));
		}
		return Horde;
	}

	FAutoConsoleCommandWithWorld CmdHordeStatus(
		TEXT("mad.ai.status"), TEXT("Live zombies, horde progress and AI counters."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadHordeSubsystem* Horde = GetHorde(World)) { UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Horde->DescribeStatus()); }
		}));

	FAutoConsoleCommandWithWorld CmdHordeWave(
		TEXT("mad.horde.wave"), TEXT("Spawns a horde wave around the player now."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadHordeSubsystem* Horde = GetHorde(World)) { Horde->ForceWave(); }
		}));

	// Aiming at the nearest zombie, the same helper mad.player.aimanimal is for:
	// a scripted session has no mouse, and "walk up to the building and hit what
	// comes out" is the one thing a clearing job is made of.
	FAutoConsoleCommandWithWorld CmdAimZombie(
		TEXT("mad.player.aimzombie"), TEXT("Turns the survivor to look at the nearest live zombie."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			UMadHordeSubsystem* Horde = GetHorde(World);
			AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(World);
			if (Horde == nullptr || Player == nullptr)
			{
				return;
			}

			AMadZombie* Nearest = nullptr;
			double Best = TNumericLimits<double>::Max();
			for (const TWeakObjectPtr<AMadZombie>& Zombie : Horde->GetAlive())
			{
				if (!Zombie.IsValid() || Zombie->IsDead())
				{
					continue;
				}
				const double Distance = FVector::DistSquared(Zombie->GetActorLocation(), Player->GetActorLocation());
				if (Distance < Best)
				{
					Best = Distance;
					Nearest = Zombie.Get();
				}
			}
			if (Nearest == nullptr)
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("No zombie to aim at."));
				return;
			}
			Player->AimAtLocation(Nearest->GetActorLocation() + FVector(0.0, 0.0, 40.0));
			UE_LOG(LogMadFallGameplay, Display, TEXT("Aiming at %s, %.1f voxels away."),
				*Nearest->GetVariantId().ToString(), FMath::Sqrt(Best) / MadFall::VoxelSizeUU);
		}));

	FAutoConsoleCommandWithWorld CmdKillAll(
		TEXT("mad.ai.killall"), TEXT("Kills every live zombie."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadHordeSubsystem* Horde = GetHorde(World)) { UE_LOG(LogMadFallGameplay, Display, TEXT("Killed %d zombie(s)."), Horde->KillAll()); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSpawnZombie(
		TEXT("mad.ai.spawn"), TEXT("mad.ai.spawn <zombie id> <dx> <dy> [horde 0|1] - spawn next to the player."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			UMadHordeSubsystem* Horde = GetHorde(World);
			const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			const AMadPlayerCharacter* Player = Controller ? Cast<AMadPlayerCharacter>(Controller->GetPawn()) : nullptr;
			int32 DX = 0, DY = 0, HordeFlag = 0;
			if (Horde == nullptr || Player == nullptr || Args.Num() < 3
				|| !FDefaultValueHelper::ParseInt(Args[1], DX) || !FDefaultValueHelper::ParseInt(Args[2], DY))
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("Usage: mad.ai.spawn <zombie id> <dx> <dy> [horde]"));
				return;
			}
			if (Args.Num() > 3) { FDefaultValueHelper::ParseInt(Args[3], HordeFlag); }

			const FMadZombieDefinition* Definition = MadFall::GetGameplayDefinitions().FindZombie(FName(*Args[0]));
			if (Definition == nullptr)
			{
				UE_LOG(LogMadFallGameplay, Error, TEXT("Unknown zombie '%s'. Run mad.items to list them."), *Args[0]);
				return;
			}

			const FIntVector Column = Player->GetFeetVoxel() + FIntVector(DX, DY, 0);
			FIntVector Feet = Column;
			const AMadZombie* Zombie = Horde->FindStandableNear(Column.X, Column.Y, Column.Z, Feet)
				? Horde->SpawnZombie(*Definition, Feet, HordeFlag != 0) : nullptr;
			UE_LOG(LogMadFallGameplay, Display, TEXT("Spawn %s at %s: %s"), *Args[0], *Feet.ToString(), Zombie ? TEXT("ok") : TEXT("failed"));
		}));
}

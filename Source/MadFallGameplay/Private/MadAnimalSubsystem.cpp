// Copyright MadFall. All Rights Reserved.

#include "MadAnimalSubsystem.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadAnimal.h"
#include "MadBiomeRegistry.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadGameplayDefinitions.h"
#include "MadHordeSubsystem.h"
#include "MadPlayerCharacter.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "MadWorldGenerator.h"
#include "Misc/DefaultValueHelper.h"

namespace
{
	TAutoConsoleVariable<float> CVarSpawnSeconds(
		TEXT("mad.animals.SpawnSeconds"),
		25.0f,
		TEXT("Seconds between attempts to spawn a herd around the survivor. 0 disables natural spawning."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarMaxAnimals(
		TEXT("mad.animals.Max"),
		12,
		TEXT("Live animals the director keeps at most."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarDespawnVoxels(
		TEXT("mad.animals.DespawnVoxels"),
		110,
		TEXT("Animals further than this from the survivor are removed."),
		ECVF_Default);

	/** Herds appear in this ring: past where the survivor would watch them pop in, inside streamed terrain. */
	constexpr int32 MinRingVoxels = 30;
	constexpr int32 MaxRingVoxels = 56;

	/** An animal out of its active hours goes once it is at least this far away. */
	constexpr float OffHoursDespawnVoxels = 40.0f;

}

bool UMadAnimalSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadAnimalSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadAnimalSubsystem, STATGROUP_Tickables);
}

void UMadAnimalSubsystem::GetCandidates(const TArray<FMadAnimalDefinition>& Animals, FName Biome, bool bNight, TArray<const FMadAnimalDefinition*>& Out)
{
	Out.Reset();
	for (const FMadAnimalDefinition& Animal : Animals)
	{
		if (Animal.SpawnWeight > 0.0f && Animal.Biomes.Contains(Biome) && Animal.IsActive(bNight))
		{
			Out.Add(&Animal);
		}
	}
}

const FMadAnimalDefinition* UMadAnimalSubsystem::PickWeighted(const TArray<const FMadAnimalDefinition*>& Candidates, float Roll01)
{
	float Total = 0.0f;
	for (const FMadAnimalDefinition* Animal : Candidates)
	{
		Total += Animal->SpawnWeight;
	}
	if (Candidates.Num() == 0 || Total <= 0.0f)
	{
		return nullptr;
	}
	float Pick = FMath::Clamp(Roll01, 0.0f, 0.9999f) * Total;
	for (const FMadAnimalDefinition* Animal : Candidates)
	{
		if (Pick < Animal->SpawnWeight)
		{
			return Animal;
		}
		Pick -= Animal->SpawnWeight;
	}
	return Candidates.Last();
}

int32 UMadAnimalSubsystem::NumAlive() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AMadAnimal>& Animal : Alive)
	{
		Count += Animal.IsValid() && !Animal->IsDead() ? 1 : 0;
	}
	return Count;
}

void UMadAnimalSubsystem::GetAlive(TArray<AMadAnimal*>& Out) const
{
	Out.Reset();
	for (const TWeakObjectPtr<AMadAnimal>& Animal : Alive)
	{
		if (Animal.IsValid() && !Animal->IsDead())
		{
			Out.Add(Animal.Get());
		}
	}
}

AMadAnimal* UMadAnimalSubsystem::SpawnAnimal(const FMadAnimalDefinition& Definition, const FIntVector& Feet)
{
	if (NumAlive() >= CVarMaxAnimals.GetValueOnGameThread())
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

	// Spawned a little high with a provisional capsule; InitialiseFromDefinition
	// resizes it and the animal settles onto the ground.
	const FVector Location((Feet.X + 0.5) * MadFall::VoxelSizeUU, (Feet.Y + 0.5) * MadFall::VoxelSizeUU, Feet.Z * MadFall::VoxelSizeUU + 60.0);
	AMadAnimal* Animal = GetWorld()->SpawnActor<AMadAnimal>(AMadAnimal::StaticClass(), Location,
		FRotator(0.0, Random.FRandRange(0.0f, 360.0f), 0.0), Params);
	if (Animal == nullptr)
	{
		return nullptr;
	}
	Animal->InitialiseFromDefinition(Definition);
	Alive.Add(Animal);
	++TotalSpawned;
	return Animal;
}

int32 UMadAnimalSubsystem::SpawnHerd(const FMadAnimalDefinition& Definition, const FIntVector& Near, int32 Count)
{
	const UMadHordeSubsystem* Horde = GetWorld()->GetSubsystem<UMadHordeSubsystem>();
	if (Horde == nullptr)
	{
		return 0;
	}

	int32 Spawned = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		// Members stand a couple of voxels apart around the herd's column.
		for (int32 Attempt = 0; Attempt < 4; ++Attempt)
		{
			const int32 X = Near.X + (Index == 0 && Attempt == 0 ? 0 : Random.RandRange(-3, 3));
			const int32 Y = Near.Y + (Index == 0 && Attempt == 0 ? 0 : Random.RandRange(-3, 3));
			FIntVector Feet;
			if (Horde->FindStandableNear(X, Y, Near.Z, Feet) && SpawnAnimal(Definition, Feet) != nullptr)
			{
				++Spawned;
				break;
			}
		}
	}
	return Spawned;
}

int32 UMadAnimalSubsystem::TrySpawnHerd(AMadPlayerCharacter& Player)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	if (Generator == nullptr || NumAlive() >= CVarMaxAnimals.GetValueOnGameThread())
	{
		return 0;
	}

	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	const bool bNight = Clock != nullptr && Clock->IsNight();
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FIntVector Feet = Player.GetFeetVoxel();

	for (int32 Attempt = 0; Attempt < 4; ++Attempt)
	{
		const float Angle = Random.FRandRange(0.0f, UE_TWO_PI);
		const float Radius = Random.FRandRange(static_cast<float>(MinRingVoxels), static_cast<float>(MaxRingVoxels));
		const FIntVector Column(Feet.X + FMath::RoundToInt32(FMath::Cos(Angle) * Radius), Feet.Y + FMath::RoundToInt32(FMath::Sin(Angle) * Radius), Feet.Z);
		if (!VoxelWorld->IsVoxelLoaded(Column.X, Column.Y, Column.Z))
		{
			continue;
		}

		const int32 BiomeIndex = Generator->GetDominantBiome(Column.X + 0.5f, Column.Y + 0.5f);
		const FMadBiomeRegistry& Biomes = Generator->GetBiomes();
		if (BiomeIndex < 0 || BiomeIndex >= Biomes.Num())
		{
			continue;
		}
		const FName Biome = Biomes.Get(BiomeIndex).Id;

		TArray<const FMadAnimalDefinition*> Candidates;
		GetCandidates(Definitions.GetAnimals(), Biome, bNight, Candidates);
		const FMadAnimalDefinition* Species = PickWeighted(Candidates, Random.FRand());
		if (Species == nullptr)
		{
			// Nothing lives here at this hour; do not keep rolling into the same ocean.
			return 0;
		}

		const int32 Herd = Random.RandRange(Species->HerdMin, Species->HerdMax);
		// Surface height is a better starting guess than the survivor's feet on a hillside.
		const FIntVector Near(Column.X, Column.Y, FMath::FloorToInt32(Generator->GetSurfaceHeight(Column.X + 0.5f, Column.Y + 0.5f)) + 1);
		const int32 Spawned = SpawnHerd(*Species, Near, Herd);
		if (Spawned > 0)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("Spawned %d %s in %s at %s."), Spawned, *Species->Id.ToString(), *Biome.ToString(), *Near.ToString());
			return Spawned;
		}
	}
	return 0;
}

void UMadAnimalSubsystem::DespawnDistant(AMadPlayerCharacter& Player)
{
	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	const bool bNight = Clock != nullptr && Clock->IsNight();
	const float MaxDistance = static_cast<float>(CVarDespawnVoxels.GetValueOnGameThread());
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();

	for (const TWeakObjectPtr<AMadAnimal>& Weak : Alive)
	{
		AMadAnimal* Animal = Weak.Get();
		if (Animal == nullptr || Animal->IsDead())
		{
			continue;
		}
		const float Distance = static_cast<float>(FVector::Dist(Animal->GetActorLocation(), Player.GetActorLocation()) / MadFall::VoxelSizeUU);
		const FMadAnimalDefinition* Species = Definitions.FindAnimal(Animal->GetSpeciesId());
		const bool bOffHours = Species != nullptr && !Species->IsActive(bNight) && Distance > OffHoursDespawnVoxels && !Animal->WasRecentlyRendered(1.0f);
		if (Distance > MaxDistance || bOffHours)
		{
			Animal->Destroy();
			++TotalDespawned;
		}
	}
}

void UMadAnimalSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Zombies);

	Alive.RemoveAll([](const TWeakObjectPtr<AMadAnimal>& Animal) { return !Animal.IsValid(); });

	AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(GetWorld());
	if (Player == nullptr || Player->IsDown())
	{
		return;
	}

	DespawnTimer -= DeltaTime;
	if (DespawnTimer <= 0.0f)
	{
		DespawnTimer = 5.0f;
		DespawnDistant(*Player);
	}

	const float Interval = CVarSpawnSeconds.GetValueOnGameThread();
	if (Interval <= 0.0f)
	{
		return;
	}
	SpawnTimer -= DeltaTime;
	if (SpawnTimer <= 0.0f)
	{
		SpawnTimer = Interval * Random.FRandRange(0.75f, 1.25f);
		TrySpawnHerd(*Player);
	}
}

int32 UMadAnimalSubsystem::KillAll()
{
	int32 Removed = 0;
	for (const TWeakObjectPtr<AMadAnimal>& Animal : Alive)
	{
		if (Animal.IsValid())
		{
			Animal->Destroy();
			++Removed;
		}
	}
	Alive.Reset();
	return Removed;
}

FString UMadAnimalSubsystem::DescribeStatus() const
{
	TStringBuilder<1024> Out;
	Out.Appendf(TEXT("Animals: %d alive, %d spawned, %d despawned, %d killed, %d hit(s) on the survivor."),
		NumAlive(), TotalSpawned, TotalDespawned, AMadAnimal::TotalKills, AMadAnimal::TotalPlayerHits);
	for (const TWeakObjectPtr<AMadAnimal>& Animal : Alive)
	{
		if (Animal.IsValid())
		{
			Out.Appendf(TEXT("\n  %s"), *Animal->DescribeStatus());
		}
	}
	return FString(Out.ToString());
}

static FAutoConsoleCommandWithWorld GMadAnimalsStatusCommand(
	TEXT("mad.animals.status"),
	TEXT("Lists live animals and what each is doing."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadAnimalSubsystem* Animals = World ? World->GetSubsystem<UMadAnimalSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Animals->DescribeStatus());
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadAnimalsSpawnCommand(
	TEXT("mad.animals.spawn"),
	TEXT("mad.animals.spawn <animal id> [count=1] [distance=5]: spawns animals in front of the survivor."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadAnimalSubsystem* Animals = World ? World->GetSubsystem<UMadAnimalSubsystem>() : nullptr;
		const AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(World);
		const FMadAnimalDefinition* Species = Args.Num() > 0 ? MadFall::GetGameplayDefinitions().FindAnimal(FName(*Args[0])) : nullptr;
		if (Animals == nullptr || Player == nullptr || Species == nullptr)
		{
			UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.animals.spawn <animal id> [count] [distance] (a known animal, in a game world)"));
			return;
		}
		int32 Count = 1;
		int32 Distance = 5;
		if (Args.Num() > 1) { FDefaultValueHelper::ParseInt(Args[1], Count); }
		if (Args.Num() > 2) { FDefaultValueHelper::ParseInt(Args[2], Distance); }

		const FVector Forward = Player->GetControlRotation().Vector().GetSafeNormal2D();
		const FIntVector Feet = Player->GetFeetVoxel();
		const FIntVector Near(Feet.X + FMath::RoundToInt32(Forward.X * Distance), Feet.Y + FMath::RoundToInt32(Forward.Y * Distance), Feet.Z);
		const int32 Spawned = Animals->SpawnHerd(*Species, Near, FMath::Clamp(Count, 1, 32));
		UE_LOG(LogMadFallGameplay, Display, TEXT("Spawned %d %s near %s."), Spawned, *Species->Id.ToString(), *Near.ToString());
	}));

static FAutoConsoleCommandWithWorld GMadAnimalsKillAllCommand(
	TEXT("mad.animals.killall"),
	TEXT("Removes every animal."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadAnimalSubsystem* Animals = World ? World->GetSubsystem<UMadAnimalSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("Removed %d animal(s)."), Animals->KillAll());
		}
	}));

static FAutoConsoleCommandWithWorld GMadPlayerAimAnimalCommand(
	TEXT("mad.player.aimanimal"),
	TEXT("Turns the survivor to look at the nearest live animal."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		const UMadAnimalSubsystem* Animals = World ? World->GetSubsystem<UMadAnimalSubsystem>() : nullptr;
		AMadPlayerCharacter* Player = MadFall::FindLocalPlayer(World);
		if (Animals == nullptr || Player == nullptr)
		{
			return;
		}
		TArray<AMadAnimal*> Alive;
		Animals->GetAlive(Alive);
		AMadAnimal* Nearest = nullptr;
		double Best = TNumericLimits<double>::Max();
		for (AMadAnimal* Animal : Alive)
		{
			const double Distance = FVector::DistSquared(Animal->GetActorLocation(), Player->GetActorLocation());
			if (Distance < Best)
			{
				Best = Distance;
				Nearest = Animal;
			}
		}
		if (Nearest == nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("No animal to aim at."));
			return;
		}
		Player->AimAtLocation(Nearest->GetActorLocation());
		UE_LOG(LogMadFallGameplay, Display, TEXT("Aiming at %s, %.1f voxels away."), *Nearest->GetSpeciesId().ToString(),
			FMath::Sqrt(Best) / MadFall::VoxelSizeUU);
	}));

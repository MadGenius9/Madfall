// Copyright MadFall. All Rights Reserved.

#include "MadCompass.h"

#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "MadFallCoordinates.h"
#include "MadFallGameplay.h"
#include "MadLocalization.h"
#include "MadPickupSubsystem.h"
#include "MadPlayerCharacter.h"
#include "MadPoiPlanner.h"
#include "MadPrefabRegistry.h"
#include "MadTrading.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldGenerator.h"

TOptional<float> MadFall::Compass::ProjectBearing(float ViewYawDegrees, float BearingDegrees, float HalfSpanDegrees)
{
	const float Delta = FRotator::NormalizeAxis(BearingDegrees - ViewYawDegrees);
	if (HalfSpanDegrees <= 0.0f || FMath::Abs(Delta) > HalfSpanDegrees)
	{
		return TOptional<float>();
	}
	return Delta / HalfSpanDegrees;
}

float MadFall::Compass::BearingTo(const FVector& From, const FVector& To)
{
	const FVector2D Offset(To.X - From.X, To.Y - From.Y);
	if (Offset.IsNearlyZero())
	{
		return 0.0f;
	}
	// Unreal yaw: atan2(Y, X), 0 along +X. That is the compass frame already.
	return FRotator::ClampAxis(FMath::RadiansToDegrees(FMath::Atan2(Offset.Y, Offset.X)));
}

const TCHAR* MadFall::Compass::GetHeadingName(float BearingDegrees)
{
	static const TCHAR* Names[] = { TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"), TEXT("S"), TEXT("SW"), TEXT("W"), TEXT("NW") };
	const int32 Index = FMath::RoundToInt32(FRotator::ClampAxis(BearingDegrees) / 45.0f) % 8;
	return Names[Index];
}

void MadFall::Compass::GatherMarkers(const AMadPlayerCharacter& Player, TArray<FMadCompassMarker>& OutMarkers)
{
	OutMarkers.Reset();
	const UWorld* World = Player.GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (const TOptional<FIntVector> Bed = Player.GetBedVoxel(); Bed.IsSet())
	{
		OutMarkers.Add({ TEXT("Bed"), (FVector(Bed.GetValue()) + FVector(0.5)) * MadFall::VoxelSizeUU, FLinearColor(0.4f, 0.8f, 1.0f) });
	}

	for (TActorIterator<AMadItemPickup> It(World); It; ++It)
	{
		if (It->bIsBackpack && !It->IsEmpty())
		{
			OutMarkers.Add({ TEXT("Backpack"), It->GetActorLocation(), FLinearColor(1.0f, 0.75f, 0.2f) });
		}
	}

	// The trader outpost, from anywhere: a new player's first destination.
	if (const UMadTraderSubsystem* Traders = World->GetSubsystem<UMadTraderSubsystem>())
	{
		FIntVector TraderVoxel;
		if (Traders->GetOutpostTrader(TraderVoxel))
		{
			OutMarkers.Add({ TEXT("Trader"), (FVector(TraderVoxel) + FVector(0.5)) * MadFall::VoxelSizeUU, FLinearColor(0.95f, 0.85f, 0.3f) });
		}
	}

	// The building a clearing job points at, from any distance and in its own
	// colour: a job you cannot find is not a job.
	if (const TOptional<FIntVector>& Job = Player.GetJobSite(); Job.IsSet())
	{
		OutMarkers.Add({ FString::Printf(TEXT("Job: %s"), *Player.GetJobLabel()),
			(FVector(Job.GetValue()) + FVector(0.5)) * MadFall::VoxelSizeUU, FLinearColor(1.0f, 0.45f, 0.35f) });
	}

	// POIs are planned per cell and cached by the generator, so asking about the
	// few cells around the survivor each frame costs map lookups.
	const UMadVoxelWorldSubsystem* VoxelWorld = World->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	if (Generator == nullptr || !Generator->GetPoiPlanner().HasPrefabs())
	{
		return;
	}
	const FMadPoiPlanner& Planner = Generator->GetPoiPlanner();
	const FMadPrefabRegistry& Prefabs = UMadVoxelWorldSubsystem::GetPrefabRegistry();
	const int32 CellSize = FMath::Max(1, Planner.GetCellSizeVoxels());
	const FVector Here = Player.GetActorLocation();
	const int32 CellX = FMath::FloorToInt32(Here.X / MadFall::VoxelSizeUU / CellSize);
	const int32 CellY = FMath::FloorToInt32(Here.Y / MadFall::VoxelSizeUU / CellSize);
	const int32 Reach = FMath::CeilToInt32(PoiRangeVoxels / CellSize);

	for (int32 DY = -Reach; DY <= Reach; ++DY)
	{
		for (int32 DX = -Reach; DX <= Reach; ++DX)
		{
			FMadPoiInstance Poi;
			if (!Planner.PlanCell(*Generator, CellX + DX, CellY + DY, Poi) || Poi.PrefabIndex >= Prefabs.Num())
			{
				continue;
			}
			const FVector Centre = (FVector(Poi.Origin) + FVector(Poi.RotatedSize) * 0.5) * MadFall::VoxelSizeUU;
			if (FVector::Dist2D(Centre, Here) > PoiRangeVoxels * MadFall::VoxelSizeUU)
			{
				continue;
			}
			const FMadPrefab& Prefab = Prefabs.Get(Poi.PrefabIndex);
			if (Prefab.Placement.bNearSpawn)
			{
				continue;   // the outpost already has its Trader marker, from any distance
			}
			const FString Name = Prefab.DisplayName.IsEmpty() ? Prefab.Id.ToString() : MadFall::Localize(Prefab.DisplayName);
			OutMarkers.Add({ Name, Centre, FLinearColor(0.85f, 0.85f, 0.85f) });
		}
	}
}

static FAutoConsoleCommandWithWorld GMadCompassCommand(
	TEXT("mad.player.compass"),
	TEXT("Prints the heading and every compass marker with its bearing and distance."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		const AMadPlayerCharacter* Player = Controller ? Cast<AMadPlayerCharacter>(Controller->GetPawn()) : nullptr;
		if (Player == nullptr)
		{
			return;
		}
		const float Yaw = FRotator::ClampAxis(Player->GetControlRotation().Yaw);
		TArray<FMadCompassMarker> Markers;
		MadFall::Compass::GatherMarkers(*Player, Markers);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Compass: heading %s (%.0f), %d marker(s)."), MadFall::Compass::GetHeadingName(Yaw), Yaw, Markers.Num());
		for (const FMadCompassMarker& Marker : Markers)
		{
			const float Bearing = MadFall::Compass::BearingTo(Player->GetActorLocation(), Marker.Location);
			UE_LOG(LogMadFallGameplay, Display, TEXT("  %s: bearing %.0f (%s), %.0f m"), *Marker.Label, Bearing,
				MadFall::Compass::GetHeadingName(Bearing), FVector::Dist2D(Player->GetActorLocation(), Marker.Location) / 100.0);
		}
	}));

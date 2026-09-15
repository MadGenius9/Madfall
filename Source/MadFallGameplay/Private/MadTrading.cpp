// Copyright MadFall. All Rights Reserved.

#include "MadTrading.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadFallCoordinates.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadGameplayDefinitions.h"
#include "MadHumanoidRig.h"
#include "MadLocalization.h"
#include "MadPlayerCharacter.h"
#include "MadPoiPlanner.h"
#include "MadPrefabRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "MadWorldGenerator.h"

// ===========================================================================
// Rules
// ===========================================================================

namespace MadFall::Trade
{
	const TCHAR* ToString(EMadTradeResult Result)
	{
		switch (Result)
		{
		case EMadTradeResult::Ok:           return TEXT("ok");
		case EMadTradeResult::NotForSale:   return TEXT("not for sale");
		case EMadTradeResult::NotBought:    return TEXT("the trader does not buy that");
		case EMadTradeResult::CannotAfford: return TEXT("not enough coins");
		case EMadTradeResult::NoSpace:      return TEXT("no room in your backpack");
		default:                            return TEXT("?");
		}
	}

	void Restock(FMadTraderState& State, const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions,
		FRandomStream& Random, int32 Day)
	{
		State.Trader = Trader.Id;
		State.Stock = FMadInventory(FMadTraderState::StockSlots);
		for (const FMadTraderStockEntry& Entry : Trader.Stock)
		{
			const FMadItemDefinition* Item = Definitions.FindItem(Entry.Item);
			if (Item == nullptr || Random.FRand() >= Entry.Chance)
			{
				continue;
			}
			const int32 Count = Random.RandRange(Entry.CountMin, Entry.CountMax);
			// A full shelf drops the rest of the line: 24 slots is plenty for a
			// definition, and a mod listing more simply sells fewer at once.
			State.Stock.Add(FMadItemStack::Make(*Item, Count), Definitions);
		}
		State.RestockDay = Day + FMath::Max(1, Trader.RestockDays);
	}

	int32 GetUnitPrice(const FMadItemStack& Stack, const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions)
	{
		return Stack.IsEmpty() ? 0 : Definitions.GetTraderPrice(Trader, Stack.Item);
	}

	int32 GetUnitOffer(const FMadItemStack& Stack, const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions)
	{
		if (Stack.IsEmpty())
		{
			return 0;
		}
		const int32 Offer = Definitions.GetTraderOffer(Trader, Stack.Item);
		const FMadItemDefinition* Item = Definitions.FindItem(Stack.Item);
		if (Offer <= 0 || Item == nullptr || !Item->bHasTool || Item->Tool.Durability <= 0 || Stack.Durability < 0)
		{
			return Offer;
		}
		const float Left = FMath::Clamp(static_cast<float>(Stack.Durability) / Item->Tool.Durability, 0.0f, 1.0f);
		return FMath::Max(1, FMath::FloorToInt32(Offer * Left));
	}

	EMadTradeResult Buy(FMadInventory& Backpack, FMadTraderState& State, int32 StockSlot, int32 Count,
		const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions, int32& OutTotal)
	{
		OutTotal = 0;
		if (StockSlot < 0 || StockSlot >= State.Stock.NumSlots())
		{
			return EMadTradeResult::NotForSale;
		}
		const FMadItemStack Shelf = State.Stock.GetSlot(StockSlot);
		const int32 Price = GetUnitPrice(Shelf, Trader, Definitions);
		if (Price <= 0)
		{
			return EMadTradeResult::NotForSale;
		}
		Count = FMath::Clamp(Count, 1, Shelf.Count);
		const int32 Total = Price * Count;
		if (Backpack.CountItem(Trader.Currency) < Total)
		{
			return EMadTradeResult::CannotAfford;
		}

		FMadInventory Working = Backpack;
		Working.Remove(Trader.Currency, Total);
		FMadItemStack Bought = Shelf;
		Bought.Count = Count;
		if (Working.Add(Bought, Definitions) > 0)
		{
			return EMadTradeResult::NoSpace;
		}

		Backpack = MoveTemp(Working);
		State.Stock.TakeFromSlot(StockSlot, Count);
		OutTotal = Total;
		return EMadTradeResult::Ok;
	}

	EMadTradeResult Sell(FMadInventory& Backpack, int32 BackpackSlot, int32 Count, FMadTraderState& State,
		const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions, int32& OutTotal)
	{
		OutTotal = 0;
		if (BackpackSlot < 0 || BackpackSlot >= Backpack.NumSlots() || Backpack.GetSlot(BackpackSlot).IsEmpty())
		{
			return EMadTradeResult::NotBought;
		}
		const FMadItemStack Offered = Backpack.GetSlot(BackpackSlot);
		const int32 Offer = GetUnitOffer(Offered, Trader, Definitions);
		if (Offer <= 0)
		{
			return EMadTradeResult::NotBought;
		}
		Count = FMath::Clamp(Count, 1, Offered.Count);
		const int32 Total = Offer * Count;

		const FMadItemDefinition* Currency = Definitions.FindItem(Trader.Currency);
		if (Currency == nullptr)
		{
			return EMadTradeResult::NotBought;
		}

		FMadInventory Working = Backpack;
		FMadItemStack Sold = Working.TakeFromSlot(BackpackSlot, Count);
		// The slot just emptied (or shrank) is where the coins may go.
		if (Working.Add(FMadItemStack::Make(*Currency, Total), Definitions) > 0)
		{
			return EMadTradeResult::NoSpace;
		}

		Backpack = MoveTemp(Working);
		State.Stock.Add(Sold, Definitions);
		OutTotal = Total;
		return EMadTradeResult::Ok;
	}
}

// ===========================================================================
// Actor
// ===========================================================================

AMadTrader::AMadTrader()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;

	GetCapsuleComponent()->InitCapsuleSize(34.0f, 90.0f);
	AutoPossessAI = EAutoPossessAI::Disabled;

	// No controller, but it still has to stand on the ground it spawns above.
	GetCharacterMovement()->bRunPhysicsWithNoController = true;
	GetCharacterMovement()->GravityScale = 1.0f;

	Body = CreateDefaultSubobject<UMadHumanoidRigComponent>(TEXT("Body"));
	Body->SetupAttachment(GetCapsuleComponent());
	Body->SetRelativeLocation(FVector(0.0, 0.0, -90.0));
}

void AMadTrader::Setup(FName InTraderId, const FIntVector& InMarkerVoxel)
{
	TraderId = InTraderId;
	MarkerVoxel = InMarkerVoxel;
	if (Body != nullptr)
	{
		// Living skin, a face and a clean jacket: a trader reads as not a zombie at a
		// glance. Linear values: a warm skin, a dark blue jacket, brown work trousers.
		Body->SetColours(FLinearColor(0.34f, 0.18f, 0.1f), FLinearColor(0.03f, 0.07f, 0.18f), FLinearColor(0.09f, 0.06f, 0.035f),
			FLinearColor(0.04f, 0.025f, 0.015f));
		Body->SetLiving(true);
	}
}

void AMadTrader::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Turn to face the survivor when they are close.
	const APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const APawn* Player = PlayerController ? PlayerController->GetPawn() : nullptr;
	if (Player != nullptr && FVector::DistSquared2D(Player->GetActorLocation(), GetActorLocation()) < FMath::Square(8.0 * MadFall::VoxelSizeUU))
	{
		const FRotator Want = (Player->GetActorLocation() - GetActorLocation()).GetSafeNormal2D().Rotation();
		SetActorRotation(FMath::RInterpTo(GetActorRotation(), FRotator(0.0, Want.Yaw, 0.0), DeltaSeconds, 4.0f));
	}
}

// ===========================================================================
// Subsystem
// ===========================================================================

namespace
{
	const FName TraderMarker(TEXT("trader"));

	AMadPlayerCharacter* FindPlayer(const UWorld* World)
	{
		const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		return Controller ? Cast<AMadPlayerCharacter>(Controller->GetPawn()) : nullptr;
	}
}

bool UMadTraderSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadTraderSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadTraderSubsystem, STATGROUP_Tickables);
}

void UMadTraderSubsystem::Restock(const FIntVector& MarkerVoxel, FMadTraderState& State, const FMadTraderDefinition& Trader, int32 Day) const
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const int64 Seed = VoxelWorld ? VoxelWorld->GetSeed() : 0;
	// Seeded by world, position and day: the same shelves for the same visit,
	// different shelves at each outpost and after each restock.
	FRandomStream Random(static_cast<int32>(HashCombine(HashCombine(GetTypeHash(Seed), GetTypeHash(MarkerVoxel)), GetTypeHash(Day))));
	MadFall::Trade::Restock(State, Trader, MadFall::GetGameplayDefinitions(), Random, Day);
}

FMadTraderState* UMadTraderSubsystem::GetState(const FIntVector& MarkerVoxel, FName TraderId)
{
	const FMadTraderDefinition* Trader = MadFall::GetGameplayDefinitions().FindTrader(TraderId);
	if (Trader == nullptr)
	{
		return nullptr;
	}
	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	const int32 Day = Clock ? Clock->GetDay() : 1;

	FMadTraderState* State = States.Find(MarkerVoxel);
	if (State == nullptr || State->Trader != TraderId || Day >= State->RestockDay)
	{
		FMadTraderState& Fresh = States.FindOrAdd(MarkerVoxel);
		Restock(MarkerVoxel, Fresh, *Trader, Day);
		UE_LOG(LogMadFallGameplay, Log, TEXT("Trader %s at %s restocked for day %d."), *TraderId.ToString(), *MarkerVoxel.ToString(), Day);
		return &Fresh;
	}
	return State;
}

AMadTrader* UMadTraderSubsystem::FindActor(const FIntVector& MarkerVoxel) const
{
	const TWeakObjectPtr<AMadTrader>* Found = Actors.Find(MarkerVoxel);
	return Found ? Found->Get() : nullptr;
}

AMadTrader* UMadTraderSubsystem::EnsureActor(const FIntVector& MarkerVoxel, FName TraderId)
{
	if (AMadTrader* Existing = FindActor(MarkerVoxel))
	{
		return Existing;
	}
	// The marker is the voxel the trader's feet stand in.
	const FVector Location = (FVector(MarkerVoxel) + FVector(0.5, 0.5, 0.0)) * MadFall::VoxelSizeUU + FVector(0.0, 0.0, 92.0);
	FActorSpawnParameters Parameters;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	AMadTrader* Trader = GetWorld()->SpawnActor<AMadTrader>(AMadTrader::StaticClass(), Location, FRotator::ZeroRotator, Parameters);
	if (Trader != nullptr)
	{
		Trader->Setup(TraderId, MarkerVoxel);
		Actors.Add(MarkerVoxel, Trader);
		UE_LOG(LogMadFallGameplay, Display, TEXT("Trader %s is at %s."), *TraderId.ToString(), *MarkerVoxel.ToString());
	}
	return Trader;
}

AMadTrader* UMadTraderSubsystem::SpawnManual(FName TraderId, const FIntVector& Feet)
{
	if (MadFall::GetGameplayDefinitions().FindTrader(TraderId) == nullptr)
	{
		return nullptr;
	}
	Manual.Add(Feet, TraderId);
	return EnsureActor(Feet, TraderId);
}

void UMadTraderSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	Timer -= DeltaTime;
	if (Timer > 0.0f)
	{
		return;
	}
	Timer = 1.0f;

	AMadPlayerCharacter* Player = FindPlayer(GetWorld());
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (Player == nullptr || Player->IsWaitingForWorld() || VoxelWorld == nullptr)
	{
		return;
	}

	MAD_FRAME_SCOPE(Other);

	const FIntVector Feet = Player->GetFeetVoxel();
	auto WithinRadius = [&Feet](const FIntVector& Voxel, int32 Radius)
	{
		return FMath::Square(Voxel.X - Feet.X) + FMath::Square(Voxel.Y - Feet.Y) <= FMath::Square(Radius) && FMath::Abs(Voxel.Z - Feet.Z) <= Radius;
	};

	// Trader markers of the POIs around the survivor, as sleepers find theirs.
	TMap<FIntVector, FName> Wanted;
	for (const TPair<FIntVector, FName>& Pair : Manual)
	{
		Wanted.Add(Pair.Key, Pair.Value);
	}
	if (const FMadWorldGenerator* Generator = VoxelWorld->GetWorldGenerator())
	{
		const FMadPoiPlanner& Planner = Generator->GetPoiPlanner();
		TSet<FIntPoint> Seen;
		TArray<FMadPoiInstance> Pois;
		TArray<FMadPoiWorldMarker> Markers;
		for (int32 DY = -2; DY <= 2; ++DY)
		{
			for (int32 DX = -2; DX <= 2; ++DX)
			{
				const FMadChunkCoord Coord = MadFall::WorldToChunk(Feet.X + DX * MadFall::ChunkSize, Feet.Y + DY * MadFall::ChunkSize, Feet.Z);
				Planner.GetPoisOverlappingChunk(*Generator, Coord, Pois);
				for (const FMadPoiInstance& Poi : Pois)
				{
					bool bSeen = false;
					Seen.Add(Poi.Cell, &bSeen);
					if (bSeen)
					{
						continue;
					}
					Planner.GetWorldMarkers(Poi, Markers);
					for (const FMadPoiWorldMarker& Marker : Markers)
					{
						if (Marker.Type == TraderMarker && !Marker.Trader.IsNone())
						{
							Wanted.Add(Marker.WorldPosition, Marker.Trader);
						}
					}
				}
			}
		}
	}

	for (const TPair<FIntVector, FName>& Pair : Wanted)
	{
		// Only over loaded ground, or the trader falls through the world.
		if (WithinRadius(Pair.Key, SpawnRadius) && VoxelWorld->IsVoxelLoaded(Pair.Key.X, Pair.Key.Y, Pair.Key.Z - 1))
		{
			EnsureActor(Pair.Key, Pair.Value);
		}
	}

	for (auto It = Actors.CreateIterator(); It; ++It)
	{
		AMadTrader* Trader = It->Value.Get();
		if (Trader == nullptr)
		{
			It.RemoveCurrent();
		}
		else if (!WithinRadius(It->Key, DespawnRadius))
		{
			Trader->Destroy();
			It.RemoveCurrent();
		}
	}
}

bool UMadTraderSubsystem::GetOutpostTrader(FIntVector& OutVoxel) const
{
	if (!bOutpostResolved)
	{
		const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
		const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
		if (Generator == nullptr)
		{
			return false;   // try again once the generator exists
		}
		bOutpostResolved = true;

		const FMadPoiPlanner& Planner = Generator->GetPoiPlanner();
		const FMadPrefabRegistry& Prefabs = UMadVoxelWorldSubsystem::GetPrefabRegistry();
		for (int32 Index = 0; Index < Prefabs.Num() && !Outpost.IsSet(); ++Index)
		{
			const FMadPrefab& Prefab = Prefabs.Get(Index);
			FIntPoint Cell;
			FMadPoiInstance Poi;
			if (!Prefab.Placement.bNearSpawn || !Planner.GetNearSpawnCell(*Generator, Prefab.Id, Cell) || !Planner.PlanCell(*Generator, Cell.X, Cell.Y, Poi))
			{
				continue;
			}
			TArray<FMadPoiWorldMarker> Markers;
			Planner.GetWorldMarkers(Poi, Markers);
			for (const FMadPoiWorldMarker& Marker : Markers)
			{
				if (Marker.Type == TraderMarker && !Outpost.IsSet())
				{
					Outpost = Marker.WorldPosition;
				}
				else if (Marker.Type == FName(TEXT("entrance")))
				{
					OutpostEntrance = Marker.WorldPosition;
				}
			}
		}
	}
	if (Outpost.IsSet())
	{
		OutVoxel = Outpost.GetValue();
		return true;
	}
	return false;
}

bool UMadTraderSubsystem::GetOutpostApproach(FIntVector& OutVoxel) const
{
	FIntVector Trader;
	if (!GetOutpostTrader(Trader))
	{
		return false;
	}
	const FIntVector Entrance = OutpostEntrance.Get(Trader);
	// Three tenths of the way from the trader to the gate: in front of the stall.
	OutVoxel = FIntVector(Trader.X + FMath::RoundToInt32((Entrance.X - Trader.X) * 0.3f), Trader.Y + FMath::RoundToInt32((Entrance.Y - Trader.Y) * 0.3f), Trader.Z);
	return true;
}

int32 UMadTraderSubsystem::RestockAll()
{
	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	const int32 Day = Clock ? Clock->GetDay() : 1;
	int32 Count = 0;
	for (TPair<FIntVector, FMadTraderState>& Pair : States)
	{
		if (const FMadTraderDefinition* Trader = MadFall::GetGameplayDefinitions().FindTrader(Pair.Value.Trader))
		{
			Restock(Pair.Key, Pair.Value, *Trader, Day);
			++Count;
		}
	}
	return Count;
}

void UMadTraderSubsystem::ExportState(TArray<FMadTraderSaveData>& Out) const
{
	Out.Reset();
	for (const TPair<FIntVector, FMadTraderState>& Pair : States)
	{
		FMadTraderSaveData& Data = Out.AddDefaulted_GetRef();
		Data.Position = Pair.Key;
		Data.Trader = Pair.Value.Trader;
		Data.RestockDay = Pair.Value.RestockDay;
		Data.Stock = Pair.Value.Stock.GetSlots();
	}
	Out.Sort([](const FMadTraderSaveData& A, const FMadTraderSaveData& B)
	{
		if (A.Position.X != B.Position.X) { return A.Position.X < B.Position.X; }
		if (A.Position.Y != B.Position.Y) { return A.Position.Y < B.Position.Y; }
		return A.Position.Z < B.Position.Z;
	});
}

void UMadTraderSubsystem::ImportState(const TArray<FMadTraderSaveData>& In)
{
	States.Reset();
	for (const FMadTraderSaveData& Data : In)
	{
		FMadTraderState& State = States.Add(Data.Position);
		State.Trader = Data.Trader;
		State.RestockDay = Data.RestockDay;
		// Kept even for a trader whose mod is gone: GetState only restocks when asked with a known id.
		for (int32 Slot = 0; Slot < Data.Stock.Num() && Slot < State.Stock.NumSlots(); ++Slot)
		{
			State.Stock.SetSlot(Slot, Data.Stock[Slot]);
		}
	}
}

FString UMadTraderSubsystem::DescribeStatus() const
{
	FString Out = FString::Printf(TEXT("Traders: %d with shelves, %d standing"), States.Num(), Actors.Num());
	FIntVector OutpostVoxel;
	if (GetOutpostTrader(OutpostVoxel))
	{
		Out += FString::Printf(TEXT(", outpost trader at %s"), *OutpostVoxel.ToString());
	}
	for (const TPair<FIntVector, FMadTraderState>& Pair : States)
	{
		int32 Stacks = 0;
		for (const FMadItemStack& Stack : Pair.Value.Stock.GetSlots())
		{
			Stacks += Stack.IsEmpty() ? 0 : 1;
		}
		Out += FString::Printf(TEXT("\n  %s at %s: %d stack(s), restocks day %d"), *Pair.Value.Trader.ToString(), *Pair.Key.ToString(), Stacks, Pair.Value.RestockDay);
	}
	return Out;
}

namespace
{
	FAutoConsoleCommandWithWorld GMadTraderStatus(
		TEXT("mad.trader.status"), TEXT("Traders with shelves, standing traders, and the outpost trader's position."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (const UMadTraderSubsystem* Traders = World ? World->GetSubsystem<UMadTraderSubsystem>() : nullptr)
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Traders->DescribeStatus());
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GMadTraderSpawn(
		TEXT("mad.trader.spawn"), TEXT("mad.trader.spawn <trader id> - a trader three voxels ahead of the survivor."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
		{
			UMadTraderSubsystem* Traders = World ? World->GetSubsystem<UMadTraderSubsystem>() : nullptr;
			AMadPlayerCharacter* Player = FindPlayer(World);
			if (Traders == nullptr || Player == nullptr || Args.Num() < 1)
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.trader.spawn <trader id> (needs a survivor)"));
				return;
			}
			const FVector Forward = Player->GetControlRotation().Vector().GetSafeNormal2D();
			const FIntVector Feet = Player->GetFeetVoxel() + FIntVector(FMath::RoundToInt32(Forward.X * 3.0), FMath::RoundToInt32(Forward.Y * 3.0), 0);
			const AMadTrader* Trader = Traders->SpawnManual(FName(*Args[0]), Feet);
			UE_LOG(LogMadFallGameplay, Display, TEXT("Spawn trader %s: %s"), *Args[0], Trader ? *Feet.ToString() : TEXT("unknown trader"));
		}));

	FAutoConsoleCommandWithWorld GMadTraderGoto(
		TEXT("mad.trader.goto"), TEXT("Travels the survivor into the trader outpost, facing the trader."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			const UMadTraderSubsystem* Traders = World ? World->GetSubsystem<UMadTraderSubsystem>() : nullptr;
			AMadPlayerCharacter* Player = FindPlayer(World);
			FIntVector Approach;
			FIntVector TraderVoxel;
			if (Traders == nullptr || Player == nullptr || !Traders->GetOutpostApproach(Approach) || !Traders->GetOutpostTrader(TraderVoxel))
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("mad.trader.goto: this world has no trader outpost"));
				return;
			}
			Player->TravelToVoxel(Approach);
			Player->AimAtVoxel(TraderVoxel + FIntVector(0, 0, 1));
			UE_LOG(LogMadFallGameplay, Display, TEXT("Travelling to the outpost at %s."), *Approach.ToString());
		}));

	FAutoConsoleCommandWithWorld GMadTraderAim(
		TEXT("mad.trader.aim"), TEXT("Turns the survivor to face the nearest standing trader."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			AMadPlayerCharacter* Player = FindPlayer(World);
			AMadTrader* Nearest = nullptr;
			for (TActorIterator<AMadTrader> It(World); It && Player; ++It)
			{
				if (Nearest == nullptr || FVector::DistSquared(It->GetActorLocation(), Player->GetActorLocation()) < FVector::DistSquared(Nearest->GetActorLocation(), Player->GetActorLocation()))
				{
					Nearest = *It;
				}
			}
			if (Nearest == nullptr)
			{
				UE_LOG(LogMadFallGameplay, Warning, TEXT("mad.trader.aim: no trader is standing near the survivor"));
				return;
			}
			Player->AimAtLocation(Nearest->GetActorLocation() + FVector(0.0, 0.0, 30.0));
			UE_LOG(LogMadFallGameplay, Display, TEXT("Facing trader %s, %.1f m away."), *Nearest->GetTraderId().ToString(),
				FVector::Dist(Nearest->GetActorLocation(), Player->GetActorLocation()) / 100.0);
		}));

	FAutoConsoleCommandWithWorld GMadTraderRestock(
		TEXT("mad.trader.restock"), TEXT("Restocks every trader with shelves now."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (UMadTraderSubsystem* Traders = World ? World->GetSubsystem<UMadTraderSubsystem>() : nullptr)
			{
				UE_LOG(LogMadFallGameplay, Display, TEXT("Restocked %d trader(s)."), Traders->RestockAll());
			}
		}));
}

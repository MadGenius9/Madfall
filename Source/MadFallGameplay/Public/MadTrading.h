// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MadGameplaySave.h"
#include "MadInventory.h"
#include "Math/RandomStream.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadTrading.generated.h"

class AMadPlayerCharacter;
class UMadHumanoidRigComponent;
struct FMadTraderDefinition;

/** Why a trade did or did not happen. */
enum class EMadTradeResult : uint8
{
	Ok,
	/** The slot is empty, or the trader does not price the item. */
	NotForSale,
	/** The trader will not buy the item (no value, wrong tag, or it is the currency). */
	NotBought,
	CannotAfford,
	/** What was bought (or the payment for what was sold) does not fit in the backpack. */
	NoSpace
};

/** One trader's shelves: what is for sale at one trader position, and when it restocks. */
struct MADFALLGAMEPLAY_API FMadTraderState
{
	static constexpr int32 StockSlots = 24;

	FName Trader;
	FMadInventory Stock{ StockSlots };

	/** The game day this trader next restocks. */
	int32 RestockDay = 0;
};

/**
 * Buying and selling, as plain functions over inventories, so the rules are
 * testable without a world.
 *
 * Every trade is atomic: it is worked out on a copy of the backpack and
 * committed only if everything fits, so a full backpack never takes the coins
 * and drops the goods, or takes the goods and loses the coins.
 */
namespace MadFall::Trade
{
	MADFALLGAMEPLAY_API const TCHAR* ToString(EMadTradeResult Result);

	/** Clears the shelves and rolls them again from the definition. */
	MADFALLGAMEPLAY_API void Restock(FMadTraderState& State, const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions,
		FRandomStream& Random, int32 Day);

	/** What the trader asks for one of a stack on their shelf. 0: not for sale. */
	MADFALLGAMEPLAY_API int32 GetUnitPrice(const FMadItemStack& Stack, const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions);

	/**
	 * What the trader pays for one of a stack from the backpack. 0: they will not
	 * buy it. A worn tool fetches its offer scaled by the durability left.
	 */
	MADFALLGAMEPLAY_API int32 GetUnitOffer(const FMadItemStack& Stack, const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions);

	/** Buys up to Count from a shelf slot. OutTotal is what was paid. */
	MADFALLGAMEPLAY_API EMadTradeResult Buy(FMadInventory& Backpack, FMadTraderState& State, int32 StockSlot, int32 Count,
		const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions, int32& OutTotal);

	/** Sells up to Count from a backpack slot. What is sold goes onto the shelf if there is room. OutTotal is what was earned. */
	MADFALLGAMEPLAY_API EMadTradeResult Sell(FMadInventory& Backpack, int32 BackpackSlot, int32 Count, FMadTraderState& State,
		const FMadTraderDefinition& Trader, const FMadGameplayDefinitions& Definitions, int32& OutTotal);
}

/**
 * A trader standing at a trader marker. Not a zombie target and not damageable:
 * a trader who can die is a softlock for a player who needed them.
 */
UCLASS()
class MADFALLGAMEPLAY_API AMadTrader : public ACharacter
{
	GENERATED_BODY()

public:
	AMadTrader();

	virtual void Tick(float DeltaSeconds) override;

	void Setup(FName InTraderId, const FIntVector& InMarkerVoxel);

	FName GetTraderId() const { return TraderId; }
	const FIntVector& GetMarkerVoxel() const { return MarkerVoxel; }

private:
	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UMadHumanoidRigComponent> Body;

	FName TraderId;
	FIntVector MarkerVoxel = FIntVector::ZeroValue;
};

/**
 * Traders in a game world: their shelves, the actors standing at trader
 * markers near the survivor, and the outpost the compass points to.
 *
 * Shelves are kept per trader position, saved with the world, and restocked
 * lazily - the first time anyone looks after the restock day - so a trader
 * nobody visits costs nothing. Actors exist only near the survivor, like
 * sleepers, and carry no state of their own.
 *
 *   `mad.trader.status`          traders, positions, restock days
 *   `mad.trader.spawn <id>`      a trader three voxels ahead (testing)
 *   `mad.trader.restock`         restocks every known trader now
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadTraderSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** A trader's shelves, restocked first if the restock day has come. Null for an unknown trader id. */
	FMadTraderState* GetState(const FIntVector& MarkerVoxel, FName TraderId);

	/** The actor at a marker, if one is spawned. */
	AMadTrader* FindActor(const FIntVector& MarkerVoxel) const;

	/** A trader outside any POI (tests, console). Remembered for the session so the actor stays. */
	AMadTrader* SpawnManual(FName TraderId, const FIntVector& Feet);

	/** Where the near-spawn trader outpost's trader stands, if the world has one. */
	bool GetOutpostTrader(FIntVector& OutVoxel) const;

	/** A voxel inside the outpost, between its entrance and the trader: where `mad.trader.goto` goes. */
	bool GetOutpostApproach(FIntVector& OutVoxel) const;

	/** Restocks every trader with state now. */
	int32 RestockAll();

	void ExportState(TArray<FMadTraderSaveData>& Out) const;
	void ImportState(const TArray<FMadTraderSaveData>& In);

	FString DescribeStatus() const;

	/** Traders within this many voxels of the survivor stand at their markers. */
	static constexpr int32 SpawnRadius = 64;

	/** And are removed past this. */
	static constexpr int32 DespawnRadius = 96;

private:
	void Restock(const FIntVector& MarkerVoxel, FMadTraderState& State, const FMadTraderDefinition& Trader, int32 Day) const;
	AMadTrader* EnsureActor(const FIntVector& MarkerVoxel, FName TraderId);

	TMap<FIntVector, FMadTraderState> States;
	TMap<FIntVector, TWeakObjectPtr<AMadTrader>> Actors;
	TMap<FIntVector, FName> Manual;

	float Timer = 0.0f;

	mutable bool bOutpostResolved = false;
	mutable TOptional<FIntVector> Outpost;
	mutable TOptional<FIntVector> OutpostEntrance;
};

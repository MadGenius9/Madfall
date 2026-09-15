// Copyright MadFall. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "MadInventory.h"
#include "MadSurvivalModel.h"
#include "MadSurvivorComponents.generated.h"

class UAbilitySystemComponent;

DECLARE_MULTICAST_DELEGATE(FMadOnInventoryChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FMadOnSurvivorDied, const FMadSurvivalStepResult& /*LastCauses*/);

/**
 * A survivor's backpack. The first HotbarSlots slots are the hotbar.
 */
UCLASS(ClassGroup = (MadFall), meta = (BlueprintSpawnableComponent))
class MADFALLGAMEPLAY_API UMadInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMadInventoryComponent();

	static constexpr int32 NumSlots = 36;
	static constexpr int32 HotbarSlots = 9;

	FMadInventory& GetInventory() { return Inventory; }
	const FMadInventory& GetInventory() const { return Inventory; }

	/** Clothing and armour being worn: head, body, legs, feet (MadFall::Wear slot order). */
	FMadInventory& GetWorn() { return Worn; }
	const FMadInventory& GetWorn() const { return Worn; }

	/** What everything worn adds up to. Armour is capped at MadFall::Wear::MaxArmor. */
	FMadWearStats GetWearTotals() const;

	/**
	 * Wears the selected hotbar item if it is clothing, swapping out whatever
	 * was in its slot. Returns false if the selected item cannot be worn.
	 */
	bool WearSelected();

	int32 GetSelectedSlot() const { return SelectedSlot; }
	void SelectSlot(int32 Slot);

	/** The selected hotbar stack, or nullptr if the slot is empty. */
	const FMadItemStack* GetSelectedStack() const;

	/** Adds items by id. Returns how many did not fit. */
	int32 AddItem(FName Item, int32 Count);
	int32 AddStack(const FMadItemStack& Stack);

	/** Replaces the selected stack (after using durability, placing a block, and so on). */
	void SetSelectedStack(const FMadItemStack& Stack);

	/** Call after mutating GetInventory() directly. */
	void NotifyChanged() { ChangedDelegate.Broadcast(); }

	FMadOnInventoryChanged& OnChanged() { return ChangedDelegate; }

	FString Describe() const;

private:
	FMadInventory Inventory{ NumSlots };
	FMadInventory Worn{ MadFall::Wear::NumSlots };
	int32 SelectedSlot = 0;
	FMadOnInventoryChanged ChangedDelegate;
};

/**
 * Runs MadFall::Survival::Step against the owner's GAS survival attributes.
 */
UCLASS(ClassGroup = (MadFall), meta = (BlueprintSpawnableComponent))
class MADFALLGAMEPLAY_API UMadSurvivalComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMadSurvivalComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	FMadSurvivalStats GetStats() const;
	void SetStats(const FMadSurvivalStats& Stats);

	/** Instant effects from a consumable. */
	void ApplyEffects(const TMap<FName, float>& Effects);

	/** Direct health damage (falls, debris, the console). */
	void ApplyDamage(float Amount);

	/** A zombie or animal attack: armour absorbs its share first. Returns the damage taken. */
	float ApplyAttackDamage(float Amount);

	/** From worn clothing. */
	void SetWear(const FMadWearStats& Totals)
	{
		Environment.ColdInsulation = Totals.Cold;
		Environment.HeatInsulation = Totals.Heat;
		Armor = FMath::Clamp(Totals.Armor, 0.0f, MadFall::Wear::MaxArmor);
	}

	float GetArmor() const { return Armor; }
	float GetColdInsulation() const { return Environment.ColdInsulation; }
	float GetHeatInsulation() const { return Environment.HeatInsulation; }

	/** Spends stamina if there is enough. */
	bool TrySpendStamina(float Amount);

	void SetSprinting(bool bInSprinting) { Environment.bSprinting = bInSprinting; }

	/** Perk multipliers on food and water loss. */
	void SetDrainMultipliers(float Food, float Water) { Environment.FoodDrainMultiplier = Food; Environment.WaterDrainMultiplier = Water; }

	/** Sets maximum health and stamina (perks), keeping current values within the new maxima. */
	void SetMaxVitals(float MaxHealth, float MaxStamina);

	/** Marks the survivor as exerting for a short window (a swing). */
	void NoteExertion(float Seconds) { ExertionRemaining = FMath::Max(ExertionRemaining, Seconds); }

	float GetAmbientTemperature() const { return Environment.AmbientTemperature; }
	const FMadSurvivalStepResult& GetLastCauses() const { return LastCauses; }

	bool IsDead() const { return GetStats().IsDead(); }

	FMadOnSurvivorDied& OnDied() { return DiedDelegate; }

	FMadSurvivalTuning Tuning;

private:
	UAbilitySystemComponent* FindAbilitySystem() const;
	void UpdateAmbient();

	FMadSurvivalEnvironment Environment;
	FMadSurvivalStepResult LastCauses;
	float ExertionRemaining = 0.0f;
	float Armor = 0.0f;
	bool bWasDead = false;
	FMadOnSurvivorDied DiedDelegate;
};

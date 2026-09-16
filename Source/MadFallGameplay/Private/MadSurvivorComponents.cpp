// Copyright MadFall. All Rights Reserved.

#include "MadSurvivorComponents.h"

#include "MadFrameBudget.h"
#include "AbilitySystemInterface.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MadFallGameplay.h"
#include "MadGameplayDefinitions.h"
#include "MadProgression.h"
#include "MadSurvivalAttributeSet.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWeather.h"
#include "MadWorldClockSubsystem.h"
#include "MadWorldGenerator.h"
#include "Misc/StringBuilder.h"

// ===========================================================================
// Inventory
// ===========================================================================

UMadInventoryComponent::UMadInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UMadInventoryComponent::SelectSlot(int32 Slot)
{
	SelectedSlot = ((Slot % HotbarSlots) + HotbarSlots) % HotbarSlots;
	ChangedDelegate.Broadcast();
}

const FMadItemStack* UMadInventoryComponent::GetSelectedStack() const
{
	const FMadItemStack& Stack = Inventory.GetSlot(SelectedSlot);
	return Stack.IsEmpty() ? nullptr : &Stack;
}

int32 UMadInventoryComponent::AddItem(FName Item, int32 Count)
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	const FMadItemDefinition* Definition = Definitions.FindItem(Item);
	if (Definition == nullptr || Count <= 0)
	{
		return Count;
	}

	// Equipment is added one piece at a time so each gets its own durability.
	int32 Left = 0;
	if (Definition->MaxStack == 1)
	{
		for (int32 Piece = 0; Piece < Count; ++Piece)
		{
			Left += Inventory.Add(FMadItemStack::Make(*Definition, 1), Definitions);
		}
	}
	else
	{
		Left = Inventory.Add(FMadItemStack::Make(*Definition, Count), Definitions);
	}

	ChangedDelegate.Broadcast();
	return Left;
}

int32 UMadInventoryComponent::AddStack(const FMadItemStack& Stack)
{
	const int32 Left = Inventory.Add(Stack, MadFall::GetGameplayDefinitions());
	ChangedDelegate.Broadcast();
	return Left;
}

void UMadInventoryComponent::SetSelectedStack(const FMadItemStack& Stack)
{
	Inventory.SetSlot(SelectedSlot, Stack.IsEmpty() ? FMadItemStack() : Stack);
	ChangedDelegate.Broadcast();
}

FMadWearStats UMadInventoryComponent::GetWearTotals() const
{
	const FMadGameplayDefinitions& Definitions = MadFall::GetGameplayDefinitions();
	FMadWearStats Totals;
	for (int32 Slot = 0; Slot < Worn.NumSlots(); ++Slot)
	{
		const FMadItemStack& Stack = Worn.GetSlot(Slot);
		const FMadItemDefinition* Item = Stack.IsEmpty() ? nullptr : Definitions.FindItem(Stack.Item);
		if (Item != nullptr && Item->bHasWear)
		{
			Totals.Cold += Item->Wear.Cold;
			Totals.Heat += Item->Wear.Heat;
			Totals.Armor += Item->Wear.Armor;
		}
	}
	Totals.Armor = FMath::Min(Totals.Armor, MadFall::Wear::MaxArmor);
	return Totals;
}

bool UMadInventoryComponent::WearSelected()
{
	const FMadItemStack Selected = Inventory.GetSlot(SelectedSlot);
	const FMadItemDefinition* Item = Selected.IsEmpty() ? nullptr : MadFall::GetGameplayDefinitions().FindItem(Selected.Item);
	const int32 WornSlot = Item != nullptr && Item->bHasWear ? MadFall::Wear::GetSlotIndex(Item->Wear.Slot) : INDEX_NONE;
	if (WornSlot == INDEX_NONE)
	{
		return false;
	}

	// One piece is worn; the rest of a stack (clothes rarely stack) stays in hand.
	FMadItemStack Piece = Selected;
	Piece.Count = 1;
	FMadItemStack Remaining = Selected;
	Remaining.Count -= 1;
	const FMadItemStack Previous = Worn.GetSlot(WornSlot);

	Worn.SetSlot(WornSlot, Piece);
	Inventory.SetSlot(SelectedSlot, Remaining.Count > 0 ? Remaining : Previous);
	if (Remaining.Count > 0 && !Previous.IsEmpty())
	{
		AddStack(Previous);
	}
	ChangedDelegate.Broadcast();
	return true;
}

FString UMadInventoryComponent::Describe() const
{
	TStringBuilder<1024> Out;
	for (int32 Slot = 0; Slot < Worn.NumSlots(); ++Slot)
	{
		if (!Worn.GetSlot(Slot).IsEmpty())
		{
			Out.Appendf(TEXT("  [%s] %s\n"), *MadFall::Wear::GetSlotName(Slot).ToString(), *Worn.GetSlot(Slot).Item.ToString());
		}
	}
	for (int32 Slot = 0; Slot < Inventory.NumSlots(); ++Slot)
	{
		const FMadItemStack& Stack = Inventory.GetSlot(Slot);
		if (Stack.IsEmpty())
		{
			continue;
		}
		Out.Appendf(TEXT("  [%2d]%s %s x%d"), Slot, Slot == SelectedSlot ? TEXT("*") : TEXT(" "), *Stack.Item.ToString(), Stack.Count);
		if (Stack.Durability >= 0)
		{
			Out.Appendf(TEXT(" (durability %d)"), Stack.Durability);
		}
		Out.Append(TEXT("\n"));
	}
	return Out.Len() > 0 ? FString(Out.ToString()) : FString(TEXT("  (empty)\n"));
}

// ===========================================================================
// Survival
// ===========================================================================

UMadSurvivalComponent::UMadSurvivalComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.0f;
}

UAbilitySystemComponent* UMadSurvivalComponent::FindAbilitySystem() const
{
	const IAbilitySystemInterface* Interface = Cast<IAbilitySystemInterface>(GetOwner());
	return Interface ? Interface->GetAbilitySystemComponent() : nullptr;
}

FMadSurvivalStats UMadSurvivalComponent::GetStats() const
{
	if (const UAbilitySystemComponent* AbilitySystem = FindAbilitySystem())
	{
		if (const UMadSurvivalAttributeSet* Set = AbilitySystem->GetSet<UMadSurvivalAttributeSet>())
		{
			return Set->ToStats();
		}
	}
	return FMadSurvivalStats();
}

void UMadSurvivalComponent::SetStats(const FMadSurvivalStats& Stats)
{
	if (UAbilitySystemComponent* AbilitySystem = FindAbilitySystem())
	{
		UMadSurvivalAttributeSet::WriteStats(*AbilitySystem, Stats);
	}
}

void UMadSurvivalComponent::ApplyEffects(const TMap<FName, float>& Effects)
{
	FMadSurvivalStats Stats = GetStats();
	TArray<FName> Unknown;
	MadFall::Survival::ApplyEffects(Stats, Effects, &Unknown);
	SetStats(Stats);

	for (const FName& Name : Unknown)
	{
		UE_LOG(LogMadFallGameplay, Warning, TEXT("Consumable effect '%s' is not a survival stat and was ignored."), *Name.ToString());
	}
}

void UMadSurvivalComponent::ApplyDamage(float Amount)
{
	FMadSurvivalStats Stats = GetStats();
	Stats.Health = FMath::Clamp(Stats.Health - Amount, 0.0f, Stats.MaxHealth);
	SetStats(Stats);
}

float UMadSurvivalComponent::ApplyAttackDamage(float Amount)
{
	const float Taken = FMath::Max(0.0f, Amount) * (1.0f - Armor);
	ApplyDamage(Taken);
	return Taken;
}

bool UMadSurvivalComponent::TrySpendStamina(float Amount)
{
	FMadSurvivalStats Stats = GetStats();
	if (Stats.Stamina < Amount)
	{
		return false;
	}
	Stats.Stamina -= Amount;
	SetStats(Stats);
	return true;
}

void UMadSurvivalComponent::UpdateAmbient()
{
	const UWorld* World = GetWorld();
	const UMadVoxelWorldSubsystem* VoxelWorld = World ? World->GetSubsystem<UMadVoxelWorldSubsystem>() : nullptr;
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	if (Generator == nullptr)
	{
		return;
	}

	// The generator's climate field is 0..1 and already falls with altitude;
	// map it onto a plausible air temperature range and add the time of day.
	const FVector Location = GetOwner()->GetActorLocation() / MadFall::VoxelSizeUU;
	const float Climate = Generator->GetTemperature(Location.X, Location.Y, Location.Z);
	float Celsius = FMath::Lerp(-20.0f, 42.0f, Climate);

	if (const UMadWorldClockSubsystem* Clock = World->GetSubsystem<UMadWorldClockSubsystem>())
	{
		Celsius += Clock->GetTemperatureOffset();
	}
	if (const UMadWeatherSubsystem* Weather = World->GetSubsystem<UMadWeatherSubsystem>())
	{
		Celsius += Weather->GetTemperatureOffset();
	}

	// Height bites on top of the biome's own climate. The generator cools its
	// temperature field with altitude too, but that one also picks biomes, and
	// it is deliberately capped (a stronger lapse there took a quarter of the
	// world for tundra - measured, see "Climate"). This one is only what the
	// survivor feels, so it can keep going where the other stops: about 8 C
	// colder a hundred voxels up, which is what makes a coat the price of a
	// summit now that summits are 116 voxels high.
	const float AltitudeAboveSea = FMath::Max(0.0f, static_cast<float>(Location.Z) - static_cast<float>(VoxelWorld->GetWorldGenSettings().SeaLevel));
	Celsius -= FMath::Min(AltitudeAboveSea, 160.0f) * 0.08f;

	Environment.AmbientTemperature = Celsius;
}

void UMadSurvivalComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	MAD_FRAME_SCOPE(Player);

	UAbilitySystemComponent* AbilitySystem = FindAbilitySystem();
	if (AbilitySystem == nullptr)
	{
		return;
	}

	UpdateAmbient();

	ExertionRemaining = FMath::Max(0.0f, ExertionRemaining - DeltaTime);
	Environment.bExerting = ExertionRemaining > 0.0f;

	FMadSurvivalStats Stats = GetStats();
	LastCauses = MadFall::Survival::Step(Stats, Environment, Tuning, DeltaTime);
	SetStats(Stats);

	// Checked against the previous tick rather than this step's input, so a
	// death from ApplyDamage between ticks is reported too.
	const bool bDeadNow = Stats.IsDead();
	if (bDeadNow && !bWasDead)
	{
		DiedDelegate.Broadcast(LastCauses);
	}
	bWasDead = bDeadNow;
}

void UMadSurvivalComponent::BeginPlay()
{
	Super::BeginPlay();

	// Balance comes from data (madfall:survival tuning plus any mod patches),
	// read once per survivor.
	Tuning = MadFall::Survival::GetConfiguredTuning();
}

void UMadSurvivalComponent::SetMaxVitals(float MaxHealth, float MaxStamina)
{
	UAbilitySystemComponent* AbilitySystem = FindAbilitySystem();
	if (AbilitySystem == nullptr)
	{
		return;
	}
	AbilitySystem->SetNumericAttributeBase(UMadSurvivalAttributeSet::GetMaxHealthAttribute(), FMath::Max(1.0f, MaxHealth));
	AbilitySystem->SetNumericAttributeBase(UMadSurvivalAttributeSet::GetMaxStaminaAttribute(), FMath::Max(1.0f, MaxStamina));

	// Re-clamp current values against the new maxima.
	FMadSurvivalStats Stats = GetStats();
	Stats.Health = FMath::Min(Stats.Health, Stats.MaxHealth);
	Stats.Stamina = FMath::Min(Stats.Stamina, Stats.MaxStamina);
	SetStats(Stats);
}

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "AbilitySystemInterface.h"
#include "CoreMinimal.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "MadHarvest.h"
#include "MadInventory.h"
#include "MadProgression.h"
#include "MadSurvivalModel.h"
#include "MadVoxelRaycast.h"
#include "Math/RandomStream.h"
#include "MadDamageable.h"
#include "MadQuests.h"
#include "MadPlayerCharacter.generated.h"

class UAbilitySystemComponent;
class UCameraComponent;
class UMadViewModelComponent;
class UInputAction;
class UInputMappingContext;
class UMadInventoryComponent;
class UMadSurvivalAttributeSet;
class UMadSurvivalComponent;
class AMadTrader;
struct FMadTraderDefinition;
struct FMadTraderState;
enum class EMadTradeResult : uint8;

/** The inventory screen's right-hand column. */
enum class EMadInventoryTab : uint8
{
	Crafting,
	Skills
};

/** Which grid of the inventory screen a slot index refers to. */
enum class EMadInventorySide : uint8
{
	Backpack,
	Container,
	/** Clothing slots: head, body, legs, feet. */
	Worn
};

namespace MadFall
{
	/**
	 * The survivor the first local player controls, or null. One definition: six
	 * files kept identical copies in anonymous namespaces, which collided the
	 * moment a unity build put two of them in one translation unit.
	 */
	MADFALLGAMEPLAY_API class AMadPlayerCharacter* FindLocalPlayer(const UWorld* World);
}

/**
 * The survivor.
 *
 * Everything a player does with the world funnels through four verbs -
 * UsePrimary (swing), UseSecondary (place or consume), Interact (loot, open a
 * station) and CraftRecipe - which are public so console commands and tests
 * drive exactly the code a mouse click does. Input handlers only translate
 * buttons into those calls.
 *
 * INPUT ASSETS ARE BUILT IN C++
 *   The mapping context and actions are created at runtime rather than loaded
 *   from .uasset files, so bindings are reviewable in a diff and the project
 *   has no binary input assets to keep in sync with code. Rebinding UI (Phase 7)
 *   works on the runtime objects the same way.
 */
UCLASS()
class MADFALLGAMEPLAY_API AMadPlayerCharacter : public ACharacter, public IAbilitySystemInterface, public IMadDamageable
{
	GENERATED_BODY()

public:
	AMadPlayerCharacter();

	//~ Begin AActor / APawn
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void NotifyControllerChanged() override;
	//~ End AActor / APawn

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystem; }

	// --- verbs ------------------------------------------------------------------

	/** One swing at the targeted voxel with the held item. Respects cooldown unless bIgnoreCooldown. */
	bool UsePrimary(bool bIgnoreCooldown = false);

	/** Places the held block against the targeted face, or consumes the held consumable. */
	bool UseSecondary();

	/** Opens the targeted crate, toggles a door, or makes a bed the respawn point. */
	bool Interact();

	/** The bed the survivor respawns at, if one was slept in and still exists. */
	TOptional<FIntVector> GetBedVoxel() const { return BedVoxel; }

	//~ Begin IMadDamageable - a ranged attack hitting the survivor
	virtual bool ReceiveHit(float Amount, FName DamageType, AActor* Attacker) override;
	virtual bool IsDead() const override { return IsDown(); }
	//~ End IMadDamageable

	/** Counts something the survivor did toward active quests, and pays out any it completes. */
	void NotifyQuest(EMadQuestObjectiveType Type, FName Id, const TArray<FName>& ThingTags, int32 Amount = 1, const TOptional<FIntVector>& Where = TOptional<FIntVector>());

	const FMadQuestLog& GetQuestLog() const { return Quests; }

	/** Debug: completes a quest as if its objectives were met. */
	bool CompleteQuest(FName Quest);

	/** True while holding onto a ladder. */
	bool IsClimbing() const { return bClimbing; }

	/** Takes the ingredients now and queues the job; the output arrives after the recipe's craft time. */
	EMadCraftResult CraftRecipe(FName RecipeId, int32 Times = 1);

	/** Cancels every queued craft and refunds the ingredients. */
	void CancelCrafting();

	/** Repairs the held tool with materials from the backpack. */
	EMadItemActionResult RepairSelected();

	/** Installs a mod from the backpack into the held tool. */
	EMadItemActionResult InstallModOnSelected(FName ModItem);

	/** Throws the held stack on the ground in front. */
	bool DropSelected();

	/** Adds items to the backpack; whatever does not fit is dropped as a pickup at DropLocation. */
	void GiveOrDrop(const TArray<FMadItemStack>& Stacks, const FVector& DropLocation);

	struct FCraftJob
	{
		FName Recipe;
		int32 Times = 1;
		float Total = 0.0f;
		float Remaining = 0.0f;
	};
	const TArray<FCraftJob>& GetCraftQueue() const { return CraftQueue; }

	// --- inventory screen ------------------------------------------------------------

	/** Opens the backpack screen; with a container voxel, that crate's slots beside it. Frees the mouse. */
	void OpenInventory(TOptional<FIntVector> ContainerVoxel = TOptional<FIntVector>());
	void CloseInventory();

	// --- trading ------------------------------------------------------------------

	/** Opens the inventory screen with a trader's shelves where a crate's slots would be. */
	void OpenTrade(const FIntVector& TraderMarker, FName TraderId);
	bool IsTrading() const { return OpenTraderVoxel.IsSet(); }

	/** The open trader's shelves and definition; false when not trading. */
	bool GetOpenTrade(FMadTraderState*& OutState, const FMadTraderDefinition*& OutTrader) const;

	/** Buys from a shelf slot (Count 0: the whole stack). */
	EMadTradeResult BuyFromTrader(int32 StockSlot, int32 Count);

	/** Sells from a backpack slot (Count 0: the whole stack). */
	EMadTradeResult SellToTrader(int32 BackpackSlot, int32 Count);

	/** The trader under the crosshair, within reach. */
	AMadTrader* GetAimedTrader() const { return AimedTrader.Get(); }
	bool IsInventoryOpen() const { return bInventoryOpen; }

	/** Rows the right-hand column is scrolled past (mouse wheel while the inventory is open). */
	int32 GetColumnScroll() const { return ColumnScroll; }
	bool HasOpenContainer() const { return OpenContainerVoxel.IsSet(); }

	/** The open crate's slots, or null when no crate is open (or it was destroyed). */
	FMadInventory* GetOpenContainerInventory() const;

	/** Drag and drop between the backpack and the open crate (see MadFall::InventoryOps::MoveSlot). */
	EMadSlotMove MoveItem(EMadInventorySide FromSide, int32 FromSlot, EMadInventorySide ToSide, int32 ToSlot, int32 Count = 0);

	/** Shift-click: backpack to crate when one is open, otherwise hotbar and backpack trade; crate to backpack. */
	int32 QuickMoveItem(EMadInventorySide Side, int32 Slot);

	/** Everything that fits from the open crate into the backpack. */
	int32 TakeAllFromContainer();

	/**
	 * A click on a slot in the screen. The first click picks a stack up (half
	 * of it with bHalf), the second puts it down; bQuick quick-moves instead.
	 * The picked-up stack never leaves its slot until it is put down, so closing
	 * the screen mid-drag cannot lose anything.
	 */
	void ClickInventorySlot(EMadInventorySide Side, int32 Slot, bool bQuick, bool bHalf);

	struct FInventoryCursor
	{
		bool bActive = false;
		EMadInventorySide Side = EMadInventorySide::Backpack;
		int32 Slot = INDEX_NONE;
		/** How many are held: 0 is the whole stack, which also swaps onto a different item. */
		int32 Count = 0;
	};
	const FInventoryCursor& GetInventoryCursor() const { return InventoryCursor; }

	/** How many items the cursor holds now (0 when nothing): the whole stack, or the part taken. */
	int32 GetHeldCount() const;

	/**
	 * Takes more (Delta > 0) or fewer of the held stack, from 1 to all of it -
	 * the mouse wheel while holding. Holding all of it again is the whole stack.
	 */
	void AdjustHeldCount(int32 Delta);

	/** Sorts the backpack (not the hotbar) or the open crate: MadFall::InventoryOps::SortRange. */
	bool SortInventory(EMadInventorySide Side);

	/** Ctrl-click: takes the last-installed mod off the tool in a backpack slot, into the backpack. */
	EMadItemActionResult RemoveModFromSlot(int32 Slot);

	/** Points the view at a voxel's centre and refreshes the target. For tests and tooling. */
	void AimAtVoxel(const FIntVector& Voxel);

	/** Turns the view toward a world point (centimetres). */
	void AimAtLocation(const FVector& Location);

	/** Teleports the capsule so its feet stand in a voxel. */
	void TeleportToVoxel(const FIntVector& Voxel);

	/**
	 * Teleports somewhere that may not be streamed in yet: holds the survivor
	 * still (and pauses console scripts) until the chunks around the destination
	 * are loaded and meshed, as on spawn. The respawn point is unchanged.
	 */
	void TravelToVoxel(const FIntVector& Voxel);

	/** Holds movement input along a world direction for Seconds (scripted runs). */
	void WalkFor(float Seconds, const FVector& Direction) { ScriptedWalkSeconds = Seconds; ScriptedWalkDirection = Direction.GetSafeNormal2D(); }

	/** The voxel the survivor's feet are in. */
	FIntVector GetFeetVoxel() const;

	// --- state the HUD reads -----------------------------------------------------

	bool HasTarget() const { return bHasTarget; }
	const FMadVoxelHit& GetTarget() const { return Target; }
	bool IsWaitingForWorld() const { return bWaitingForWorld; }
	// --- crafting column of the inventory screen ----------------------------------

	EMadInventoryTab GetInventoryTab() const { return InventoryTab; }
	void SetInventoryTab(EMadInventoryTab Tab) { InventoryTab = Tab; ColumnScroll = 0; }

	EMadRecipeCategory GetCraftCategory() const { return CraftCategory; }
	void SetCraftCategory(EMadRecipeCategory Category) { CraftCategory = Category; ColumnScroll = 0; }

	bool IsCraftableOnly() const { return bCraftableOnly; }
	void SetCraftableOnly(bool bOnly) { bCraftableOnly = bOnly; ColumnScroll = 0; }

	/** The recipe the detail pane shows; the first row when none is chosen or it left the list. */
	FName GetSelectedRecipe() const;
	void SelectRecipe(FName Recipe) { SelectedRecipe = Recipe; }

	/** The crafting list as the screen shows it now (MadFall::Crafting::ListRecipes). */
	void GetRecipeRows(TArray<FMadRecipeRow>& Out) const;
	bool IsStationNearby(FName Station) const;

	int32 GetLevel() const { return Level; }
	int32 GetExperience() const { return Experience; }
	int32 GetExperienceForNextLevel() const { return Level * 100; }
	void AddExperience(int32 Amount);

	/** Days survived plus level: what loot and hordes scale with. */
	int32 GetGameStage() const;

	/** Buys the next rank of a perk with a level-up point. */
	EMadPerkResult BuyPerk(FName PerkId);

	/** Product of owned perk multipliers for a stat (see MadFall::Perks). */
	float GetPerkMultiplier(FName Stat) const;

	const TMap<FName, int32>& GetPerkRanks() const { return PerkRanks; }
	int32 GetUnspentPerkPoints() const;

	struct FMessage
	{
		FString Text;
		double ExpiresAt = 0.0;
	};
	const TArray<FMessage>& GetMessages() const { return Messages; }
	void PushMessage(const FString& Text, float Seconds = 3.0f);

	UMadInventoryComponent* GetInventory() const { return Inventory; }
	UMadSurvivalComponent* GetSurvival() const { return Survival; }

	/** Marks a loud action (a swing, a placed block, sprinting). Zombies within hearing range react. */
	void ReportNoise() { LastNoiseTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0; }

	/** True if the survivor made noise within the last WindowSeconds. */
	bool IsNoisy(double WindowSeconds) const { return GetWorld() && GetWorld()->GetTimeSeconds() - LastNoiseTime <= WindowSeconds; }

	/** Dead and waiting to respawn: zombies lose interest. */
	bool IsDown() const { return RespawnAt >= 0.0f || bWaitingForWorld; }

	/** Reach in voxels for the held item. */
	float GetReach() const;

	FString DescribeStatus() const;

	/** Save support. Restore replaces the starting kit; the position is applied once the world is ready. */
	void SaveState(struct FMadPlayerSaveData& Out) const;
	void RestoreState(const struct FMadPlayerSaveData& In);

protected:
	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UCameraComponent> Camera;

	/** The held item, in first person. */
	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UMadViewModelComponent> ViewModel;

	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UAbilitySystemComponent> AbilitySystem;

	UPROPERTY()
	TObjectPtr<UMadSurvivalAttributeSet> SurvivalAttributes;

	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UMadSurvivalComponent> Survival;

	UPROPERTY(VisibleAnywhere, Category = "MadFall")
	TObjectPtr<UMadInventoryComponent> Inventory;

private:
	void EnsureInput();

	/** Maps every key onto the actions, from MadFall::Input::GetActive(). Runs again when bindings change. */
	void MapKeys();
	FDelegateHandle BindingsChangedHandle;
	void OnMove(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnSprint(const FInputActionValue& Value);
	void OnPrimaryStarted(const FInputActionValue& Value);
	void OnPrimaryCompleted(const FInputActionValue& Value);
	void OnSecondary(const FInputActionValue& Value);
	void OnInteract(const FInputActionValue& Value);
	void OnScroll(const FInputActionValue& Value);
	void OnToggleCraft(const FInputActionValue& Value);
	void OnHotbar(const FInputActionValue& Value, int32 Slot);

	void OnRepair(const FInputActionValue& Value);
	void OnDrop(const FInputActionValue& Value);
	void OnToggleInventory(const FInputActionValue& Value);
	void OnPause(const FInputActionValue& Value);
	void OnToggleMap(const FInputActionValue& Value);

	/** Closes the crate if it is gone or out of reach. */
	void TickInventoryScreen();
	FMadInventory* GetSideInventory(EMadInventorySide Side) const;

	void TickCrafting(float DeltaSeconds);
	void GetCraftRefund(TArray<FMadItemStack>& OutRefund) const;
	static float CraftTimeScale();

	void UpdateTarget();
	void TickSpawn(float DeltaSeconds);
	void HandleDied(const struct FMadSurvivalStepResult& Causes);
	void Respawn();

	/** The top standable voxel in a column, from voxel data. INDEX_NONE if the column is not loaded. */
	int32 FindStandableZ(int32 X, int32 Y) const;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> Actions;

	UInputAction* MoveAction = nullptr;
	UInputAction* LookAction = nullptr;
	UInputAction* JumpAction = nullptr;
	UInputAction* SprintAction = nullptr;
	UInputAction* PrimaryAction = nullptr;
	UInputAction* SecondaryAction = nullptr;
	UInputAction* InteractAction = nullptr;
	UInputAction* ScrollAction = nullptr;
	UInputAction* CraftAction = nullptr;
	UInputAction* RepairAction = nullptr;
	UInputAction* DropAction = nullptr;
	UInputAction* InventoryAction = nullptr;
	UInputAction* PauseAction = nullptr;
	UInputAction* MapAction = nullptr;

	/** mad.player.walk: movement input held for a while, for scripted runs. */
	float ScriptedWalkSeconds = 0.0f;
	FVector ScriptedWalkDirection = FVector::ForwardVector;

	float StepDistance = 0.0f;
	float LastHealth = 0.0f;
	float HurtCooldown = 0.0f;
	void TickSounds(float DeltaSeconds);

	/** Forward/back input this frame, for climbing. Consumed by TickClimbing. */
	float ClimbInput = 0.0f;
	bool bClimbing = false;
	void TickClimbing();

	TOptional<FIntVector> BedVoxel;

	FMadQuestLog Quests;
	float QuestCheckTimer = 0.0f;
	void TickQuests(float DeltaSeconds);
	void HandleQuestsCompleted(const TArray<FName>& Done);

	/** Swaps a block (and the matching blocks stacked on it: both halves of a door) for its toggle_to. */
	bool ToggleBlock(const FIntVector& Voxel, const struct FMadBlockDefinitionData& Block);

	/** Right-click water with an item that has "fill". */
	bool TryFill(const struct FMadItemDefinition& Item);

	/** Right-click a `block.tillable` block with an item that has "till": a hoe makes farmland. */
	bool TryTill(const struct FMadItemDefinition& Item);

	/** Primary use of a weapon with tool.ranged: looses one round of its ammo. */
	bool FireRanged(const struct FMadItemDefinition& Weapon, double Now);

	bool bInventoryOpen = false;
	TOptional<FIntVector> OpenTraderVoxel;
	FName OpenTraderId;
	TWeakObjectPtr<AMadTrader> AimedTrader;
	int32 ColumnScroll = 0;
	EMadInventoryTab InventoryTab = EMadInventoryTab::Crafting;
	EMadRecipeCategory CraftCategory = EMadRecipeCategory::All;
	bool bCraftableOnly = false;
	FName SelectedRecipe;
	TOptional<FIntVector> OpenContainerVoxel;
	FInventoryCursor InventoryCursor;

	TArray<FCraftJob> CraftQueue;
	TArray<UInputAction*> HotbarActions;

	FRandomStream Random;

	FMadVoxelHit Target;
	bool bHasTarget = false;
	FIntVector LastPenaltyTarget = FIntVector(MAX_int32);

	double NextUseTime = 0.0;
	bool bPrimaryHeld = false;

	bool bWaitingForWorld = true;
	float SpawnWait = 0.0f;

	/**
	 * Holds a falling survivor in the air while the ground under them has no
	 * collision yet (its chunk unloaded, or its mesh still building). Returns
	 * true while held.
	 */
	bool TickTerrainHold(float DeltaSeconds);
	bool bTerrainHold = false;
	float TerrainHoldSettle = 0.0f;
	float SettleTime = 0.0f;
	FIntPoint SpawnColumn = FIntPoint::ZeroValue;

	/** Set when restoring a save: spawn here instead of the top of SpawnColumn. */
	TOptional<FVector> SavedLocation;
	FRotator SavedView = FRotator::ZeroRotator;


	int32 Level = 1;
	int32 Experience = 0;
	TMap<FName, int32> PerkRanks;

	/** Pushes perk effects that live outside the verbs (max vitals, drain) into the survival component. */
	void ApplyPerkStats();

	float RespawnAt = -1.0f;
	double LastNoiseTime = -1000.0;

	TArray<FMessage> Messages;
};

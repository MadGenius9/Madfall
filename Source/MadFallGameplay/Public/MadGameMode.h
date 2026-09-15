// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "MadInventory.h"
#include "MadGameMode.generated.h"

/** Single-player survival rules: the survivor pawn and the survival HUD. */
UCLASS()
class MADFALLGAMEPLAY_API AMadGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMadGameMode();

	/** No survivor at the title screen: the menu owns the camera there. */
	virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;
};

/**
 * The survival HUD, drawn on the canvas.
 *
 * Deliberately canvas rather than UMG for now: it has no assets, so it is
 * reviewable in a diff and cannot drift from the C++ state it shows. Phase 7
 * replaces it with a styled UMG layer reading the same accessors.
 */
UCLASS()
class MADFALLGAMEPLAY_API AMadHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
	virtual void NotifyHitBoxClick(FName BoxName) override;

	/**
	 * What a click on a hit box does: a slot, "Take all", "Sort", or a perk's Take
	 * button. NotifyHitBoxClick reads shift, ctrl and the right button from the
	 * controller; `mad.hud.click` passes them, so CI drives exactly the path a
	 * mouse does. Ctrl on a modded tool takes its last mod off.
	 */
	void ClickBox(FName BoxName, bool bQuick, bool bHalf, bool bCtrl = false);

	/** Width of the crafting / skills column beside the inventory. */
	static constexpr float SkillsPanelWidth = 380.0f;

private:
	void DrawBar(float X, float Y, float Width, float Fraction, const FLinearColor& Colour, const FString& Label);
	void DrawInventoryScreen(const class AMadPlayerCharacter& Player);
	void DrawSlot(const struct FMadItemStack& Stack, float X, float Y, float Size, bool bHighlighted, FName HitBox);
	/** An item's icon (MadFall::IconCache) as a Size x Size tile. False, drawing nothing, without one. */
	bool DrawItemIcon(FName ItemId, float X, float Y, float Size, float Opacity = 1.0f);
	/** A stack count right-aligned at RightX, shadowed. */
	void DrawCount(int32 Count, float RightX, float Y);
	void DrawSkillsPanel(const class AMadPlayerCharacter& Player, float X, float Y, float Height);
	void DrawCraftingPanel(const class AMadPlayerCharacter& Player, float X, float Y, float Height);
	void DrawTooltip(const FMadItemStack& Stack);
	/** The held stack's icon and count beside the mouse, over everything else on the screen. */
	void DrawHeldStack(const class AMadPlayerCharacter& Player);

	/** The mouse position this frame, and the stack under it (set while drawing slots). */
	FVector2D Mouse = FVector2D(-1.0, -1.0);
	TOptional<FMadItemStack> HoveredStack;
	FName HoveredBox;
};

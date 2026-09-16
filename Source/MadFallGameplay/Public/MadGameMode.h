// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadHudLayout.h"
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
 * Deliberately canvas rather than UMG: it has no assets, so it is reviewable in
 * a diff and cannot drift from the C++ state it shows.
 *
 * Where everything goes is FMadHudLayout's job, not this class's: sizes are
 * authored at 1600x900 and scaled to the window (mad.ui.Scale), so the panels
 * neither overlap in a small window nor shrink to nothing on a 4K screen, and
 * a test sweeps the placement across window sizes a screenshot never would.
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

	/** Width of the crafting / skills column beside the inventory, in design pixels. */
	static constexpr float SkillsPanelWidth = 380.0f;

	/** Where the panels are this frame; rebuilt from the viewport every DrawHUD. */
	const struct FMadHudLayout& GetLayout() const { return Layout; }

private:
	void DrawBar(float X, float Y, float Width, float Height, float Fraction, const FLinearColor& Colour, const FString& Label);
	void DrawInventoryScreen(const class AMadPlayerCharacter& Player);
	void DrawSlot(const struct FMadItemStack& Stack, float X, float Y, float Size, bool bHighlighted, FName HitBox);
	/** An item's icon (MadFall::IconCache) as a Size x Size tile. False, drawing nothing, without one. */
	bool DrawItemIcon(FName ItemId, float X, float Y, float Size, float Opacity = 1.0f);
	/** A stack count right-aligned at RightX, shadowed, at the scale of what it labels. */
	void DrawCount(int32 Count, float RightX, float Y, float Scale = 1.0f);
	void DrawSkillsPanel(const class AMadPlayerCharacter& Player, float X, float Y, float Height);
	void DrawCraftingPanel(const class AMadPlayerCharacter& Player, float X, float Y, float Height);
	void DrawTooltip(const FMadItemStack& Stack);
	/** The held stack's icon and count beside the mouse, over everything else on the screen. */
	void DrawHeldStack(const class AMadPlayerCharacter& Player);

	/** Design pixels to screen pixels, for the window this frame. */
	float Px(float DesignPixels) const;
	/** Text at the HUD's scale: the engine's fonts are fixed size, so they are scaled. */
	void DrawScaled(const FString& Text, const FLinearColor& Colour, float X, float Y, class UFont* Font);
	float ScaledTextWidth(const FString& Text, class UFont* Font) const;

	/** Where every panel goes, rebuilt each frame from the viewport size. */
	FMadHudLayout Layout;
	/** The inventory screen's own layout, rebuilt when that screen is drawn. */
	FMadInventoryLayout Bag;

	/** The mouse position this frame, and the stack under it (set while drawing slots). */
	FVector2D Mouse = FVector2D(-1.0, -1.0);
	TOptional<FMadItemStack> HoveredStack;
	FName HoveredBox;
};

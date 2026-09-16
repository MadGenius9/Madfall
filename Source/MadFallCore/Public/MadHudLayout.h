// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Where the HUD's panels go, for a viewport of any size.
 *
 * WHY this is a module of its own, and not pixel arithmetic inside DrawHUD:
 * the HUD was laid out in fixed pixels, so it only looked right near 1600x900.
 * In a 900x500 window the vitals ran under the hotbar and the crafting column
 * covered the backpack; on a 4K screen everything was a postage stamp. Layout
 * that can be wrong at some sizes and right at others belongs in a pure
 * function a test can sweep across every size, rather than in a draw call that
 * only a screenshot can check.
 *
 * Everything is authored in *design pixels* at 1600x900 and multiplied by
 * FMadHudLayout::Scale, so one number changes the whole HUD's size and a
 * player can set it (mad.ui.Scale).
 */
struct MADFALLCORE_API FMadHudLayout
{
	/** The size the numbers below are authored against. */
	static constexpr float DesignWidth = 1600.0f;
	static constexpr float DesignHeight = 900.0f;

	/** Viewport size this layout was built for. */
	float Width = DesignWidth;
	float Height = DesignHeight;

	/** Design pixels to screen pixels. */
	float Scale = 1.0f;

	/** How many bars the vitals box was sized for (breath appears only under water). */
	int32 VitalBars = 4;

	/** Always-on elements. */
	FBox2D Clock = FBox2D(ForceInit);
	FBox2D Compass = FBox2D(ForceInit);
	FBox2D Journal = FBox2D(ForceInit);
	FBox2D Vitals = FBox2D(ForceInit);
	FBox2D Hotbar = FBox2D(ForceInit);
	/** Pickups and warnings, above the hotbar. */
	FBox2D Messages = FBox2D(ForceInit);
	/** The crafting queue, bottom right. */
	FBox2D Queue = FBox2D(ForceInit);
	/** The world map, when it is open (the journal is hidden then). */
	FBox2D Map = FBox2D(ForceInit);

	/**
	 * How many quest lines the journal has room for, at most what was asked
	 * for: on a short screen it gives way to the crafting queue below it, and
	 * the HUD draws the quests that fit.
	 */
	int32 JournalLines = 0;

	/** Design pixels to screen pixels. */
	float Px(float DesignPixels) const { return DesignPixels * Scale; }

	/** The widest text that fits a box, in characters of the engine's small font. */
	float FontScale() const { return Scale; }
};

/** The inventory screen: the bag panel, and the crafting/skills column beside it. */
struct MADFALLCORE_API FMadInventoryLayout
{
	float Scale = 1.0f;
	float SlotSize = 56.0f;
	float Gap = 4.0f;
	int32 Columns = 9;

	/** The bag panel. Its height is what the caller asked for, capped to the screen. */
	FBox2D Panel = FBox2D(ForceInit);
	/** The crafting and skills column. */
	FBox2D Side = FBox2D(ForceInit);
	/** False when the screen is too narrow for both and the column overlays the panel's edge. */
	bool bSideBySide = true;
};

namespace MadFall::Hud
{
	/** The smallest and largest the HUD may scale, before the player's multiplier. */
	inline constexpr float MinScale = 0.5f;
	inline constexpr float MaxScale = 2.0f;

	/**
	 * The scale for a viewport: the smaller of its width and height ratios
	 * against the design size, so nothing is ever cut off by a narrow window,
	 * clamped, then multiplied by the player's setting.
	 */
	MADFALLCORE_API float GetScale(float ViewportWidth, float ViewportHeight, float UserScale = 1.0f);

	/**
	 * The HUD's boxes for a viewport. HotbarSlots sizes the hotbar; JournalLines
	 * is how many text rows the quest journal needs (it grows with the quests).
	 */
	MADFALLCORE_API FMadHudLayout Build(float ViewportWidth, float ViewportHeight, float UserScale = 1.0f,
		int32 HotbarSlots = 9, int32 JournalLines = 6, int32 VitalBars = 4);

	/**
	 * One attempt at a layout, at exactly this multiplier. Build() calls it,
	 * stepping the multiplier down until Validate passes, so this is what a
	 * test uses to see an invalid layout on purpose.
	 */
	MADFALLCORE_API FMadHudLayout BuildAt(float ViewportWidth, float ViewportHeight, float UserScale,
		int32 HotbarSlots = 9, int32 JournalLines = 6, int32 VitalBars = 4);

	/**
	 * True when every box is on screen and no two boxes that are drawn at the
	 * same time overlap. Fills OutProblem with the first failure, so a test and
	 * `mad.hud.layout` report the same thing.
	 */
	MADFALLCORE_API bool Validate(const FMadHudLayout& Layout, FString& OutProblem);

	/**
	 * The inventory screen's boxes. Rows is the bag's content height in slot
	 * rows and ExtraLines the header and footer rows, both of which the caller
	 * knows (a container or a trader adds rows).
	 */
	MADFALLCORE_API FMadInventoryLayout BuildInventory(float ViewportWidth, float ViewportHeight, float UserScale,
		int32 Rows, float ExtraDesignHeight, float SideDesignWidth);
}

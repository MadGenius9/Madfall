// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FMadItemDefinition;
class UTexture2D;

/**
 * Item icons, painted in code.
 *
 * WHY: there are no icon assets, and a slot showing "Wood Pla" or "Canned F"
 * read as a debug view. Every item already says what it is - its kind, tags,
 * the block it places and that block's surface - so a small pixel-art icon can
 * be drawn from the data alone: a block is an isometric cube in its surface's
 * colour and pattern, a pickaxe a pickaxe, food a can or a steak, and a mod's
 * new item gets a sensible icon without shipping one. An item's own `icon`
 * texture, if it names one, still wins.
 *
 * Painting is pure (a 32x32 sRGB pixel buffer, transparent background, dark
 * outline) so it is tested without a renderer; UMadItemIconCache turns the
 * buffers into textures once per item.
 */
namespace MadFall::Icons
{
	inline constexpr int32 Size = 32;

	enum class EShape : uint8
	{
		Block,
		Pickaxe, Axe, Shovel, Hoe, Club, Spear, Bow, Arrow,
		Can, Meat, Berries, Potato, Corn, Food,
		Bottle, Medical, Seeds,
		Hat, Shirt, Trousers, Boots,
		Coin, Gear, Plank, Rock, Scrap, Cloth, Lump
	};

	/** Which picture an item gets, from its kind, tags and id. */
	MADFALLGAMEPLAY_API EShape ChooseShape(const FMadItemDefinition& Item);

	/**
	 * Paints an item's icon into Size*Size sRGB pixels, row by row from the top,
	 * alpha 0 where there is nothing. Deterministic.
	 */
	MADFALLGAMEPLAY_API void Paint(const FMadItemDefinition& Item, TArray<FColor>& OutPixels);
}

/** Textures for item icons, painted on first use and kept for the session. */
namespace MadFall::IconCache
{
	/**
	 * The icon texture for an item id: its own `icon` asset if it has one,
	 * otherwise a painted one. Null for an unknown item or without a renderer.
	 */
	MADFALLGAMEPLAY_API UTexture2D* Get(FName ItemId);
}

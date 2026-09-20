// Copyright MadFall. All Rights Reserved.

#include "MadBuilding.h"

namespace
{
	/** The block an item places, or None. */
	FName PlacedBlockOf(const FMadGameplayDefinitions& Definitions, FName ItemId)
	{
		const FMadItemDefinition* Item = Definitions.FindItem(ItemId);
		return Item ? Item->PlacesBlock : NAME_None;
	}
}

FName MadFall::Building::FindBlockItem(const FMadGameplayDefinitions& Definitions, FName BlockId)
{
	if (BlockId.IsNone())
	{
		return NAME_None;
	}

	// Block items are generated from the registry at load, and the generated one
	// is named after its block - but a hand-written item may place it instead,
	// so the search is over what items say rather than over the name.
	for (const FMadItemDefinition& Item : Definitions.GetItems())
	{
		if (Item.PlacesBlock == BlockId)
		{
			return Item.Id;
		}
	}
	return NAME_None;
}

const FMadRecipeDefinition* MadFall::Building::FindUpgradeRecipe(
	const FMadGameplayDefinitions& Definitions, FName FromBlock)
{
	if (FromBlock.IsNone())
	{
		return nullptr;
	}

	for (const FMadRecipeDefinition& Recipe : Definitions.GetRecipes())
	{
		// It has to make one of a block.
		if (Recipe.Output.Count != 1 || PlacedBlockOf(Definitions, Recipe.Output.Item).IsNone())
		{
			continue;
		}

		// ...out of exactly one other block, taken one at a time. Two blocks in
		// the ingredients is a recipe that combines things, not a tier step, and
		// wanting four of a block is a recipe that consumes a stack.
		int32 BlockIngredients = 0;
		bool bFromThisBlock = false;
		for (const FMadItemAmount& Ingredient : Recipe.Ingredients)
		{
			const FName Placed = PlacedBlockOf(Definitions, Ingredient.Item);
			if (Placed.IsNone())
			{
				continue;
			}
			++BlockIngredients;
			bFromThisBlock = bFromThisBlock || (Placed == FromBlock && Ingredient.Count == 1);
		}

		if (BlockIngredients == 1 && bFromThisBlock)
		{
			return &Recipe;
		}
	}
	return nullptr;
}

FMadBlockWorkPlan MadFall::Building::PlanBlockWork(
	const FMadGameplayDefinitions& Definitions, FName BlockId, uint8 Damage, int32 Level,
	TFunctionRef<int32(FName)> Held)
{
	FMadBlockWorkPlan Plan;
	if (BlockId.IsNone())
	{
		return Plan;
	}

	// Damage first. A survivor standing in front of a wall a horde has been
	// chewing wants it whole again, not better - and an upgrade that silently
	// threw away the damage would be a cheaper repair than repairing.
	if (Damage > 0)
	{
		const FName Patch = FindBlockItem(Definitions, BlockId);
		if (!Patch.IsNone())
		{
			Plan.Action = EMadBlockWork::Repair;
			Plan.Cost.Add({ Patch, 1 });
			Plan.bMissingMaterials = Held(Patch) < 1;
			return Plan;
		}
		// A block with no item to patch it with - terrain, a POI's own
		// scenery - simply cannot be repaired, and says so by planning nothing.
		return Plan;
	}

	const FMadRecipeDefinition* Recipe = FindUpgradeRecipe(Definitions, BlockId);
	if (Recipe == nullptr)
	{
		return Plan;
	}

	Plan.Action = EMadBlockWork::Upgrade;
	Plan.ToBlock = PlacedBlockOf(Definitions, Recipe->Output.Item);
	Plan.RequiredLevel = Recipe->RequiredLevel;
	Plan.bUnderLevelled = Level < Recipe->RequiredLevel;

	// Everything the recipe asks for except the block already standing there.
	bool bSkippedBase = false;
	for (const FMadItemAmount& Ingredient : Recipe->Ingredients)
	{
		if (!bSkippedBase && PlacedBlockOf(Definitions, Ingredient.Item) == BlockId && Ingredient.Count == 1)
		{
			bSkippedBase = true;
			continue;
		}
		Plan.Cost.Add(Ingredient);
		Plan.bMissingMaterials = Plan.bMissingMaterials || Held(Ingredient.Item) < Ingredient.Count;
	}
	return Plan;
}

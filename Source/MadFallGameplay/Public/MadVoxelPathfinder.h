// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallVoxelTypes.h"
#include "Templates/Function.h"

/** How a path step is taken. */
enum class EMadPathMove : uint8
{
	Walk,
	StepUp,
	Drop,
	/** Through a neighbour whose feet or head voxels are broken first. */
	Dig,
	/** Straight up or down a ladder, or up a wall for a walker that climbs walls. */
	Climb,
	/** Through the floor: the voxel underfoot is broken and the walker falls to the next ground. */
	DigDown
};

/** One step of a path: where the feet go, and what has to be broken first. */
struct MADFALLGAMEPLAY_API FMadPathStep
{
	/** The voxel the walker's feet occupy after this step. */
	FIntVector Feet = FIntVector::ZeroValue;

	EMadPathMove Move = EMadPathMove::Walk;

	/** Solid voxels in the way of this step, feet level first. Empty for a plain move. */
	TArray<FIntVector, TInlineAllocator<2>> BlocksToBreak;
};

struct MADFALLGAMEPLAY_API FMadVoxelPath
{
	/** Steps after the start cell, in order. */
	TArray<FMadPathStep> Steps;

	/** The path ends next to the goal. False: it ends at the explored cell closest to it. */
	bool bReachesGoal = false;

	float Cost = 0.0f;
	int32 NodesExpanded = 0;

	bool RequiresDigging() const
	{
		for (const FMadPathStep& Step : Steps)
		{
			if (Step.BlocksToBreak.Num() > 0) { return true; }
		}
		return false;
	}
};

struct MADFALLGAMEPLAY_API FMadPathSettings
{
	/** Expansion budget. A search that runs out returns the best partial path. */
	int32 MaxNodes = 3000;

	/** Voxels a walker will drop down in one step. */
	int32 MaxDrop = 3;

	float StepUpCost = 1.6f;

	/**
	 * What a step through water costs, on top of the step itself: waist deep
	 * once, over the head twice that.
	 *
	 * WHY: water was free. A horde crossed a lake in a straight line as if it
	 * were a field, which made a moat - the oldest defence there is - worth
	 * nothing. Deep water is passable, not forbidden: a zombie that can only
	 * reach the survivor by wading still comes, it just takes the dry way round
	 * when there is one.
	 */
	float WaterCostPerStep = 2.5f;
	float DropCostPerVoxel = 0.4f;

	/** True for a voxel of water (or any liquid). Nothing charges for water without it. */
	TFunction<bool(const FIntVector&)> IsLiquid;

	/**
	 * Cost units per second of digging. 1.0 means a block that takes 5 seconds
	 * to break costs the same as walking 5 voxels, so a zombie walks around a
	 * wall if the detour is shorter than digging through it - and digs through
	 * the weakest section of a wall rather than the nearest one.
	 */
	float DigCostPerSecond = 1.0f;

	/** Allow breaking blocks at all. Off for wandering, on for chasing and hordes. */
	bool bAllowDigging = true;

	/** Goal counts as reached within this many voxels (Chebyshev, horizontal) and 1 vertically. */
	int32 GoalRadius = 1;

	/**
	 * Ladders and other climbable blocks, which pathing otherwise sees as open
	 * air. Unset: nothing is climbable.
	 */
	TFunction<bool(const FIntVector&)> IsClimbable;

	/** Climb any wall: up through open air beside a solid block. For climbing variants. */
	bool bClimbWalls = false;

	float ClimbCostPerVoxel = 1.5f;

	/** Break through the floor toward a goal below. Needs bAllowDigging. */
	bool bAllowDigDown = true;
};

/**
 * A* for a 2-voxel-tall walker on the voxel grid.
 *
 * WHY NOT THE NAVMESH
 *   Recast builds navmesh from collision geometry. Every block edit would dirty
 *   a tile, rebuilds lag edits by seconds, and a navmesh has no notion of "this
 *   wall is concrete, that door is wood" - which is the entire point of a horde
 *   night. Pathing directly on voxels gives exact, immediate walkability after
 *   an edit and lets digging be an ordinary edge with a cost.
 *
 * MOVES from a standable cell (air at feet and head, solid below):
 *   walk      to a standable neighbour at the same height            cost 1
 *   step up   onto a neighbour one higher (needs headroom)          cost StepUpCost
 *   drop      down up to MaxDrop voxels                              1 + DropCostPerVoxel each
 *   dig       through a neighbour whose feet/head voxels are solid,
 *             standing on solid ground                                1 + break seconds x DigCostPerSecond
 *   climb     up or down a ladder; up a wall if bClimbWalls           ClimbCostPerVoxel
 *   dig down  through the floor when the goal is below, falling to
 *             the next ground within MaxDrop                          1.5 + break seconds x DigCostPerSecond
 *
 * A climbing walker is not standing, so from a climb node only climbing
 * further, stepping off onto a ledge, or walking onto ground at that height
 * are moves: nothing walks on air.
 *
 * Diagonals are not moves: a 2-wide zombie squeezing between two blocks'
 * corners is the classic voxel-pathing bug, and four-way paths look fine once
 * steering smooths them.
 */
namespace MadFall::Pathfinding
{
	/** Voxel lookup. */
	using FGetVoxel = TFunctionRef<FMadVoxel(const FIntVector&)>;

	/** Seconds this walker needs to break a voxel, or a negative number if it cannot. */
	using FBreakSeconds = TFunctionRef<float(const FIntVector&, const FMadVoxel&)>;

	MADFALLGAMEPLAY_API bool IsStandable(const FIntVector& Feet, FGetVoxel GetVoxel);

	/**
	 * Finds a path from Start (feet voxel) toward Goal (feet voxel). Start need
	 * not be standable - a walker mid-fall still gets a path from where it is.
	 * Returns false only if not even a partial path exists.
	 */
	MADFALLGAMEPLAY_API bool FindPath(const FIntVector& Start, const FIntVector& Goal, const FMadPathSettings& Settings,
		FGetVoxel GetVoxel, FBreakSeconds BreakSeconds, FMadVoxelPath& OutPath);
}

namespace MadFall::Pathfinding
{
	/**
	 * What to hit when the target is out of reach above: a survivor on a
	 * pillar, a roof, a scaffold. A 2-voxel walker cannot climb, so it attacks
	 * what the target stands on and lets structural integrity bring them down -
	 * the 7 Days to Die answer to "I'll just build up".
	 *
	 * Candidates are solid, breakable voxels the walker can reach from where it
	 * stands (horizontally adjacent, at feet or head height) that are under or
	 * beside the goal's column. The goal column itself is preferred - that is
	 * the support - then the quickest to break, then the lowest.
	 *
	 * False when the goal is not above reach (the normal path handles it) or
	 * nothing suitable is in reach (the walker should move closer first).
	 * Natural terrain qualifies but never collapses, so a survivor on a dirt
	 * spire is dug at without falling; that is accepted, not solved.
	 */
	MADFALLGAMEPLAY_API bool FindUndermineTarget(const FIntVector& WalkerFeet, const FIntVector& GoalFeet,
		FGetVoxel GetVoxel, FBreakSeconds BreakSeconds, FIntVector& OutBlock);

	/**
	 * The block to break next to get THROUGH to a goal that no path reaches: a
	 * survivor inside a sealed base.
	 *
	 * WHY IT IS NEEDED: FindPath allows digging, but at DigCostPerSecond a
	 * concrete wall costs as much as a long walk, and on open ground the node
	 * budget runs out exploring the detours before the path through the wall is
	 * ever the cheapest. The search then returns its best partial path - ending
	 * at the wall - and the walker stood there re-planning. Measured: nineteen
	 * zombies around a 13 x 13 concrete shell, 2,574 paths and 8 block hits in
	 * two minutes, nobody inside.
	 *
	 * Candidates are the eight columns around the walker that lie toward the
	 * goal (within 60 degrees), each passable once its feet and head voxels are
	 * gone. The best is the quickest to clear among the most direct; within it,
	 * the feet voxel before the head. Unbreakable columns are skipped; a column
	 * already open is not a breach. False when the goal is within reach, nothing
	 * toward it is breakable, or the walker is not against anything.
	 */
	MADFALLGAMEPLAY_API bool FindBreachTarget(const FIntVector& WalkerFeet, const FIntVector& GoalFeet,
		FGetVoxel GetVoxel, FBreakSeconds BreakSeconds, FIntVector& OutBlock);
}

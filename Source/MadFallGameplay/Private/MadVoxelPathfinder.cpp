// Copyright MadFall. All Rights Reserved.

#include "MadVoxelPathfinder.h"

#include "Algo/Reverse.h"
#include "MadFallStats.h"

DECLARE_CYCLE_STAT(TEXT("Voxel Pathfind"), STAT_MadPathfind, STATGROUP_MadFallStructural);

namespace MadFall::Pathfinding
{
	namespace
	{
		const FIntVector Up(0, 0, 1);
		const FIntVector Horizontal[4] = { FIntVector(1, 0, 0), FIntVector(-1, 0, 0), FIntVector(0, 1, 0), FIntVector(0, -1, 0) };

		FORCEINLINE bool IsSolidVoxel(const FMadVoxel& Voxel)
		{
			return Voxel.IsSolid() && !Voxel.HasFlag(EMadVoxelFlags::Liquid);
		}

		struct FNode
		{
			FIntVector Feet;
			float G = 0.0f;
			float H = 0.0f;
			int32 Parent = INDEX_NONE;
			bool bClosed = false;
			EMadPathMove Move = EMadPathMove::Walk;
			TArray<FIntVector, TInlineAllocator<2>> Breaks;
		};

		struct FOpenEntry
		{
			float F;
			float H;
			int32 Node;
		};

		struct FOpenLess
		{
			bool operator()(const FOpenEntry& A, const FOpenEntry& B) const
			{
				if (A.F != B.F) { return A.F < B.F; }
				if (A.H != B.H) { return A.H < B.H; }
				return A.Node < B.Node;
			}
		};

		float Heuristic(const FIntVector& A, const FIntVector& B)
		{
			return static_cast<float>(FMath::Abs(A.X - B.X) + FMath::Abs(A.Y - B.Y) + FMath::Abs(A.Z - B.Z));
		}

		bool IsAtGoal(const FIntVector& Feet, const FIntVector& Goal, int32 Radius)
		{
			return FMath::Abs(Feet.X - Goal.X) <= Radius && FMath::Abs(Feet.Y - Goal.Y) <= Radius && FMath::Abs(Feet.Z - Goal.Z) <= 1;
		}
	}

	bool IsStandable(const FIntVector& Feet, FGetVoxel GetVoxel)
	{
		return !IsSolidVoxel(GetVoxel(Feet)) && !IsSolidVoxel(GetVoxel(Feet + Up)) && IsSolidVoxel(GetVoxel(Feet - Up));
	}

	bool FindPath(const FIntVector& InStart, const FIntVector& Goal, const FMadPathSettings& Settings,
		FGetVoxel GetVoxel, FBreakSeconds BreakSeconds, FMadVoxelPath& OutPath)
	{
		SCOPE_CYCLE_COUNTER(STAT_MadPathfind);

		OutPath = FMadVoxelPath();

		auto Solid = [&GetVoxel](const FIntVector& V) { return IsSolidVoxel(GetVoxel(V)); };

		// A walker in mid-air paths from where it will land - unless it is holding
		// on. A climber re-plans every few seconds, and planning from the foot of
		// the wall sent it back down the wall each time: a tall climb never
		// finished (the zombie variety CI gate, one run in three).
		auto IsHoldingOn = [&](const FIntVector& P)
		{
			if (Settings.IsClimbable && (Settings.IsClimbable(P) || Settings.IsClimbable(P - Up)))
			{
				return true;
			}
			if (Settings.bClimbWalls)
			{
				for (const FIntVector& Dir : Horizontal)
				{
					if (Solid(P + Dir) || Solid(P + Dir + Up))
					{
						return true;
					}
				}
			}
			return false;
		};
		FIntVector Start = InStart;
		for (int32 Fall = 0; Fall < 16 && !IsStandable(Start, GetVoxel) && !Solid(Start - Up) && !IsHoldingOn(Start); ++Fall)
		{
			Start -= Up;
		}

		TArray<FNode> Nodes;
		TMap<FIntVector, int32> Index;
		TArray<FOpenEntry> Open;
		Nodes.Reserve(Settings.MaxNodes + 16);
		Index.Reserve(Settings.MaxNodes + 16);

		{
			FNode StartNode;
			StartNode.Feet = Start;
			StartNode.H = Heuristic(Start, Goal);
			Index.Add(Start, 0);
			Nodes.Add(MoveTemp(StartNode));
			Open.HeapPush(FOpenEntry{ Nodes[0].H, Nodes[0].H, 0 }, FOpenLess());
		}

		int32 Best = 0;
		int32 Reached = INDEX_NONE;

		auto Consider = [&](int32 From, const FIntVector& Feet, float StepCost, TArray<FIntVector, TInlineAllocator<2>>&& Breaks, EMadPathMove Move)
		{
			const float G = Nodes[From].G + StepCost;
			if (int32* Existing = Index.Find(Feet))
			{
				FNode& Node = Nodes[*Existing];
				if (Node.bClosed || G >= Node.G)
				{
					return;
				}
				Node.G = G;
				Node.Parent = From;
				Node.Move = Move;
				Node.Breaks = MoveTemp(Breaks);
				Open.HeapPush(FOpenEntry{ G + Node.H, Node.H, *Existing }, FOpenLess());
				return;
			}

			FNode Node;
			Node.Feet = Feet;
			Node.G = G;
			Node.H = Heuristic(Feet, Goal);
			Node.Parent = From;
			Node.Move = Move;
			Node.Breaks = MoveTemp(Breaks);
			const int32 NewIndex = Nodes.Add(MoveTemp(Node));
			Index.Add(Feet, NewIndex);
			Open.HeapPush(FOpenEntry{ G + Nodes[NewIndex].H, Nodes[NewIndex].H, NewIndex }, FOpenLess());
		};

		int32 Expanded = 0;
		while (Open.Num() > 0 && Expanded < Settings.MaxNodes)
		{
			FOpenEntry Entry;
			Open.HeapPop(Entry, FOpenLess(), EAllowShrinking::No);

			if (Nodes[Entry.Node].bClosed)
			{
				continue;
			}
			Nodes[Entry.Node].bClosed = true;
			++Expanded;

			const int32 Current = Entry.Node;
			const FIntVector P = Nodes[Current].Feet;

			if (Nodes[Current].H < Nodes[Best].H || (Nodes[Current].H == Nodes[Best].H && Nodes[Current].G < Nodes[Best].G))
			{
				Best = Current;
			}

			if (IsAtGoal(P, Goal, Settings.GoalRadius))
			{
				Reached = Current;
				break;
			}

			const bool bHeadroomAbove = !Solid(P + Up * 2);

			// --- climb: ladders, and walls for walkers that climb them -------------
			const bool bRoomToRise = !Solid(P + Up) && bHeadroomAbove;
			const bool bLadderHere = Settings.IsClimbable && (Settings.IsClimbable(P) || Settings.IsClimbable(P + Up));
			bool bWallBeside = false;
			if (Settings.bClimbWalls)
			{
				for (const FIntVector& Dir : Horizontal)
				{
					if (Solid(P + Dir) || Solid(P + Dir + Up))
					{
						bWallBeside = true;
						break;
					}
				}
			}
			if (bRoomToRise && (bLadderHere || bWallBeside))
			{
				Consider(Current, P + Up, Settings.ClimbCostPerVoxel, {}, EMadPathMove::Climb);
			}
			if (Settings.IsClimbable && Settings.IsClimbable(P - Up) && !Solid(P - Up))
			{
				Consider(Current, P - Up, Settings.ClimbCostPerVoxel, {}, EMadPathMove::Climb);
			}

			// --- dig down, toward a goal below ------------------------------------------
			if (Settings.bAllowDigging && Settings.bAllowDigDown && Goal.Z < P.Z && Solid(P - Up))
			{
				const FIntVector Floor = P - Up;
				const FMadVoxel FloorVoxel = GetVoxel(Floor);
				const float Seconds = BreakSeconds(Floor, FloorVoxel);
				if (Seconds >= 0.0f)
				{
					// Where the walker lands once the floor is gone.
					for (int32 Depth = 1; Depth <= Settings.MaxDrop; ++Depth)
					{
						const FIntVector Landing = P - Up * Depth;
						if (Solid(Landing - Up))
						{
							TArray<FIntVector, TInlineAllocator<2>> Breaks;
							Breaks.Add(Floor);
							Consider(Current, Landing, 1.5f + Seconds * Settings.DigCostPerSecond + Settings.DropCostPerVoxel * (Depth - 1), MoveTemp(Breaks), EMadPathMove::DigDown);
							break;
						}
						if (Depth > 1 && Solid(Landing))
						{
							break;
						}
					}
				}
			}

			for (const FIntVector& Dir : Horizontal)
			{
				const FIntVector Q = P + Dir;
				const bool bFeetBlocked = Solid(Q);
				const bool bHeadBlocked = Solid(Q + Up);
				// The top rung of a ladder holds a walker stepping onto it from a ledge.
				const bool bGround = Solid(Q - Up) || (Settings.IsClimbable && Settings.IsClimbable(Q - Up));

				// --- walk or dig, same level -----------------------------------
				if (bGround)
				{
					if (!bFeetBlocked && !bHeadBlocked)
					{
						Consider(Current, Q, 1.0f, {}, EMadPathMove::Walk);
					}
					else if (Settings.bAllowDigging)
					{
						float Seconds = 0.0f;
						bool bBreakable = true;
						TArray<FIntVector, TInlineAllocator<2>> Breaks;
						for (const FIntVector& Block : { Q, Q + Up })
						{
							const FMadVoxel Voxel = GetVoxel(Block);
							if (!IsSolidVoxel(Voxel))
							{
								continue;
							}
							const float BlockSeconds = BreakSeconds(Block, Voxel);
							if (BlockSeconds < 0.0f)
							{
								bBreakable = false;
								break;
							}
							Seconds += BlockSeconds;
							Breaks.Add(Block);
						}
						if (bBreakable)
						{
							Consider(Current, Q, 1.0f + Seconds * Settings.DigCostPerSecond, MoveTemp(Breaks), EMadPathMove::Dig);
						}
					}
				}

				// --- step up onto a one-voxel ledge ------------------------------
				if (bFeetBlocked && !bHeadBlocked && bHeadroomAbove && !Solid(Q + Up * 2))
				{
					Consider(Current, Q + Up, Settings.StepUpCost, {}, EMadPathMove::StepUp);
				}

				// --- drop: only from where the walker stands ------------------------
				if (!bGround && !bFeetBlocked && !bHeadBlocked && Solid(P - Up))
				{
					for (int32 Depth = 1; Depth <= Settings.MaxDrop; ++Depth)
					{
						const FIntVector Landing = Q - Up * Depth;
						if (Solid(Landing))
						{
							break;
						}
						if (Solid(Landing - Up))
						{
							Consider(Current, Landing, 1.0f + Settings.DropCostPerVoxel * Depth, {}, EMadPathMove::Drop);
							break;
						}
					}
				}
			}
		}

		OutPath.NodesExpanded = Expanded;
		// (moves up, down and through the floor are expanded inside the loop above)

		const int32 End = Reached != INDEX_NONE ? Reached : Best;
		OutPath.bReachesGoal = Reached != INDEX_NONE;
		OutPath.Cost = Nodes[End].G;

		for (int32 Node = End; Node != INDEX_NONE && Nodes[Node].Parent != INDEX_NONE; Node = Nodes[Node].Parent)
		{
			FMadPathStep Step;
			Step.Feet = Nodes[Node].Feet;
			Step.Move = Nodes[Node].Move;
			Step.BlocksToBreak = Nodes[Node].Breaks;
			OutPath.Steps.Add(MoveTemp(Step));
		}
		Algo::Reverse(OutPath.Steps);

		return OutPath.bReachesGoal || OutPath.Steps.Num() > 0;
	}
}

namespace MadFall::Pathfinding
{
	bool FindUndermineTarget(const FIntVector& WalkerFeet, const FIntVector& GoalFeet,
		FGetVoxel GetVoxel, FBreakSeconds BreakSeconds, FIntVector& OutBlock)
	{
		if (GoalFeet.Z <= WalkerFeet.Z + 1)
		{
			return false;
		}

		bool bFound = false;
		bool bBestInColumn = false;
		float BestSeconds = 0.0f;

		for (int32 DZ = 0; DZ <= 1; ++DZ)
		{
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					if (DX == 0 && DY == 0)
					{
						continue;   // its own column: it would be digging the floor under itself
					}

					const FIntVector P = WalkerFeet + FIntVector(DX, DY, DZ);
					if (FMath::Max(FMath::Abs(P.X - GoalFeet.X), FMath::Abs(P.Y - GoalFeet.Y)) > 1)
					{
						continue;
					}

					const FMadVoxel Voxel = GetVoxel(P);
					if (!IsSolidVoxel(Voxel))
					{
						continue;
					}
					const float Seconds = BreakSeconds(P, Voxel);
					if (Seconds < 0.0f)
					{
						continue;
					}

					const bool bInColumn = P.X == GoalFeet.X && P.Y == GoalFeet.Y;
					const bool bBetter = !bFound
						|| (bInColumn && !bBestInColumn)
						|| (bInColumn == bBestInColumn && (Seconds < BestSeconds
							|| (Seconds == BestSeconds && P.Z < OutBlock.Z)));
					if (bBetter)
					{
						bFound = true;
						bBestInColumn = bInColumn;
						BestSeconds = Seconds;
						OutBlock = P;
					}
				}
			}
		}
		return bFound;
	}
}

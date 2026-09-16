// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadVoxelPathfinder.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadPathTests
{
	constexpr uint16 Ground = 1;
	constexpr uint16 Wood = 2;
	constexpr uint16 Concrete = 3;
	constexpr uint16 Bedrock = 4;

	/** Flat ground below z = 0 and whatever the test places. */
	struct FWorld
	{
		TMap<FIntVector, uint16> Blocks;

		FMadVoxel Get(const FIntVector& P) const
		{
			FMadVoxel V;
			V.BlockTypeID = MadFall::BlockTypeAir;
			V.Density = 0;
			V.Damage = 0;
			V.Rotation = 0;
			V.Flags = 0;
			uint16 Id = P.Z < 0 ? Ground : 0;
			if (const uint16* Found = Blocks.Find(P))
			{
				Id = *Found;
			}
			if (Id != 0)
			{
				V.BlockTypeID = Id;
				V.Density = 255;
			}
			return V;
		}

		void Wall(int32 X, int32 YMin, int32 YMax, int32 Height, uint16 Block)
		{
			for (int32 Y = YMin; Y <= YMax; ++Y)
			{
				for (int32 Z = 0; Z < Height; ++Z)
				{
					Blocks.Add(FIntVector(X, Y, Z), Block);
				}
			}
		}
	};

	/** Wood 2 s, concrete 10 s, bedrock unbreakable, terrain 3 s. */
	float BreakSeconds(const FIntVector&, const FMadVoxel& Voxel)
	{
		switch (Voxel.BlockTypeID)
		{
		case Wood:     return 2.0f;
		case Concrete: return 10.0f;
		case Bedrock:  return -1.0f;
		default:       return 3.0f;
		}
	}

	bool Find(const FWorld& World, const FIntVector& Start, const FIntVector& Goal, FMadVoxelPath& Path,
		const FMadPathSettings& Settings = FMadPathSettings())
	{
		return MadFall::Pathfinding::FindPath(Start, Goal, Settings,
			[&World](const FIntVector& P) { return World.Get(P); }, &BreakSeconds, Path);
	}

	bool PathBreaks(const FMadVoxelPath& Path, const FIntVector& Block)
	{
		for (const FMadPathStep& Step : Path.Steps)
		{
			if (Step.BlocksToBreak.Contains(Block)) { return true; }
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadPathfinderTest,
	"MadFall.AI.Pathfinder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadPathfinderTest::RunTest(const FString& Parameters)
{
	using namespace MadPathTests;
	FMadVoxelPath Path;

	// Open ground: a straight walk, no digging.
	{
		FWorld World;
		TestTrue(TEXT("open ground path"), Find(World, FIntVector(0, 0, 0), FIntVector(10, 0, 0), Path));
		TestTrue(TEXT("reaches"), Path.bReachesGoal);
		TestEqual(TEXT("9 steps to within 1 of the goal"), Path.Steps.Num(), 9);
		TestFalse(TEXT("no digging on open ground"), Path.RequiresDigging());
	}

	// A short wall with a gap: walking around is cheaper than digging.
	{
		FWorld World;
		World.Wall(5, -3, 3, 3, Wood);
		// One step aside: the detour (+2) is cheaper than digging two wood blocks (+4).
		World.Blocks.Remove(FIntVector(5, 1, 0));
		World.Blocks.Remove(FIntVector(5, 1, 1));
		TestTrue(TEXT("detour path"), Find(World, FIntVector(0, 0, 0), FIntVector(10, 0, 0), Path));
		TestTrue(TEXT("detour reaches"), Path.bReachesGoal);
		TestFalse(TEXT("walks around through the gap rather than digging"), Path.RequiresDigging());
	}

	// A sealed wall: digs, and picks the weakest section.
	{
		FWorld World;
		World.Wall(5, -20, 20, 3, Concrete);
		World.Blocks.Add(FIntVector(5, 2, 0), Wood);
		World.Blocks.Add(FIntVector(5, 2, 1), Wood);
		TestTrue(TEXT("dig path"), Find(World, FIntVector(0, 0, 0), FIntVector(10, 0, 0), Path));
		TestTrue(TEXT("dig path reaches"), Path.bReachesGoal);
		TestTrue(TEXT("digging required"), Path.RequiresDigging());
		TestTrue(TEXT("digs through the wooden section"), PathBreaks(Path, FIntVector(5, 2, 0)) && PathBreaks(Path, FIntVector(5, 2, 1)));
		TestFalse(TEXT("does not dig the concrete in front of it"), PathBreaks(Path, FIntVector(5, 0, 0)));
	}

	// Unbreakable wall with no gap and digging off: best partial path, not a failure.
	{
		FWorld World;
		World.Wall(5, -40, 40, 3, Bedrock);
		FMadPathSettings Settings;
		Settings.MaxNodes = 800;
		TestTrue(TEXT("partial path exists"), Find(World, FIntVector(0, 0, 0), FIntVector(10, 0, 0), Path, Settings));
		TestFalse(TEXT("does not reach through bedrock"), Path.bReachesGoal);
		TestFalse(TEXT("never digs bedrock"), Path.RequiresDigging());
		TestEqual(TEXT("ends pressed against the wall"), Path.Steps.Last().Feet.X, 4);
	}

	// Steps up a staircase and drops off a ledge.
	{
		FWorld World;
		World.Blocks.Add(FIntVector(2, 0, 0), Wood);
		World.Blocks.Add(FIntVector(3, 0, 0), Wood);
		World.Blocks.Add(FIntVector(3, 0, 1), Wood);
		// A 1-wide staircase in a 1-wide trench so the walker cannot go round it.
		for (int32 X = 0; X <= 8; ++X)
		{
			World.Blocks.Add(FIntVector(X, 1, 0), Bedrock); World.Blocks.Add(FIntVector(X, 1, 1), Bedrock); World.Blocks.Add(FIntVector(X, 1, 2), Bedrock); World.Blocks.Add(FIntVector(X, 1, 3), Bedrock);
			World.Blocks.Add(FIntVector(X, -1, 0), Bedrock); World.Blocks.Add(FIntVector(X, -1, 1), Bedrock); World.Blocks.Add(FIntVector(X, -1, 2), Bedrock); World.Blocks.Add(FIntVector(X, -1, 3), Bedrock);
		}
		FMadPathSettings NoDig;
		NoDig.bAllowDigging = false;
		NoDig.GoalRadius = 0;
		TestTrue(TEXT("stair path"), Find(World, FIntVector(0, 0, 0), FIntVector(6, 0, 0), Path, NoDig));
		TestTrue(TEXT("stair path reaches"), Path.bReachesGoal);

		bool bClimbedTo2 = false;
		for (const FMadPathStep& Step : Path.Steps) { bClimbedTo2 |= Step.Feet == FIntVector(3, 0, 2); }
		TestTrue(TEXT("climbed onto the top step"), bClimbedTo2);
		TestEqual(TEXT("ends back on the ground"), Path.Steps.Last().Feet, FIntVector(6, 0, 0));
	}

	// Mid-air start paths from the landing point.
	{
		FWorld World;
		TestTrue(TEXT("falling start"), Find(World, FIntVector(0, 0, 6), FIntVector(3, 0, 0), Path));
		TestTrue(TEXT("falling start reaches"), Path.bReachesGoal);
		TestEqual(TEXT("first step is at ground level"), Path.Steps[0].Feet.Z, 0);
	}

	// Budget: a big open field search stays cheap.
	{
		FWorld World;
		for (int32 I = 0; I < 400; ++I)
		{
			World.Blocks.Add(FIntVector((I * 37) % 60 - 5, (I * 53) % 60 - 30, 0), Concrete);
		}
		const double Start = FPlatformTime::Seconds();
		Find(World, FIntVector(0, 0, 0), FIntVector(48, 7, 0), Path);
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
		TestTrue(TEXT("finds a way across a cluttered field"), Path.bReachesGoal);
		TestTrue(FString::Printf(TEXT("under 3000 nodes (%d)"), Path.NodesExpanded), Path.NodesExpanded <= 3000);
		TestTrue(FString::Printf(TEXT("under 10 ms (%.2f ms)"), Ms), Ms < 10.0);
		AddInfo(FString::Printf(TEXT("Cluttered field: %d nodes, %.2f ms, cost %.1f, %d steps."), Path.NodesExpanded, Ms, Path.Cost, Path.Steps.Num()));
	}

	// Horde night: the goal is inside a sealed room 30 voxels away, walls 3 high
	// and one concrete wall facing the zombie. The search must commit to digging
	// through a wooden wall within budget.
	{
		FWorld World;
		World.Wall(36, -4, 4, 3, Concrete);
		World.Wall(44, -4, 4, 3, Wood);
		for (int32 X = 36; X <= 44; ++X)
		{
			for (int32 Z = 0; Z < 3; ++Z)
			{
				World.Blocks.Add(FIntVector(X, -4, Z), Wood);
				World.Blocks.Add(FIntVector(X, 4, Z), Wood);
			}
		}
		const double Start = FPlatformTime::Seconds();
		Find(World, FIntVector(10, 0, 0), FIntVector(40, 0, 0), Path);
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
		TestTrue(TEXT("reaches into the sealed room"), Path.bReachesGoal);
		TestTrue(TEXT("by digging"), Path.RequiresDigging());
		TestTrue(TEXT("through wood, not the concrete wall it faces"), !PathBreaks(Path, FIntVector(36, 0, 0)) && !PathBreaks(Path, FIntVector(36, 0, 1)));
		TestTrue(FString::Printf(TEXT("sealed room under 10 ms (%.2f ms)"), Ms), Ms < 10.0);
		AddInfo(FString::Printf(TEXT("Sealed room: %d nodes, %.2f ms, cost %.1f, %d steps."), Path.NodesExpanded, Ms, Path.Cost, Path.Steps.Num()));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWaterPathTest,
	"MadFall.AI.WaterPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWaterPathTest::RunTest(const FString& Parameters)
{
	using namespace MadPathTests;

	// A pond across the straight line from start to goal, with dry ground round
	// it: the voxels at Z = 0 for -1 <= Y <= 1 and 2 <= X <= 6 are water.
	auto IsPond = [](const FIntVector& V)
	{
		return V.Z == 0 && V.X >= 2 && V.X <= 6 && V.Y >= -1 && V.Y <= 1;
	};

	FWorld World;
	const FIntVector Start(0, 0, 0);
	const FIntVector Goal(8, 0, 0);

	// Water is passable: without a cost for it, the way through is the way taken.
	FMadVoxelPath Straight;
	TestTrue(TEXT("a path exists across the pond"), Find(World, Start, Goal, Straight));
	const int32 StraightSteps = Straight.Steps.Num();

	FMadPathSettings Wet;
	Wet.IsLiquid = IsPond;
	FMadVoxelPath Around;
	TestTrue(TEXT("and still exists once water costs something"), Find(World, Start, Goal, Around, Wet));

	int32 WetSteps = 0;
	for (const FMadPathStep& Step : Around.Steps)
	{
		WetSteps += IsPond(Step.Feet) ? 1 : 0;
	}
	TestEqual(TEXT("the dry way round is taken"), WetSteps, 0);
	TestTrue(TEXT("and it is longer than the straight line through"), Around.Steps.Num() > StraightSteps);

	// A pond with no way round is still crossed: water is a toll, not a wall.
	auto IsCanal = [](const FIntVector& V) { return V.Z == 0 && V.X >= 2 && V.X <= 4; };
	FMadPathSettings Canal;
	Canal.IsLiquid = IsCanal;
	FMadVoxelPath Through;
	TestTrue(TEXT("a canal across the world is crossed anyway"), Find(World, Start, Goal, Through, Canal));
	int32 CanalSteps = 0;
	for (const FMadPathStep& Step : Through.Steps)
	{
		CanalSteps += IsCanal(Step.Feet) ? 1 : 0;
	}
	TestTrue(TEXT("by wading through it"), CanalSteps > 0);

	// Deeper water costs more than a wade, so a shallow ford wins over a channel.
	auto IsDeep = [](const FIntVector& V) { return (V.Z == 0 || V.Z == 1) && V.X >= 2 && V.X <= 4 && V.Y <= 0; };
	FMadPathSettings Ford;
	Ford.IsLiquid = [&IsDeep](const FIntVector& V)
	{
		// Deep channel south of the line, one voxel of water north of it.
		return IsDeep(V) || (V.Z == 0 && V.X >= 2 && V.X <= 4 && V.Y > 0);
	};
	FMadVoxelPath Crossing;
	TestTrue(TEXT("a crossing is found"), Find(World, Start, Goal, Crossing, Ford));
	bool bUsedDeep = false;
	for (const FMadPathStep& Step : Crossing.Steps)
	{
		bUsedDeep = bUsedDeep || IsDeep(Step.Feet);
	}
	TestFalse(TEXT("the shallow ford is preferred to the deep channel"), bUsedDeep);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadUndermineTest,
	"MadFall.AI.Undermine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadUndermineTest::RunTest(const FString& Parameters)
{
	using namespace MadPathTests;
	auto Get = [](const FWorld& World) { return [&World](const FIntVector& P) { return World.Get(P); }; };

	// A survivor on a 3-high wooden pillar at (2,0): feet at z = 3.
	FWorld World;
	for (int32 Z = 0; Z < 3; ++Z)
	{
		World.Blocks.Add(FIntVector(2, 0, Z), Wood);
	}
	const FIntVector Goal(2, 0, 3);

	// No walking path gets within reach.
	FMadVoxelPath Path;
	TestTrue(TEXT("a partial path exists"), Find(World, FIntVector(-6, 0, 0), Goal, Path));
	TestFalse(TEXT("but it cannot reach a survivor three blocks up"), Path.bReachesGoal);

	FIntVector Block;
	TestFalse(TEXT("too far away to reach the pillar"),
		MadFall::Pathfinding::FindUndermineTarget(FIntVector(-6, 0, 0), Goal, Get(World), &BreakSeconds, Block));

	TestTrue(TEXT("next to the pillar: hit it"),
		MadFall::Pathfinding::FindUndermineTarget(FIntVector(1, 0, 0), Goal, Get(World), &BreakSeconds, Block));
	TestEqual(TEXT("the pillar base, the lowest support in reach"), Block, FIntVector(2, 0, 0));

	TestTrue(TEXT("diagonal neighbours reach it too"),
		MadFall::Pathfinding::FindUndermineTarget(FIntVector(1, 1, 0), Goal, Get(World), &BreakSeconds, Block));

	// The support column beats a quicker block beside it.
	{
		FWorld Braced = World;
		Braced.Blocks.Add(FIntVector(2, 0, 0), Concrete);   // 10 s
		Braced.Blocks.Add(FIntVector(2, 0, 1), Concrete);
		Braced.Blocks.Add(FIntVector(1, 1, 0), Wood);       // 2 s, beside the column
		TestTrue(TEXT("braced pillar"), MadFall::Pathfinding::FindUndermineTarget(FIntVector(1, 0, 0), Goal, Get(Braced), &BreakSeconds, Block));
		TestEqual(TEXT("goes for the support column, not the easy block beside it"), Block.X * 10 + Block.Y, 20);
	}

	// Within reach vertically: no undermining, the normal attack handles it.
	TestFalse(TEXT("a target one block up is not undermined"),
		MadFall::Pathfinding::FindUndermineTarget(FIntVector(1, 0, 0), FIntVector(2, 0, 1), Get(World), &BreakSeconds, Block));

	// Unbreakable support: nothing to do.
	{
		FWorld Bedrocked;
		for (int32 Z = 0; Z < 3; ++Z)
		{
			Bedrocked.Blocks.Add(FIntVector(2, 0, Z), Bedrock);
		}
		TestFalse(TEXT("bedrock pillar cannot be undermined"),
			MadFall::Pathfinding::FindUndermineTarget(FIntVector(1, 0, 0), Goal, Get(Bedrocked), &BreakSeconds, Block));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadBreachTest,
	"MadFall.AI.Breach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadBreachTest::RunTest(const FString& Parameters)
{
	using namespace MadPathTests;
	auto Get = [](const FWorld& World) { return [&World](const FIntVector& P) { return World.Get(P); }; };

	// A survivor at (5,0) behind a concrete wall at x = 2, three high, walker at (1,0) against it.
	FWorld World;
	World.Wall(2, -8, 8, 3, Concrete);
	const FIntVector Goal(5, 0, 0);
	FIntVector Block;

	TestTrue(TEXT("against the wall: break through it"),
		MadFall::Pathfinding::FindBreachTarget(FIntVector(1, 0, 0), Goal, Get(World), &BreakSeconds, Block));
	TestEqual(TEXT("straight toward the survivor, feet first"), Block, FIntVector(2, 0, 0));

	// Once the feet voxel is gone, the head voxel of the same column.
	World.Blocks.Remove(FIntVector(2, 0, 0));
	TestTrue(TEXT("head next"), MadFall::Pathfinding::FindBreachTarget(FIntVector(1, 0, 0), Goal, Get(World), &BreakSeconds, Block));
	TestEqual(TEXT("the same column's head voxel"), Block, FIntVector(2, 0, 1));
	World.Blocks.Remove(FIntVector(2, 0, 1));
	TestTrue(TEXT("with the column open, no breach needed straight ahead - a diagonal one may still be offered, but not the open column"),
		!MadFall::Pathfinding::FindBreachTarget(FIntVector(1, 0, 0), Goal, Get(World), &BreakSeconds, Block) || Block.Y != 0);

	// A much weaker panel beside the direct line is taken when it is still toward the goal.
	{
		FWorld Patched;
		Patched.Wall(2, -8, 8, 3, Concrete);
		Patched.Blocks.Add(FIntVector(2, 1, 0), Wood);
		Patched.Blocks.Add(FIntVector(2, 1, 1), Wood);
		TestTrue(TEXT("patched wall"), MadFall::Pathfinding::FindBreachTarget(FIntVector(1, 0, 0), FIntVector(6, 0, 0), Get(Patched), &BreakSeconds, Block));
		TestEqual(TEXT("the direct column wins over a diagonal at similar cost - but here the diagonal is 45 degrees off, so direct concrete it is"), Block, FIntVector(2, 0, 0));
	}

	// Not toward the goal, in reach, or nothing breakable: no breach.
	TestFalse(TEXT("a wall behind the walker is not a breach"),
		MadFall::Pathfinding::FindBreachTarget(FIntVector(3, 0, 0), FIntVector(8, 0, 0), Get(World), &BreakSeconds, Block));
	TestFalse(TEXT("in reach: the attack handles it"),
		MadFall::Pathfinding::FindBreachTarget(FIntVector(1, 0, 0), FIntVector(2, 0, 0), Get(World), &BreakSeconds, Block));
	{
		FWorld Vault;
		Vault.Wall(2, -8, 8, 3, Bedrock);
		TestFalse(TEXT("bedrock cannot be breached"),
			MadFall::Pathfinding::FindBreachTarget(FIntVector(1, 0, 0), Goal, Get(Vault), &BreakSeconds, Block));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadPathClimbTest,
	"MadFall.AI.Climbing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadPathClimbTest::RunTest(const FString& Parameters)
{
	using namespace MadPathTests;
	FMadVoxelPath Path;

	auto HasMove = [](const FMadVoxelPath& P, EMadPathMove Move)
	{
		return P.Steps.ContainsByPredicate([Move](const FMadPathStep& Step) { return Step.Move == Move; });
	};

	// A bedrock cliff four voxels high: x >= 3, z 0..3. The goal is on top.
	auto Cliff = [](FWorld& World)
	{
		for (int32 X = 3; X <= 8; ++X)
		{
			for (int32 Y = -12; Y <= 12; ++Y)
			{
				for (int32 Z = 0; Z < 4; ++Z)
				{
					World.Blocks.Add(FIntVector(X, Y, Z), Bedrock);
				}
			}
		}
	};
	const FIntVector Start(0, 0, 0);
	const FIntVector OnTop(6, 0, 4);

	FMadPathSettings Walker;
	Walker.bAllowDigging = false;

	// No ladder, no climbing: a walker cannot get up.
	{
		FWorld World;
		Cliff(World);
		Find(World, Start, OnTop, Path, Walker);
		TestFalse(TEXT("a walker cannot scale a four-voxel cliff"), Path.bReachesGoal);
	}

	// A ladder against the cliff: pathing sees open air (ladders are walk-through), and IsClimbable says what it is.
	{
		FWorld World;
		Cliff(World);
		TSet<FIntVector> Ladder = { FIntVector(2, 0, 0), FIntVector(2, 0, 1), FIntVector(2, 0, 2), FIntVector(2, 0, 3) };
		FMadPathSettings Settings = Walker;
		Settings.IsClimbable = [&Ladder](const FIntVector& V) { return Ladder.Contains(V); };
		TestTrue(TEXT("ladder path"), Find(World, Start, OnTop, Path, Settings));
		TestTrue(TEXT("a walker climbs the ladder to the top"), Path.bReachesGoal);
		TestTrue(TEXT("with climb steps"), HasMove(Path, EMadPathMove::Climb));
		TestTrue(TEXT("up the ladder column"), Path.Steps.ContainsByPredicate([](const FMadPathStep& Step)
		{
			return Step.Move == EMadPathMove::Climb && Step.Feet.X == 2 && Step.Feet.Y == 0;
		}));

		// And back down.
		TestTrue(TEXT("down path"), Find(World, OnTop, Start, Path, Settings));
		TestTrue(TEXT("a walker climbs down again rather than being stuck"), Path.bReachesGoal);
	}

	// A climbing variant scales the bare cliff.
	{
		FWorld World;
		Cliff(World);
		FMadPathSettings Settings = Walker;
		Settings.bClimbWalls = true;
		TestTrue(TEXT("wall path"), Find(World, Start, OnTop, Path, Settings));
		TestTrue(TEXT("a climber reaches the top"), Path.bReachesGoal);
		const int32 Climbs = Path.Steps.FilterByPredicate([](const FMadPathStep& Step) { return Step.Move == EMadPathMove::Climb; }).Num();
		TestEqual(TEXT("four voxels of climbing, then a step onto the top"), Climbs, 3);
		TestTrue(TEXT("steps off the wall onto the top"), HasMove(Path, EMadPathMove::StepUp));

		// Re-planning halfway up carries on up: it does not path from the foot of the wall.
		const FIntVector HalfwayUp(2, 0, 2);
		TestTrue(TEXT("mid-wall path"), Find(World, HalfwayUp, OnTop, Path, Settings));
		TestTrue(TEXT("a climber re-planning halfway up still reaches the top"), Path.bReachesGoal);
		TestTrue(TEXT("its first step climbs on from where it clings"), Path.Steps.Num() > 0
			&& Path.Steps[0].Move == EMadPathMove::Climb && Path.Steps[0].Feet == HalfwayUp + FIntVector(0, 0, 1));

		// A walker that cannot climb, at the same spot in the air, still plans from where it lands.
		Find(World, HalfwayUp, Start, Path, Walker);
		TestTrue(TEXT("a falling walker plans from the ground"), Path.Steps.Num() > 0 && Path.Steps[0].Feet.Z == 0);
	}

	// A climber in open ground walks: nothing to climb, and no climbing on air.
	{
		FWorld World;
		FMadPathSettings Settings = Walker;
		Settings.bClimbWalls = true;
		Find(World, Start, FIntVector(8, 0, 0), Path, Settings);
		TestFalse(TEXT("no climbing without a wall"), HasMove(Path, EMadPathMove::Climb));
	}

	// Digging down to a survivor in a cellar: air at z -3 and -2 under the start.
	{
		FWorld World;
		World.Blocks.Add(FIntVector(4, 0, -2), 0);
		World.Blocks.Add(FIntVector(4, 0, -3), 0);
		const FIntVector Cellar(4, 0, -3);
		FMadPathSettings Hunter;
		TestTrue(TEXT("cellar path"), Find(World, Start, Cellar, Path, Hunter));
		TestTrue(TEXT("reaches the cellar"), Path.bReachesGoal);
		TestTrue(TEXT("by digging through the floor"), HasMove(Path, EMadPathMove::DigDown));
		TestTrue(TEXT("breaking the one voxel over the cellar"), PathBreaks(Path, FIntVector(4, 0, -1)));

		FMadPathSettings Wanderer;
		Wanderer.bAllowDigging = false;
		Find(World, Start, Cellar, Path, Wanderer);
		TestFalse(TEXT("a wanderer does not dig down"), HasMove(Path, EMadPathMove::DigDown));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

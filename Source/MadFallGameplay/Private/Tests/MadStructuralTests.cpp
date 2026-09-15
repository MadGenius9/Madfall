// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBlockDamage.h"
#include "MadBlockRegistry.h"
#include "MadChunkStorage.h"
#include "MadFallCoordinates.h"
#include "MadPrefabRegistry.h"
#include "MadStructuralSolver.h"
#include "MadVoxelWorldSubsystem.h"
#include "Math/RandomStream.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadStructuralTests
{
	constexpr uint16 Stone = 1;
	constexpr uint16 Concrete = 2;
	constexpr uint16 Wood = 3;
	constexpr uint16 Steel = 4;

	/**
	 * Flat natural ground below z = 0, air above, and whatever the test builds.
	 * Anything at X >= UnloadedFromX reports as not loaded.
	 */
	class FTestWorld final : public IMadStructuralWorld
	{
	public:
		TMap<FIntVector, FMadVoxel> Voxels;
		int32 UnloadedFromX = MAX_int32;

		virtual bool IsLoaded(const FIntVector& Position) const override
		{
			return Position.X < UnloadedFromX;
		}

		virtual FMadVoxel GetVoxel(const FIntVector& Position) const override
		{
			if (const FMadVoxel* Found = Voxels.Find(Position))
			{
				return *Found;
			}

			FMadVoxel Voxel = MakeAir();
			if (Position.Z < 0)
			{
				// Natural terrain: solid, NOT cubic.
				Voxel.BlockTypeID = Stone;
				Voxel.Density = 255;
			}
			return Voxel;
		}

		static FMadVoxel MakeAir()
		{
			FMadVoxel Voxel;
			Voxel.BlockTypeID = MadFall::BlockTypeAir;
			Voxel.Density = 0;
			Voxel.Damage = 0;
			Voxel.Rotation = 0;
			Voxel.Flags = 0;
			return Voxel;
		}

		void Place(int32 X, int32 Y, int32 Z, uint16 Block, uint8 Damage = 0)
		{
			FMadVoxel Voxel = MakeAir();
			Voxel.BlockTypeID = Block;
			Voxel.Density = 255;
			Voxel.Damage = Damage;
			Voxel.SetFlag(EMadVoxelFlags::Cubic, true);
			Voxels.Add(FIntVector(X, Y, Z), Voxel);
		}

		void Remove(int32 X, int32 Y, int32 Z)
		{
			Voxels.Add(FIntVector(X, Y, Z), MakeAir());
		}

		TArray<FIntVector> AllPositions() const
		{
			TArray<FIntVector> Out;
			Voxels.GetKeys(Out);
			Out.Sort([](const FIntVector& A, const FIntVector& B)
			{
				if (A.Z != B.Z) { return A.Z < B.Z; }
				if (A.Y != B.Y) { return A.Y < B.Y; }
				return A.X < B.X;
			});
			return Out;
		}
	};

	FMadStructuralMaterials MakeMaterials()
	{
		FMadStructuralMaterials Materials;

		FMadStructuralMaterial StoneMat;
		StoneMat.bKnown = true;
		StoneMat.bIsAnchor = true;
		StoneMat.MassKg = 2600.0f;
		Materials.Set(Stone, StoneMat);

		FMadStructuralMaterial ConcreteMat;
		ConcreteMat.bKnown = true;
		ConcreteMat.MassKg = 1800.0f;
		ConcreteMat.SupportStrength = 1.0e7f;
		ConcreteMat.MaxHorizontalSpan = 6;
		ConcreteMat.Stages = { { 0, 1.0f }, { 112, 0.55f }, { 200, 0.2f }, { 255, 0.0f } };
		Materials.Set(Concrete, ConcreteMat);

		FMadStructuralMaterial WoodMat;
		WoodMat.bKnown = true;
		WoodMat.MassKg = 250.0f;
		WoodMat.SupportStrength = 800.0f;
		WoodMat.MaxHorizontalSpan = 2;
		Materials.Set(Wood, WoodMat);

		FMadStructuralMaterial SteelMat;
		SteelMat.bKnown = true;
		SteelMat.MassKg = 100.0f;
		SteelMat.SupportStrength = 1.0e9f;
		SteelMat.MaxHorizontalSpan = 14;
		Materials.Set(Steel, SteelMat);

		return Materials;
	}

	TArray<FMadStructuralFailureRecord> Solve(const FTestWorld& World, const FMadStructuralMaterials& Materials,
		TArray<FIntVector> Seeds, const FMadStructuralSettings& Settings = FMadStructuralSettings())
	{
		FMadStructuralJob Job(MoveTemp(Seeds), Settings);
		Job.RunToCompletion(World, Materials);
		return Job.GetFailures();
	}

	TArray<FIntVector> SeedsAround(int32 X, int32 Y, int32 Z)
	{
		return {
			FIntVector(X, Y, Z),
			FIntVector(X + 1, Y, Z), FIntVector(X - 1, Y, Z),
			FIntVector(X, Y + 1, Z), FIntVector(X, Y - 1, Z),
			FIntVector(X, Y, Z + 1), FIntVector(X, Y, Z - 1)
		};
	}

	bool ContainsPosition(const TArray<FMadStructuralFailureRecord>& Failures, const FIntVector& Position)
	{
		return Failures.ContainsByPredicate([&Position](const FMadStructuralFailureRecord& R) { return R.Position == Position; });
	}

	/** A concrete column at x=0 up to z=Top, and a cantilever along +X at z=Top. */
	void BuildCantilever(FTestWorld& World, int32 Top, int32 Length, uint16 Beam = Concrete, uint8 BeamDamage = 0)
	{
		for (int32 Z = 0; Z <= Top; ++Z)
		{
			World.Place(0, 0, Z, Concrete);
		}
		for (int32 X = 1; X <= Length; ++X)
		{
			World.Place(X, 0, Top, Beam, BeamDamage);
		}
	}
}

// ===========================================================================
// Span
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralSpanTest,
	"MadFall.Structural.Span",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralSpanTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	// Exactly at span: stands.
	{
		FTestWorld World;
		BuildCantilever(World, 4, 6);
		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, World.AllPositions());
		TestEqual(TEXT("span-6 concrete holds a 6-block cantilever"), Failures.Num(), 0);
	}

	// One past span: exactly the last block drops.
	{
		FTestWorld World;
		BuildCantilever(World, 4, 7);
		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, World.AllPositions());
		TestEqual(TEXT("a 7-block cantilever loses exactly one block"), Failures.Num(), 1);
		TestTrue(TEXT("the block that drops is the 7th"), ContainsPosition(Failures, FIntVector(7, 0, 4)));
		if (Failures.Num() == 1)
		{
			TestTrue(TEXT("it fails for lack of support"), Failures[0].Reason == EMadStructuralFailure::Unsupported);
		}
	}

	// Mixed materials: wood on the end of a steel cantilever reaches less far
	// than steel alone, because costs sum along the path.
	{
		FTestWorld World;
		for (int32 Z = 0; Z <= 4; ++Z) { World.Place(0, 0, Z, Steel); }
		for (int32 X = 1; X <= 12; ++X) { World.Place(X, 0, 4, Steel); }
		World.Place(13, 0, 4, Wood);
		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, World.AllPositions());
		TestEqual(TEXT("wood beyond 12 steel steps is over budget"), Failures.Num(), 1);
		TestTrue(TEXT("it is the wood block"), ContainsPosition(Failures, FIntVector(13, 0, 4)));
	}

	// A bridge between two columns only needs half its length from each.
	{
		FTestWorld World;
		for (int32 Z = 0; Z <= 3; ++Z) { World.Place(0, 0, Z, Concrete); World.Place(11, 0, Z, Concrete); }
		for (int32 X = 1; X <= 10; ++X) { World.Place(X, 0, 3, Concrete); }
		TestEqual(TEXT("a 10-block bridge on two columns stands"),
			Solve(World, Materials, World.AllPositions()).Num(), 0);

		// Take out one column's base: that column is now floating, and the half
		// of the bridge more than 6 steps from the surviving column goes with it.
		World.Remove(0, 0, 0);
		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, SeedsAround(0, 0, 0));
		TestTrue(TEXT("the orphaned column falls"), ContainsPosition(Failures, FIntVector(0, 0, 1)));
		TestTrue(TEXT("bridge block 7 steps out falls"), ContainsPosition(Failures, FIntVector(4, 0, 3)));
		TestFalse(TEXT("bridge block 6 steps out stands"), ContainsPosition(Failures, FIntVector(5, 0, 3)));
		TestEqual(TEXT("3 column blocks + 4 bridge blocks fall"), Failures.Num(), 7);
	}

	return true;
}

// ===========================================================================
// Strain: the member that creaks
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralStrainTest,
	"MadFall.Structural.Strain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralStrainTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	auto MostStressed = [&Materials](const FTestWorld& World, FMadStressSample& Out)
	{
		FMadStructuralJob Job(World.AllPositions(), FMadStructuralSettings());
		Job.RunToCompletion(World, Materials);
		return Job.GetMostStressed(Out);
	};

	// Half span: the tip is the most stressed member, at half its limit - no creak.
	{
		FTestWorld World;
		BuildCantilever(World, 4, 3);
		FMadStressSample Sample;
		TestTrue(TEXT("a sound structure reports its most stressed member"), MostStressed(World, Sample));
		TestTrue(TEXT("the tip of a 3-block cantilever"), Sample.Position == FIntVector(3, 0, 4));
		TestTrue(TEXT("at half its span"), FMath::IsNearlyEqual(Sample.Stress, 0.5f, 0.01f));
	}

	// At span: the tip is at its limit and still standing - the creak case.
	{
		FTestWorld World;
		BuildCantilever(World, 4, 6);
		FMadStressSample Sample;
		TestTrue(TEXT("a structure at its limit reports it"), MostStressed(World, Sample));
		TestTrue(TEXT("the 6th block"), Sample.Position == FIntVector(6, 0, 4));
		TestTrue(TEXT("at 100%"), FMath::IsNearlyEqual(Sample.Stress, 1.0f, 0.01f));
		TestFalse(TEXT("and not failing"), Sample.bFailing);
	}

	// Past span: the failing 7th block is not what creaks; the 6th is.
	{
		FTestWorld World;
		BuildCantilever(World, 4, 7);
		FMadStressSample Sample;
		TestTrue(TEXT("failures are skipped"), MostStressed(World, Sample));
		TestTrue(TEXT("the surviving 6th block"), Sample.Position == FIntVector(6, 0, 4));
	}

	// Load, not span: a wood post carrying close to its strength.
	{
		FTestWorld World;
		World.Place(0, 0, 0, Wood);
		for (int32 Z = 1; Z <= 3; ++Z) { World.Place(0, 0, Z, Wood); }   // 3 x 250 kg on an 800 kg post
		FMadStressSample Sample;
		TestTrue(TEXT("a loaded post"), MostStressed(World, Sample));
		TestTrue(TEXT("the bottom block carries the most"), Sample.Position == FIntVector(0, 0, 0));
		TestTrue(TEXT("750 of 800 kg"), FMath::IsNearlyEqual(Sample.Stress, 750.0f / 800.0f, 0.01f));
	}
	return true;
}

// ===========================================================================
// Support removal, hanging, unloaded chunks
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralChunkLoadMidJobTest,
	"MadFall.Structural.ChunkLoadMidJob",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralChunkLoadMidJobTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	// A concrete pillar at x=0 with two beams off its top, x=1..2 at z=3 and
	// z=4. Everything at x >= 2 starts unloaded and streams in partway through
	// the gather - after (1,0,3) has recorded (2,0,3) as an unloaded anchor,
	// before the upper beam reaches x=2.
	FTestWorld World;
	for (int32 Z = 0; Z <= 4; ++Z)
	{
		World.Place(0, 0, Z, Concrete);
	}
	for (int32 X = 1; X <= 2; ++X)
	{
		World.Place(X, 0, 3, Concrete);
		World.Place(X, 0, 4, Concrete);
	}
	World.UnloadedFromX = 2;

	FMadStructuralJob Job({ FIntVector(0, 0, 0) }, FMadStructuralSettings());
	// One seed, then the pillar (4) and (1,0,3): six work units.
	Job.Step(World, Materials, 6);
	World.UnloadedFromX = MAX_int32;
	Job.RunToCompletion(World, Materials);

	TestEqual(TEXT("nothing fails when a chunk loads mid-gather"), Job.GetFailures().Num(), 0);

	// (2,0,3) is one concrete step from (1,0,3), which is one from the pillar.
	// Before edges were made two-way it could only be reached by hanging under
	// the upper beam (three steps), because (1,0,3) still thought it was an
	// unloaded anchor; a longer beam in that position dropped as unsupported.
	FMadStructuralNodeReport Report;
	const int32 Step = static_cast<int32>(MadFall::Structural::SupportScale / 6);
	if (TestTrue(TEXT("(2,0,3) was gathered"), Job.GetNodeReport(FIntVector(2, 0, 3), Report)))
	{
		TestEqual(TEXT("support flows back across an edge first seen while unloaded"), Report.SupportDistance, 2 * Step);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralSupportTest,
	"MadFall.Structural.Support",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralSupportTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	// Removing a column base brings down everything that depended on it.
	{
		FTestWorld World;
		BuildCantilever(World, 4, 3);
		World.Remove(0, 0, 0);
		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, SeedsAround(0, 0, 0));
		TestEqual(TEXT("4 column blocks + 3 beam blocks fall"), Failures.Num(), 7);
	}

	// Seeds that are not members still reach the structure through neighbours.
	{
		FTestWorld World;
		World.Place(0, 0, 5, Concrete);
		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, SeedsAround(0, 0, 4));
		TestEqual(TEXT("a floating block found from a neighbouring seed"), Failures.Num(), 1);
	}

	// Hanging: a chain below a beam, clear of the column, costs a step per block.
	{
		FTestWorld World;
		for (int32 Z = 0; Z <= 9; ++Z) { World.Place(0, 0, Z, Concrete); }
		World.Place(1, 0, 9, Concrete);
		World.Place(2, 0, 9, Concrete);
		for (int32 Z = 8; Z >= 3; --Z) { World.Place(2, 0, Z, Concrete); }

		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, World.AllPositions());
		// beam x2 = 2 steps, z8 = 3 ... z5 = 6 (limit), z4 = 7, z3 = 8.
		TestEqual(TEXT("two hanging blocks beyond span fall"), Failures.Num(), 2);
		TestTrue(TEXT("z=4 falls"), ContainsPosition(Failures, FIntVector(2, 0, 4)));
		TestFalse(TEXT("z=5 holds"), ContainsPosition(Failures, FIntVector(2, 0, 5)));
	}

	// An unloaded neighbour is an anchor: half a building streaming out must not
	// knock down the half that is loaded.
	{
		FTestWorld World;
		World.UnloadedFromX = 100;
		for (int32 X = 95; X <= 99; ++X) { World.Place(X, 0, 20, Concrete); }
		TestEqual(TEXT("beam into an unloaded chunk stands"), Solve(World, Materials, World.AllPositions()).Num(), 0);

		World.UnloadedFromX = MAX_int32;
		TestEqual(TEXT("the same beam with nothing beyond it falls"), Solve(World, Materials, World.AllPositions()).Num(), 5);
	}

	// Unknown block ids are anchors, never members.
	{
		FTestWorld World;
		World.Place(0, 0, 30, 999);
		World.Place(1, 0, 30, Concrete);
		TestEqual(TEXT("block hanging off an unknown id is held"), Solve(World, Materials, World.AllPositions()).Num(), 0);
	}

	return true;
}

// ===========================================================================
// Damage stages
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralDamageStageTest,
	"MadFall.Structural.DamageStages",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralDamageStageTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	// Damage 120 is the 0.55 stage: span 6 becomes 3.3, so 3 blocks reach.
	{
		FTestWorld World;
		BuildCantilever(World, 4, 6, Concrete, 120);
		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, World.AllPositions());
		TestEqual(TEXT("cracked concrete spans 3, not 6"), Failures.Num(), 3);
		TestFalse(TEXT("3rd block holds"), ContainsPosition(Failures, FIntVector(3, 0, 4)));
		TestTrue(TEXT("4th block falls"), ContainsPosition(Failures, FIntVector(4, 0, 4)));
	}

	// A block at a zero-multiplier stage rests where it is but holds nothing up.
	{
		FTestWorld World;
		for (int32 Z = 0; Z <= 4; ++Z) { World.Place(0, 0, Z, Concrete, Z == 2 ? 255 : 0); }
		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, World.AllPositions());
		TestFalse(TEXT("the ruined block itself is still resting"), ContainsPosition(Failures, FIntVector(0, 0, 2)));
		TestEqual(TEXT("the two blocks above it fall"), Failures.Num(), 2);
	}

	TestEqual(TEXT("stage index at 0"), Materials.Get(Concrete).GetStageIndex(0), 0);
	TestEqual(TEXT("stage index at 111"), Materials.Get(Concrete).GetStageIndex(111), 0);
	TestEqual(TEXT("stage index at 112"), Materials.Get(Concrete).GetStageIndex(112), 1);
	TestEqual(TEXT("stage index at 255"), Materials.Get(Concrete).GetStageIndex(255), 3);

	return true;
}

// ===========================================================================
// Load
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralLoadTest,
	"MadFall.Structural.Load",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralLoadTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	// Wood carries 800 kg and weighs 250: a 5-high post overloads its base only.
	{
		FTestWorld World;
		for (int32 Z = 0; Z <= 4; ++Z) { World.Place(0, 0, Z, Wood); }

		const TArray<FMadStructuralFailureRecord> Failures = Solve(World, Materials, World.AllPositions());
		TestEqual(TEXT("one overloaded block"), Failures.Num(), 1);
		if (Failures.Num() == 1)
		{
			TestEqual(TEXT("it is the base"), Failures[0].Position, FIntVector(0, 0, 0));
			TestTrue(TEXT("reason is overload"), Failures[0].Reason == EMadStructuralFailure::Overloaded);
		}

		FMadStructuralSettings NoLoad;
		NoLoad.bCheckLoad = false;
		TestEqual(TEXT("load check off: nothing fails"), Solve(World, Materials, World.AllPositions(), NoLoad).Num(), 0);
	}

	// Stress near a block, for shading: nearest first, radius-bounded, capped.
	{
		FTestWorld World;
		for (int32 X = 0; X <= 6; ++X) { World.Place(X, 0, 0, Concrete); }
		for (int32 X = 1; X <= 6; ++X) { World.Place(0, 0, X, Concrete); }
		FMadStructuralJob Job(World.AllPositions(), FMadStructuralSettings());
		TArray<FMadStressSample> Samples;
		Job.CollectStressNear(FIntVector(0, 0, 0), 3, 100, Samples);
		TestEqual(TEXT("no samples before the solve is done"), Samples.Num(), 0);
		Job.RunToCompletion(World, Materials);
		Job.CollectStressNear(FIntVector(0, 0, 0), 3, 100, Samples);
		TestEqual(TEXT("members within 3: the corner, 3 along the floor, 3 up the pillar"), Samples.Num(), 7);
		if (Samples.Num() > 0)
		{
			TestEqual(TEXT("nearest first"), Samples[0].Position, FIntVector(0, 0, 0));
			FMadStructuralNodeReport Report;
			Job.GetNodeReport(Samples.Last().Position, Report);
			TestEqual(TEXT("a sample's stress is its report's"), Samples.Last().Stress, Report.Stress);
		}
		Job.CollectStressNear(FIntVector(0, 0, 0), 3, 2, Samples);
		TestEqual(TEXT("capped"), Samples.Num(), 2);
	}

	// Load splits evenly over equally close supports.
	{
		FTestWorld World;
		World.Place(0, 0, 0, Concrete);
		World.Place(4, 0, 0, Concrete);
		for (int32 X = 0; X <= 4; ++X) { World.Place(X, 0, 1, Concrete); }

		FMadStructuralJob Job(World.AllPositions(), FMadStructuralSettings());
		Job.RunToCompletion(World, Materials);

		FMadStructuralNodeReport Left, Right;
		TestTrue(TEXT("left base report"), Job.GetNodeReport(FIntVector(0, 0, 0), Left));
		TestTrue(TEXT("right base report"), Job.GetNodeReport(FIntVector(4, 0, 0), Right));
		TestEqual(TEXT("left base carries half the 5-block beam"), Left.CarriedKg, 4500.0f, 0.5f);
		TestEqual(TEXT("right base carries the other half"), Right.CarriedKg, 4500.0f, 0.5f);
	}

	return true;
}

// ===========================================================================
// Determinism and time slicing
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralDeterminismTest,
	"MadFall.Structural.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	// A random scaffold: columns, beams and damage, with plenty of failures.
	FTestWorld World;
	FRandomStream Random(0x5EED);
	for (int32 Index = 0; Index < 2500; ++Index)
	{
		const int32 X = Random.RandRange(0, 24);
		const int32 Y = Random.RandRange(0, 24);
		const int32 Z = Random.RandRange(0, 12);
		const uint16 Block = static_cast<uint16>(Random.RandRange(Concrete, Steel));
		World.Place(X, Y, Z, Block, static_cast<uint8>(Random.RandRange(0, 3) == 0 ? Random.RandRange(0, 255) : 0));
	}

	const TArray<FIntVector> Seeds = World.AllPositions();

	FMadStructuralJob Whole(Seeds, FMadStructuralSettings());
	Whole.RunToCompletion(World, Materials);

	FMadStructuralJob Sliced(Seeds, FMadStructuralSettings());
	int32 Steps = 0;
	while (!Sliced.Step(World, Materials, 7))
	{
		++Steps;
	}

	TestTrue(TEXT("the sliced job actually took many steps"), Steps > 100);
	TestTrue(TEXT("the scaffold produces failures"), Whole.GetFailures().Num() > 0);
	TestEqual(TEXT("same failure count"), Sliced.GetFailures().Num(), Whole.GetFailures().Num());

	const int32 Count = FMath::Min(Sliced.GetFailures().Num(), Whole.GetFailures().Num());
	int32 Mismatches = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FMadStructuralFailureRecord& A = Whole.GetFailures()[Index];
		const FMadStructuralFailureRecord& B = Sliced.GetFailures()[Index];
		if (A.Position != B.Position || A.Reason != B.Reason)
		{
			++Mismatches;
		}
	}
	TestEqual(TEXT("identical failures in identical order"), Mismatches, 0);

	AddInfo(FString::Printf(TEXT("Scaffold: %d members, %d failures, %d slices."),
		Whole.NumMembers(), Whole.GetFailures().Num(), Steps));
	return true;
}

// ===========================================================================
// Performance
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralChunkSeedsTest,
	"MadFall.Structural.ChunkSeeds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralChunkSeedsTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();
	const FMadChunkCoord Coord(-1, 2, 0);

	auto Make = [](uint16 Block, bool bCubic)
	{
		FMadVoxel Voxel = FTestWorld::MakeAir();
		Voxel.BlockTypeID = Block;
		Voxel.Density = 255;
		Voxel.SetFlag(EMadVoxelFlags::Cubic, bCubic);
		return Voxel;
	};

	TArray<FIntVector> Members;

	// All air, and all natural stone: nothing, via the early outs.
	{
		FMadChunkStorage Air;
		MadFall::Structural::FindMembersInChunk(Air, Coord, Materials, Members);
		TestEqual(TEXT("an air chunk has no members"), Members.Num(), 0);

		FMadChunkStorage Terrain;
		Terrain.Fill(Make(Stone, false));
		MadFall::Structural::FindMembersInChunk(Terrain, Coord, Materials, Members);
		TestEqual(TEXT("a terrain chunk has no members"), Members.Num(), 0);
	}

	// Concrete that is natural (not cubic) is terrain-like: not a member.
	{
		FMadChunkStorage Storage;
		Storage.SetVoxel(1, 1, 1, Make(Concrete, false));
		MadFall::Structural::FindMembersInChunk(Storage, Coord, Materials, Members);
		TestEqual(TEXT("non-cubic concrete is not a member"), Members.Num(), 0);
	}

	// A small building: three cubic members, a cubic anchor-material block, and terrain.
	{
		FMadChunkStorage Storage;
		Storage.Fill(Make(Stone, false));
		Storage.SetVoxel(4, 5, 6, Make(Concrete, true));
		Storage.SetVoxel(4, 5, 7, Make(Wood, true));
		Storage.SetVoxel(31, 31, 31, Make(Steel, true));
		Storage.SetVoxel(0, 0, 0, Make(Stone, true));   // cubic, but an anchor material

		MadFall::Structural::FindMembersInChunk(Storage, Coord, Materials, Members);
		TestEqual(TEXT("three members"), Members.Num(), 3);

		// Chunk (-1, 2, 0) starts at world (-32, 64, 0).
		TestTrue(TEXT("world position of the first"), Members.Contains(FIntVector(-32 + 4, 64 + 5, 6)));
		TestTrue(TEXT("world position of the corner voxel"), Members.Contains(FIntVector(-32 + 31, 64 + 31, 31)));
		TestFalse(TEXT("anchor material excluded"), Members.Contains(FIntVector(-32, 64, 0)));
	}

	// Cost of the full scan on a chunk that does contain construction (the
	// early outs make the terrain case trivially cheap).
	{
		FMadChunkStorage Storage;
		for (int32 Z = 0; Z < 8; ++Z)
		{
			for (int32 X = 0; X < 32; ++X)
			{
				Storage.SetVoxel(X, 16, Z, Make(Concrete, true));
			}
		}
		const double Start = FPlatformTime::Seconds();
		for (int32 Repeat = 0; Repeat < 10; ++Repeat)
		{
			MadFall::Structural::FindMembersInChunk(Storage, Coord, Materials, Members);
		}
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0 / 10.0;
		TestEqual(TEXT("a wall of 256 members"), Members.Num(), 256);
		AddInfo(FString::Printf(TEXT("FindMembersInChunk on a chunk with construction: %.3f ms"), Ms));
		TestTrue(*FString::Printf(TEXT("scan fits well inside the 2 ms frame rule (%.3f ms)"), Ms), Ms < 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralPerformanceTest,
	"MadFall.Structural.Performance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralPerformanceTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	// A 24x24x24 concrete lattice (columns every 4, full floors every 4):
	// ~4.1k members, about a large player base.
	FTestWorld World;
	for (int32 Z = 0; Z < 24; ++Z)
	{
		for (int32 Y = 0; Y < 24; ++Y)
		{
			for (int32 X = 0; X < 24; ++X)
			{
				const bool bColumn = (X % 4 == 0) && (Y % 4 == 0);
				const bool bFloor = (Z % 4 == 3);
				if (bColumn || bFloor)
				{
					World.Place(X, Y, Z, Concrete);
				}
			}
		}
	}

	// Worst case for one edit: seeded at a single block, the whole lattice gathers.
	const double Start = FPlatformTime::Seconds();
	FMadStructuralJob Job(SeedsAround(0, 0, 0), FMadStructuralSettings());

	double WorstSliceMs = 0.0;
	bool bDone = false;
	while (!bDone)
	{
		const double SliceStart = FPlatformTime::Seconds();
		bDone = Job.Step(World, Materials, 256);
		WorstSliceMs = FMath::Max(WorstSliceMs, (FPlatformTime::Seconds() - SliceStart) * 1000.0);
	}
	const double TotalMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	TestEqual(TEXT("the lattice stands"), Job.GetFailures().Num(), 0);
	TestTrue(TEXT("the whole lattice was gathered"), Job.NumMembers() > 4000);

	// Generous ceilings - shared CI machines are noisy - but a regression that
	// makes a single slice blow the frame budget should fail loudly.
	TestTrue(FString::Printf(TEXT("worst 256-unit slice %.3f ms is under 1 ms"), WorstSliceMs), WorstSliceMs < 1.0);
	TestTrue(FString::Printf(TEXT("full solve %.1f ms is under 100 ms"), TotalMs), TotalMs < 100.0);

	AddInfo(FString::Printf(TEXT("Lattice: %d members solved in %.2f ms total, worst slice %.3f ms (%.1f us/member)."),
		Job.NumMembers(), TotalMs, WorstSliceMs, TotalMs * 1000.0 / FMath::Max(1, Job.NumMembers())));
	return true;
}

// ===========================================================================
// Shipped content
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStructuralShippedPrefabsTest,
	"MadFall.Structural.ShippedPrefabsStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStructuralShippedPrefabsTest::RunTest(const FString& Parameters)
{
	using namespace MadStructuralTests;

	// The real registries and the real prefab files. A POI that collapses the
	// moment a player walks up and nudges it is a content bug this catches in
	// CI instead of in a playtest.
	const FMadBlockRegistry& Registry = UMadVoxelWorldSubsystem::GetBlockRegistry();
	const FMadPrefabRegistry& Prefabs = UMadVoxelWorldSubsystem::GetPrefabRegistry();

	FMadStructuralMaterials Materials;
	Materials.Build(Registry);

	TestTrue(TEXT("shipped prefabs are loaded"), Prefabs.Num() >= 4);

	// The ground a POI actually gets. The planner sets the base at the median
	// surface height and fills foundation (its placement block, cubic, so it
	// is structural) down to the ground under every column the bottom layer
	// stands on. Flat ground alone never loads a foundation at all - the
	// watchtower passed it while shipping a wood-frame foundation that its own
	// posts overloaded on any slope.
	struct FGround
	{
		const TCHAR* Name;
		TFunction<int32(const FMadPrefab&, int32 X, int32 Y)> FoundationDepth;
	};
	const FGround Grounds[] =
	{
		{ TEXT("flat ground"), [](const FMadPrefab&, int32, int32) { return 0; } },
		{ TEXT("a slope along X at max_slope"), [](const FMadPrefab& P, int32 X, int32)
			{ return FMath::Min(P.Placement.MaxFoundationDepth, P.Placement.MaxSlope * X / FMath::Max(1, P.Size.X - 1)); } },
		{ TEXT("a slope along Y at max_slope"), [](const FMadPrefab& P, int32, int32 Y)
			{ return FMath::Min(P.Placement.MaxFoundationDepth, P.Placement.MaxSlope * Y / FMath::Max(1, P.Size.Y - 1)); } },
		{ TEXT("stilts at max_foundation_depth (a cave under it)"), [](const FMadPrefab& P, int32, int32)
			{ return P.Placement.MaxFoundationDepth; } },
	};

	for (const FMadPrefab& Prefab : Prefabs.GetAll())
	{
		const uint16 FoundationId = Registry.ResolveRuntimeId(Prefab.Placement.FoundationBlock);
		const uint16 GroundId = Registry.ResolveRuntimeId(FName(TEXT("madfall:stone")));

		for (const FGround& Ground : Grounds)
		{
			FTestWorld World;
			const int32 BaseZ = Prefab.Placement.MaxFoundationDepth;

			for (int32 Y = 0; Y < Prefab.Size.Y; ++Y)
			{
				for (int32 X = 0; X < Prefab.Size.X; ++X)
				{
					for (int32 Z = 0; Z < Prefab.Size.Z; ++Z)
					{
						const FMadPrefabPaletteEntry& Entry = Prefab.GetEntry(X, Y, Z);
						if (Entry.bVoid || Entry.IsAir())
						{
							continue;
						}

						FMadVoxel Voxel = FTestWorld::MakeAir();
						Voxel.BlockTypeID = Registry.ResolveRuntimeId(Entry.Block);
						Voxel.Density = Entry.Density;
						Voxel.SetFlag(EMadVoxelFlags::Cubic, Entry.bCubic);
						World.Voxels.Add(FIntVector(X, Y, BaseZ + Z), Voxel);
					}

					// Natural ground up to the foundation, then the foundation, under
					// columns the building stands on - the planner's rule.
					const FMadPrefabPaletteEntry& Bottom = Prefab.GetEntry(X, Y, 0);
					const bool bStandsHere = !Bottom.bVoid && !Bottom.IsAir() && Bottom.Density >= 128;
					const int32 Depth = bStandsHere ? Ground.FoundationDepth(Prefab, X, Y) : 0;
					for (int32 Z = 0; Z < BaseZ; ++Z)
					{
						const bool bFoundation = Z >= BaseZ - Depth;
						if (!bFoundation && !bStandsHere)
						{
							continue;   // open air beside the plinths
						}
						FMadVoxel Voxel = FTestWorld::MakeAir();
						Voxel.BlockTypeID = bFoundation ? FoundationId : GroundId;
						Voxel.Density = 255;
						Voxel.SetFlag(EMadVoxelFlags::Cubic, bFoundation);
						World.Voxels.Add(FIntVector(X, Y, Z), Voxel);
					}
				}
			}

			FMadStructuralJob Job(World.AllPositions(), FMadStructuralSettings());
			Job.RunToCompletion(World, Materials);

			const TArray<FMadStructuralFailureRecord>& Failures = Job.GetFailures();
			TestEqual(FString::Printf(TEXT("%s stands on %s"), *Prefab.Id.ToString(), Ground.Name), Failures.Num(), 0);

			for (int32 Index = 0; Index < FMath::Min(Failures.Num(), 8); ++Index)
			{
				const FMadStructuralFailureRecord& F = Failures[Index];
				FMadStructuralNodeReport Report;
				Job.GetNodeReport(F.Position, Report);
				AddError(FString::Printf(TEXT("  %s on %s, %s at %s: %s, %s, carrying %.0f of %.0f kg"),
					*Prefab.Id.ToString(), Ground.Name, *Registry.GetStringId(F.Voxel.BlockTypeID).ToString(), *F.Position.ToString(),
					F.Reason == EMadStructuralFailure::Overloaded ? TEXT("overloaded") : TEXT("unsupported"),
					Report.SupportDistance == MadFall::Structural::Unreachable ? TEXT("no path")
						: *FString::Printf(TEXT("%.2f span"), static_cast<double>(Report.SupportDistance) / MadFall::Structural::SupportScale),
					Report.CarriedKg, Report.CapacityKg));
			}

			AddInfo(FString::Printf(TEXT("%s on %s: %d structural members, %d failures."),
				*Prefab.Id.ToString(), Ground.Name, Job.NumMembers(), Failures.Num()));
		}
	}

	return true;
}

// ===========================================================================
// Block damage
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadBlockDamageTest,
	"MadFall.Structural.BlockDamage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadBlockDamageTest::RunTest(const FString& Parameters)
{
	FMadBlockRegistry Registry;
	TArray<FMadDefinitionError> Errors;
	Registry.BeginLoad();

	FMadBlockDefinitionData Frame;
	Frame.Id = FName(TEXT("test:frame"));
	Frame.SourceModId = FName(TEXT("test"));
	Frame.Hardness = 50.0f;
	Registry.AddFromAsset(Frame, Errors);

	FMadBlockDefinitionData Wall;
	Wall.Id = FName(TEXT("test:wall"));
	Wall.SourceModId = FName(TEXT("test"));
	Wall.Hardness = 100.0f;
	Wall.Resistances.Add(FName(TEXT("test:fire")), 2.0f);
	Wall.Resistances.Add(FName(TEXT("test:magic")), 0.0f);
	{
		FMadBlockDamageStage Intact;
		Intact.At = 0;
		FMadBlockDamageStage Cracked;
		Cracked.At = 128;
		Cracked.SupportMultiplier = 0.5f;
		FMadBlockDamageStage Gone;
		Gone.At = 255;
		Gone.DowngradeTo = Frame.Id;
		Wall.DamageStages = { Intact, Cracked, Gone };
	}
	Registry.AddFromAsset(Wall, Errors);

	Registry.FinishLoad(Errors);
	TestEqual(TEXT("test registry loads cleanly"), Errors.Num(), 0);

	const uint16 WallId = Registry.ResolveRuntimeId(Wall.Id);
	const uint16 FrameId = Registry.ResolveRuntimeId(Frame.Id);

	FMadVoxel Voxel;
	Voxel.BlockTypeID = WallId;
	Voxel.Density = 255;
	Voxel.Damage = 0;
	Voxel.Rotation = 0;
	Voxel.Flags = static_cast<uint8>(EMadVoxelFlags::Cubic);

	const FName Blunt(TEXT("test:blunt"));

	// 10 of 100 HP: byte rounds up to 26, same stage.
	FMadBlockDamageResult R = MadFall::BlockDamage::Compute(Voxel, 10.0f, Blunt, Registry);
	TestEqual(TEXT("10% damage -> byte 26"), static_cast<int32>(R.NewVoxel.Damage), 26);
	TestFalse(TEXT("no stage change yet"), R.bStageChanged);
	TestEqual(TEXT("effective damage"), R.EffectiveDamage, 10.0f, 0.01f);

	// 41 more crosses 128.
	R = MadFall::BlockDamage::Compute(R.NewVoxel, 41.0f, Blunt, Registry);
	TestTrue(TEXT("crossing 128 changes stage"), R.bStageChanged);
	TestFalse(TEXT("not downgraded"), R.bDowngraded);

	// Immunity.
	FMadBlockDamageResult Immune = MadFall::BlockDamage::Compute(Voxel, 1000.0f, FName(TEXT("test:magic")), Registry);
	TestEqual(TEXT("resistance 0 is immune"), static_cast<int32>(Immune.NewVoxel.Damage), 0);
	TestEqual(TEXT("immune: same block"), Immune.NewVoxel.BlockTypeID, WallId);

	// Fire x2 against an intact wall: 60 raw = 120 effective. The wall (100 HP)
	// downgrades; 20 effective overflow is 10 raw, x1 against the frame (50 HP).
	FMadBlockDamageResult Fire = MadFall::BlockDamage::Compute(Voxel, 60.0f, FName(TEXT("test:fire")), Registry);
	TestTrue(TEXT("downgraded"), Fire.bDowngraded);
	TestFalse(TEXT("not destroyed"), Fire.bDestroyed);
	TestEqual(TEXT("now the frame"), Fire.NewVoxel.BlockTypeID, FrameId);
	TestEqual(TEXT("overflow lands on the frame: 10/50 -> byte 51"), static_cast<int32>(Fire.NewVoxel.Damage), 51);

	// Enough to go through both.
	FMadBlockDamageResult Through = MadFall::BlockDamage::Compute(Voxel, 500.0f, Blunt, Registry);
	TestTrue(TEXT("downgraded on the way"), Through.bDowngraded);
	TestTrue(TEXT("and destroyed"), Through.bDestroyed);
	TestTrue(TEXT("leaves air"), Through.NewVoxel.IsAir());
	TestEqual(TEXT("effective damage is capped at the HP that existed"), Through.EffectiveDamage, 150.0f, 0.01f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

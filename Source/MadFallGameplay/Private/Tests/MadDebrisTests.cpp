// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadDebris.h"
#include "MadGameplayDefinitions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadDebrisTests
{
	constexpr uint16 Concrete = 2;

	FMadStructuralMaterials MakeMaterials()
	{
		FMadStructuralMaterials Materials;
		FMadStructuralMaterial ConcreteMat;
		ConcreteMat.bKnown = true;
		ConcreteMat.MassKg = 1000.0f;
		ConcreteMat.SupportStrength = 1.0e7f;
		ConcreteMat.MaxHorizontalSpan = 6;
		Materials.Set(Concrete, ConcreteMat);
		return Materials;
	}

	FMadStructuralFailureRecord Failure(int32 X, int32 Y, int32 Z)
	{
		FMadStructuralFailureRecord Record;
		Record.Position = FIntVector(X, Y, Z);
		Record.Voxel.BlockTypeID = Concrete;
		Record.Voxel.Density = 255;
		Record.Voxel.Damage = 0;
		Record.Voxel.Rotation = 0;
		Record.Voxel.Flags = static_cast<uint8>(EMadVoxelFlags::Cubic);
		Record.Reason = EMadStructuralFailure::Unsupported;
		return Record;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadDebrisTest,
	"MadFall.Structural.Debris",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadDebrisTest::RunTest(const FString& Parameters)
{
	using namespace MadDebrisTests;
	const FMadStructuralMaterials Materials = MakeMaterials();

	// --- clustering ---------------------------------------------------------
	{
		const TArray<FMadStructuralFailureRecord> Failures =
		{
			Failure(0, 0, 10), Failure(1, 0, 10), Failure(1, 0, 11),   // an L
			Failure(5, 0, 10),                                         // a lone block
			Failure(2, 0, 11)                                          // joins the L via (1,0,11)
		};

		const TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters(Failures, Materials);
		TestEqual(TEXT("two clusters"), Clusters.Num(), 2);
		if (Clusters.Num() == 2)
		{
			TestEqual(TEXT("first cluster is the L plus its extension"), Clusters[0].Blocks.Num(), 4);
			TestEqual(TEXT("second cluster is the lone block"), Clusters[1].Blocks.Num(), 1);
			TestEqual(TEXT("cluster mass sums its blocks"), Clusters[0].MassKg, 4000.0f, 0.01f);

			// (1,0,11) sits on (1,0,10) so it is not a bottom block; (2,0,11) overhangs, so it is.
			TestEqual(TEXT("three bottom faces in the L"), Clusters[0].BottomBlocks.Num(), 3);
		}
	}

	// Ground: everything below z = 0 is solid.
	auto FlatGround = [](const FIntVector& P) { return P.Z >= 0; };

	// --- a free fall lands on the ground at the right speed -----------------
	{
		TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters({ Failure(0, 0, 10) }, Materials);
		FMadDebrisCluster& Cluster = Clusters[0];
		MadFall::Debris::AdvanceToLanding(Cluster, FlatGround);

		TestTrue(TEXT("landed"), Cluster.bLanded);
		TestEqual(TEXT("fell 10 voxels onto the ground"), Cluster.Dropped, 10);
		TestEqual(TEXT("landed position is z = 0"), Cluster.GetLandedPosition(0), FIntVector(0, 0, 0));

		const float Analytic = FMath::Sqrt(2.0f * MadFall::Debris::Gravity * 10.0f);
		TestEqual(TEXT("landing speed is close to sqrt(2gh)"), Cluster.Velocity, Analytic, 0.35f);

		TArray<FMadDebrisImpact> Impacts;
		MadFall::Debris::ComputeImpacts(Cluster, FlatGround, Impacts);
		TestEqual(TEXT("one contact"), Impacts.Num(), 1);
		if (Impacts.Num() == 1)
		{
			TestEqual(TEXT("contact is the ground under it"), Impacts[0].Position, FIntVector(0, 0, -1));
			TestEqual(TEXT("energy is 1/2 m v^2"), Impacts[0].EnergyJ,
				0.5f * 1000.0f * Cluster.Velocity * Cluster.Velocity, 1.0f);
		}
	}

	// --- a tall cluster does not land on itself ------------------------------
	{
		TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters(
			{ Failure(0, 0, 5), Failure(0, 0, 6), Failure(0, 0, 7) }, Materials);
		MadFall::Debris::AdvanceToLanding(Clusters[0], FlatGround);
		TestEqual(TEXT("stack falls until its bottom is on the ground"), Clusters[0].Dropped, 5);
	}

	// --- a beam lands on a pillar under one end -------------------------------
	{
		auto Pillar = [](const FIntVector& P)
		{
			const bool bPillar = P.X == 2 && P.Y == 0 && P.Z >= 0 && P.Z <= 2;
			return P.Z >= 0 && !bPillar;
		};

		TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters(
			{ Failure(0, 0, 8), Failure(1, 0, 8), Failure(2, 0, 8) }, Materials);
		FMadDebrisCluster& Beam = Clusters[0];
		MadFall::Debris::AdvanceToLanding(Beam, Pillar);

		TestEqual(TEXT("beam stops on the pillar top"), Beam.Dropped, 5);

		TArray<FMadDebrisImpact> Impacts;
		MadFall::Debris::ComputeImpacts(Beam, Pillar, Impacts);
		TestEqual(TEXT("only the pillar is hit"), Impacts.Num(), 1);
		if (Impacts.Num() == 1)
		{
			TestEqual(TEXT("the pillar top takes it"), Impacts[0].Position, FIntVector(2, 0, 2));
			TestEqual(TEXT("and takes all of the beam's energy"), Impacts[0].EnergyJ,
				0.5f * 3000.0f * Beam.Velocity * Beam.Velocity, 1.0f);
		}
	}

	// --- blocked from the start: no fall, no impact ---------------------------
	{
		TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters({ Failure(0, 0, 0) }, Materials);
		const bool bLanded = MadFall::Debris::Advance(Clusters[0], 1.0f / 60.0f, FlatGround);
		TestTrue(TEXT("a block resting on the ground lands immediately"), bLanded);

		TArray<FMadDebrisImpact> Impacts;
		MadFall::Debris::ComputeImpacts(Clusters[0], FlatGround, Impacts);
		TestEqual(TEXT("crushed in place deals no impact damage"), Impacts.Num(), 0);
	}

	// --- rubble compacts per column and is deterministic ------------------------
	{
		TArray<FMadStructuralFailureRecord> Wall;
		for (int32 Z = 10; Z < 20; ++Z)
		{
			for (int32 X = 0; X < 6; ++X)
			{
				Wall.Add(Failure(X, 0, Z));
			}
		}

		TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters(Wall, Materials);
		MadFall::Debris::AdvanceToLanding(Clusters[0], FlatGround);

		TArray<TPair<FIntVector, int32>> RubbleA, RubbleB, RubbleAll;
		MadFall::Debris::ComputeRubble(Clusters[0], 3, RubbleA);
		MadFall::Debris::ComputeRubble(Clusters[0], 3, RubbleB);
		MadFall::Debris::ComputeRubble(Clusters[0], 1, RubbleAll);

		TestEqual(TEXT("keep-one-in-1 keeps everything"), RubbleAll.Num(), 60);
		TestTrue(TEXT("keep-one-in-3 keeps some but not all"), RubbleA.Num() > 5 && RubbleA.Num() < 45);
		TestTrue(TEXT("rubble is deterministic"), RubbleA == RubbleB);

		bool bCompacted = true;
		TMap<int32, int32> CountPerColumn;
		for (const TPair<FIntVector, int32>& Entry : RubbleA)
		{
			const int32 Seen = CountPerColumn.FindOrAdd(Entry.Key.X)++;
			bCompacted &= (Entry.Key.Z == Seen);
		}
		TestTrue(TEXT("each column's rubble stacks up from the landing surface"), bCompacted);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadDebrisPawnsAndDropsTest,
	"MadFall.Structural.DebrisPawnsAndDrops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadDebrisPawnsAndDropsTest::RunTest(const FString& Parameters)
{
	using namespace MadDebrisTests;
	const FMadStructuralMaterials Materials = MakeMaterials();
	auto FlatGround = [](const FIntVector& P) { return P.Z >= 0; };

	// --- the space a pawn stands in ------------------------------------------------------
	{
		// A survivor-sized capsule (radius 34, half height 88) centred in voxel column (2, 3), feet on z = 5.
		TArray<FIntVector> Voxels;
		MadFall::Debris::GetPawnVoxels(FVector(250.0, 350.0, 500.0 + 88.0), 88.0f, 34.0f, Voxels);
		TestEqual(TEXT("a centred survivor is one column, two voxels tall"), Voxels.Num(), 2);
		TestTrue(TEXT("feet voxel"), Voxels.Contains(FIntVector(2, 3, 5)));
		TestTrue(TEXT("head voxel"), Voxels.Contains(FIntVector(2, 3, 6)));

		// Straddling a voxel boundary in X: both columns.
		Voxels.Reset();
		MadFall::Debris::GetPawnVoxels(FVector(300.0, 350.0, 588.0), 88.0f, 34.0f, Voxels);
		TestEqual(TEXT("straddling a boundary claims both columns"), Voxels.Num(), 4);

		// Brushing a boundary by 2 cm does not claim the next voxel over.
		Voxels.Reset();
		MadFall::Debris::GetPawnVoxels(FVector(268.0, 350.0, 588.0), 88.0f, 34.0f, Voxels);
		TestEqual(TEXT("brushing a wall does not claim it"), Voxels.Num(), 2);

		// Half a voxel of skin: the neighbouring columns and the voxel above the head, never below the feet.
		Voxels.Reset();
		MadFall::Debris::GetPawnVoxels(FVector(250.0, 350.0, 588.0), 88.0f, 34.0f, Voxels, 50.0f);
		TestEqual(TEXT("with skin, a 3x3 footprint three voxels tall"), Voxels.Num(), 27);
		TestFalse(TEXT("nothing below the feet"), Voxels.Contains(FIntVector(2, 3, 4)));
		TestTrue(TEXT("the voxel above the head"), Voxels.Contains(FIntVector(2, 3, 7)));
	}

	// --- swept hits ---------------------------------------------------------------
	{
		// A 3-high column at x=0 starting at z=10, falling onto a 2-voxel-tall pawn at (0,0,0).
		TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters({ Failure(0, 0, 10), Failure(0, 0, 11), Failure(0, 0, 12) }, Materials);
		FMadDebrisCluster& Column = Clusters[0];
		const FIntVector Pawn(0, 0, 0);
		const FIntVector Beside(1, 0, 0);

		// Fall in coarse ticks and sum what the pawn would receive, once.
		float Energy = 0.0f;
		float BesideEnergy = 0.0f;
		int32 Hits = 0;
		for (int32 Tick = 0; Tick < 400 && !Column.bLanded; ++Tick)
		{
			const int32 Before = Column.Dropped;
			MadFall::Debris::Advance(Column, 1.0f / 20.0f, FlatGround);
			if (Hits == 0)
			{
				const float E = MadFall::Debris::ComputeSweptHitEnergy(Column, Before, Pawn, 2);
				if (E > 0.0f)
				{
					Energy = E;
					++Hits;
				}
			}
			BesideEnergy += MadFall::Debris::ComputeSweptHitEnergy(Column, Before, Beside, 2);
		}

		TestEqual(TEXT("the pawn under the column is hit"), Hits, 1);
		TestEqual(TEXT("a pawn in the next column is not"), BesideEnergy, 0.0f);

		// Entering the pawn's head voxel (z=1) means the bottom fell 9 m: v^2 = 2 g 9,
		// with the whole 3000 kg column behind it. Tick granularity makes it approximate.
		const float Expected = 0.5f * 3000.0f * 2.0f * MadFall::Debris::Gravity * 9.0f;
		TestTrue(*FString::Printf(TEXT("hit energy ~ column mass at head-height speed (%.0f J vs %.0f J)"), Energy, Expected),
			Energy > Expected * 0.85f && Energy < Expected * 1.3f);

		// A cluster that has not moved since last check hits nothing.
		TestEqual(TEXT("no movement, no hit"), MadFall::Debris::ComputeSweptHitEnergy(Column, Column.Dropped, Pawn, 2), 0.0f);

		// One huge step through the pawn still registers (no tunnelling).
		TArray<FMadDebrisCluster> Fast = MadFall::Debris::BuildClusters({ Failure(0, 0, 10) }, Materials);
		Fast[0].Dropped = 10;
		Fast[0].Velocity = 14.0f;
		TestTrue(TEXT("a ten-voxel step through the pawn is a hit"), MadFall::Debris::ComputeSweptHitEnergy(Fast[0], 0, Pawn, 2) > 0.0f);
		TestEqual(TEXT("a pawn above where the block started is never hit"), MadFall::Debris::ComputeSweptHitEnergy(Fast[0], 0, FIntVector(0, 0, 11), 2), 0.0f);
	}

	// --- collapse drops -------------------------------------------------------------
	{
		auto Json = [](const TCHAR* Text)
		{
			TSharedPtr<FJsonObject> Object;
			FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Object);
			check(Object.IsValid());
			return Object.ToSharedRef();
		};

		const FName Mod(TEXT("test"));
		FMadGameplayDefinitions Defs;
		TArray<FMadDefinitionError> Errors;
		Defs.BeginLoad();
		Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"test:rock","max_stack":10})")), TEXT("i.json"), Mod, Errors);
		Defs.AddLootJson(Json(TEXT(R"({"schema":"madfall.loot/1","id":"test:loot/rubble","entries":[{"item":"test:rock","count":[1,1]}]})")), TEXT("l.json"), Mod, Errors);
		Defs.FinishLoad(nullptr, Errors);
		TestEqual(TEXT("drop definitions load"), Errors.Num(), 0);
		const FMadLootTableDefinition* Table = Defs.FindLootTable(FName(TEXT("test:loot/rubble")));

		TArray<FMadStructuralFailureRecord> Wall;
		for (int32 X = 0; X < 24; ++X)
		{
			Wall.Add(Failure(X, 0, 5));
		}
		TArray<FMadDebrisCluster> Clusters = MadFall::Debris::BuildClusters(Wall, Materials);
		MadFall::Debris::AdvanceToLanding(Clusters[0], FlatGround);

		TArray<TPair<FIntVector, int32>> Rubble;
		MadFall::Debris::ComputeRubble(Clusters[0], 3, Rubble);

		auto TableOf = [Table](uint16 Id) -> const FMadLootTableDefinition* { return Id == Concrete ? Table : nullptr; };
		FRandomStream RandomA(7);
		FRandomStream RandomB(7);
		TArray<FMadItemStack> DropsA, DropsB;
		MadFall::Debris::RollCollapseDrops(Clusters[0], Rubble, TableOf, Defs, RandomA, DropsA);
		MadFall::Debris::RollCollapseDrops(Clusters[0], Rubble, TableOf, Defs, RandomB, DropsB);

		int32 Rocks = 0;
		for (const FMadItemStack& Stack : DropsA)
		{
			Rocks += Stack.Count;
			TestTrue(TEXT("stacks respect max_stack"), Stack.Count <= 10);
		}
		TestEqual(TEXT("one rock per block that did NOT become rubble"), Rocks, 24 - Rubble.Num());
		TestEqual(TEXT("merged into full stacks"), DropsA.Num(), FMath::DivideAndRoundUp(24 - Rubble.Num(), 10));
		TestEqual(TEXT("same seed, same drops"), DropsA.Num(), DropsB.Num());

		TArray<FMadItemStack> None;
		MadFall::Debris::RollCollapseDrops(Clusters[0], Rubble, [](uint16) -> const FMadLootTableDefinition* { return nullptr; }, Defs, RandomA, None);
		TestEqual(TEXT("blocks without a collapse table drop nothing"), None.Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

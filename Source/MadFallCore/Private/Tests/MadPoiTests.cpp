// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadOrientation.h"
#include "MadPrefab.h"
#include "MadPrefabRegistry.h"
#include "MadWorldGenerator.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadPoiTests
{
	/** The project's real blocks, biomes and prefabs. */
	struct FFixture
	{
		FMadBlockRegistry Blocks;
		FMadBiomeRegistry Biomes;
		FMadPrefabRegistry Prefabs;
		TArray<FMadDefinitionError> Errors;

		FFixture()
		{
			const FString Defs = FPaths::Combine(FPaths::ProjectDir(), TEXT("Definitions"));

			Blocks.BeginLoad();
			Blocks.AddFromDirectory(FPaths::Combine(Defs, TEXT("blocks")), FName(TEXT("madfall")), Errors);
			Blocks.FinishLoad(Errors);

			Biomes.BeginLoad();
			Biomes.AddFromDirectory(FPaths::Combine(Defs, TEXT("biomes")), FName(TEXT("madfall")), Errors);
			Biomes.FinishLoad(Errors);

			Prefabs.Reset();
			Prefabs.AddFromDirectory(FPaths::Combine(Defs, TEXT("prefabs")), FName(TEXT("madfall")), Errors);
			Prefabs.Finalize();
		}
	};

}

// ===========================================================================
// Orientation
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadOrientationTest,
	"MadFall.WorldGen.Orientation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadOrientationTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Orientation;

	// --- the 24 are distinct proper rotations ---
	for (int32 A = 0; A < Count; ++A)
	{
		const FIntMatrix3& M = GetMatrix(static_cast<uint8>(A));

		const int32 Det =
			M.M[0][0] * (M.M[1][1] * M.M[2][2] - M.M[1][2] * M.M[2][1]) -
			M.M[0][1] * (M.M[1][0] * M.M[2][2] - M.M[1][2] * M.M[2][0]) +
			M.M[0][2] * (M.M[1][0] * M.M[2][1] - M.M[1][1] * M.M[2][0]);

		// Determinant -1 would be a reflection: a mirrored staircase that cannot
		// exist physically and that no block mesh is authored for.
		TestEqual(FString::Printf(TEXT("orientation %d is a proper rotation (det +1)"), A), Det, 1);

		for (int32 B = A + 1; B < Count; ++B)
		{
			TestFalse(FString::Printf(TEXT("orientations %d and %d are distinct"), A, B),
				GetMatrix(static_cast<uint8>(A)) == GetMatrix(static_cast<uint8>(B)));
		}
	}

	// --- the up-face labels mean what they say ---
	const FIntVector Up(0, 0, 1);
	const FIntVector Expected[6] =
	{
		FIntVector(0, 0, 1), FIntVector(0, 0, -1),
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0)
	};

	for (int32 Face = 0; Face < 6; ++Face)
	{
		for (int32 Spin = 0; Spin < 4; ++Spin)
		{
			const FIntVector Result = GetMatrix(static_cast<uint8>(Face * 4 + Spin)).Transform(Up);
			TestTrue(FString::Printf(TEXT("up face %d spin %d points local +Z at (%d,%d,%d)"),
				Face, Spin, Expected[Face].X, Expected[Face].Y, Expected[Face].Z),
				Result == Expected[Face]);
		}
	}

	// --- yaw composition ---
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint8 O = static_cast<uint8>(Index);

		TestEqual(FString::Printf(TEXT("yaw 0 leaves orientation %d unchanged"), Index),
			static_cast<int32>(ApplyYaw(O, 0)), Index);

		uint8 Walked = O;
		for (int32 Step = 0; Step < 4; ++Step)
		{
			Walked = ApplyYaw(Walked, 1);
		}
		TestEqual(FString::Printf(TEXT("four quarter turns return orientation %d to itself"), Index),
			static_cast<int32>(Walked), Index);

		// The table must agree with actual matrix multiplication, not merely be
		// self-consistent.
		for (int32 Yaw = 0; Yaw < 4; ++Yaw)
		{
			const FIntMatrix3 Composed = MakeYaw(Yaw) * GetMatrix(O);
			TestEqual(FString::Printf(TEXT("ApplyYaw(%d, %d) matches the composed matrix"), Index, Yaw),
				static_cast<int32>(ApplyYaw(O, Yaw)), FindIndex(Composed));
		}
	}

	// --- footprint rotation ---
	constexpr int32 SizeX = 5;
	constexpr int32 SizeY = 3;

	for (int32 Yaw = 0; Yaw < 4; ++Yaw)
	{
		const FIntVector Rotated = RotateSize(FIntVector(SizeX, SizeY, 1), Yaw);
		TSet<FIntPoint> Seen;

		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				int32 RX, RY;
				RotateFootprint(X, Y, SizeX, SizeY, Yaw, RX, RY);

				TestTrue(TEXT("a rotated position stays inside the rotated footprint"),
					RX >= 0 && RY >= 0 && RX < Rotated.X && RY < Rotated.Y);
				TestFalse(TEXT("footprint rotation is one-to-one"), Seen.Contains(FIntPoint(RX, RY)));
				Seen.Add(FIntPoint(RX, RY));

				int32 BackX, BackY;
				UnrotateFootprint(RX, RY, SizeX, SizeY, Yaw, BackX, BackY);
				TestTrue(FString::Printf(TEXT("unrotate inverts rotate at (%d,%d) yaw %d"), X, Y, Yaw),
					BackX == X && BackY == Y);
			}
		}
	}

	// Footprint rotation must turn the SAME way as block yaw, or a building's
	// blocks would face 90 degrees against the building itself. A step of +1 in
	// local X must become the direction MakeYaw maps +X to.
	for (int32 Yaw = 0; Yaw < 4; ++Yaw)
	{
		int32 AX, AY, BX, BY;
		RotateFootprint(1, 1, SizeX, SizeY, Yaw, AX, AY);
		RotateFootprint(2, 1, SizeX, SizeY, Yaw, BX, BY);

		const FIntVector FootprintStep(BX - AX, BY - AY, 0);
		const FIntVector MatrixStep = MakeYaw(Yaw).Transform(FIntVector(1, 0, 0));

		TestTrue(FString::Printf(TEXT("footprint rotation and block yaw agree at yaw %d"), Yaw),
			FootprintStep == MatrixStep);
	}

	return true;
}

// ===========================================================================
// Prefab format
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadPrefabFormatTest,
	"MadFall.WorldGen.PrefabFormat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadPrefabFormatTest::RunTest(const FString& Parameters)
{
	MadPoiTests::FFixture Fixture;

	// --- the shipped prefabs load cleanly ---
	TestTrue(TEXT("at least four prefabs ship"), Fixture.Prefabs.Num() >= 4);

	int32 PrefabErrors = 0;
	for (const FMadDefinitionError& Error : Fixture.Errors)
	{
		if (Error.SourcePath.Contains(TEXT("prefabs")))
		{
			++PrefabErrors;
			AddError(Error.ToString());
		}
	}
	TestEqual(TEXT("the shipped prefabs have no validation errors"), PrefabErrors, 0);

	for (const FMadPrefab& Prefab : Fixture.Prefabs.GetAll())
	{
		TestEqual(FString::Printf(TEXT("%s voxel count matches its size"), *Prefab.Id.ToString()),
			Prefab.Voxels.Num(), Prefab.Size.X * Prefab.Size.Y * Prefab.Size.Z);
		TestTrue(FString::Printf(TEXT("%s has solid voxels"), *Prefab.Id.ToString()), Prefab.CountSolidVoxels() > 0);

		for (const FMadPrefabPaletteEntry& Entry : Prefab.Palette)
		{
			if (!Entry.bVoid)
			{
				TestTrue(FString::Printf(TEXT("%s uses registered block %s"), *Prefab.Id.ToString(), *Entry.Block.ToString()),
					Fixture.Blocks.IsRegistered(Entry.Block));
			}
		}
	}

	// --- write then parse is lossless ---
	if (Fixture.Prefabs.Num() > 0)
	{
		const FMadPrefab& Original = Fixture.Prefabs.Get(0);
		const FString Text = MadFall::PrefabJson::WriteText(Original);

		FMadPrefab Reloaded;
		TArray<FMadDefinitionError> Errors;
		TestTrue(TEXT("a written prefab parses back"),
			MadFall::PrefabJson::ParseText(Text, TEXT("roundtrip.json"), FName(TEXT("madfall")), Reloaded, Errors));

		TestTrue(TEXT("round trip keeps the size"), Reloaded.Size == Original.Size);
		TestTrue(TEXT("round trip keeps every voxel"), Reloaded.Voxels == Original.Voxels);
		TestEqual(TEXT("round trip keeps the palette"), Reloaded.Palette.Num(), Original.Palette.Num());
		TestEqual(TEXT("round trip keeps the markers"), Reloaded.Markers.Num(), Original.Markers.Num());
		TestEqual(TEXT("round trip keeps the tier"), Reloaded.Tier, Original.Tier);
		TestEqual(TEXT("round trip produces no errors"), Errors.Num(), 0);
	}

	// --- malformed prefabs are refused, not scrambled ---
	auto Expect = [this](const TCHAR* What, const FString& Json, bool bShouldLoad)
	{
		FMadPrefab Prefab;
		TArray<FMadDefinitionError> Errors;
		const bool bLoaded = MadFall::PrefabJson::ParseText(Json, TEXT("case.json"), FName(TEXT("test")), Prefab, Errors);
		TestEqual(What, bLoaded, bShouldLoad);
		return Prefab;
	};

	const FString Good = TEXT(R"({ "schema": "madfall.prefab/1", "id": "test:box", "size": [2, 2, 1],
		"palette": [ { "block": "*" }, { "block": "madfall:stone" } ], "voxels": [ 2, 0, 2, 1 ] })");
	const FMadPrefab Box = Expect(TEXT("a well-formed prefab loads"), Good, true);
	TestTrue(TEXT("the void token parses as void"), Box.Palette.Num() == 2 && Box.Palette[0].bVoid);

	Expect(TEXT("runs covering too few voxels are refused"),
		TEXT(R"({ "schema": "madfall.prefab/1", "id": "test:box", "size": [2, 2, 1],
		"palette": [ { "block": "madfall:stone" } ], "voxels": [ 3, 0 ] })"), false);

	Expect(TEXT("runs covering too many voxels are refused"),
		TEXT(R"({ "schema": "madfall.prefab/1", "id": "test:box", "size": [2, 2, 1],
		"palette": [ { "block": "madfall:stone" } ], "voxels": [ 5, 0 ] })"), false);

	Expect(TEXT("an out-of-range palette index is refused"),
		TEXT(R"({ "schema": "madfall.prefab/1", "id": "test:box", "size": [2, 2, 1],
		"palette": [ { "block": "madfall:stone" } ], "voxels": [ 4, 3 ] })"), false);

	Expect(TEXT("an oversized prefab is refused"),
		TEXT(R"({ "schema": "madfall.prefab/1", "id": "test:box", "size": [200, 2, 1],
		"palette": [ { "block": "madfall:stone" } ], "voxels": [ 400, 0 ] })"), false);

	const FMadPrefab OutOfBounds = Expect(TEXT("a prefab with a stray marker still loads"),
		TEXT(R"({ "schema": "madfall.prefab/1", "id": "test:box", "size": [2, 2, 1],
		"palette": [ { "block": "madfall:stone" } ], "voxels": [ 4, 0 ],
		"markers": [ { "type": "loot", "position": [9, 9, 9], "loot_table": "test:x" },
		             { "type": "loot", "position": [1, 1, 0], "loot_table": "test:y" } ] })"), true);
	TestEqual(TEXT("an out-of-bounds marker is dropped and an in-bounds one kept"), OutOfBounds.Markers.Num(), 1);

	return true;
}

// ===========================================================================
// Placement and stamping
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadPoiPlacementTest,
	"MadFall.WorldGen.PoiPlacement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadPoiPlacementTest::RunTest(const FString& Parameters)
{
	MadPoiTests::FFixture Fixture;
	if (!TestTrue(TEXT("prefabs loaded"), Fixture.Prefabs.Num() > 0))
	{
		return false;
	}

	FMadWorldGenSettings Settings;
	Settings.Seed = 20260912u;

	FMadWorldGenerator Generator(Settings, Fixture.Biomes, Fixture.Blocks, &Fixture.Prefabs);
	const FMadPoiPlanner& Planner = Generator.GetPoiPlanner();

	// --- determinism across generator instances ---
	{
		FMadWorldGenerator Other(Settings, Fixture.Biomes, Fixture.Blocks, &Fixture.Prefabs);

		int32 Compared = 0;
		int32 Mismatched = 0;

		for (int32 CellY = -4; CellY <= 4; ++CellY)
		{
			for (int32 CellX = -4; CellX <= 4; ++CellX)
			{
				FMadPoiInstance A, B;
				const bool bA = Planner.PlanCell(Generator, CellX, CellY, A);
				const bool bB = Other.GetPoiPlanner().PlanCell(Other, CellX, CellY, B);
				++Compared;

				if (bA != bB || (bA && (A.PrefabIndex != B.PrefabIndex || A.Origin != B.Origin || A.Yaw != B.Yaw)))
				{
					++Mismatched;
				}
			}
		}

		TestEqual(FString::Printf(TEXT("%d cells plan identically in two generators"), Compared), Mismatched, 0);
	}

	// --- POIs stay inside their cells, and markers inside their POIs ---
	{
		const int32 CellSize = Planner.GetCellSizeVoxels();
		int32 Found = 0;
		int32 Escaped = 0;
		int32 StrayMarkers = 0;
		int32 TierViolations = 0;

		for (int32 CellY = -6; CellY <= 6; ++CellY)
		{
			for (int32 CellX = -6; CellX <= 6; ++CellX)
			{
				FMadPoiInstance Poi;
				if (!Planner.PlanCell(Generator, CellX, CellY, Poi))
				{
					continue;
				}
				++Found;

				const int32 MinX = CellX * CellSize;
				const int32 MinY = CellY * CellSize;

				if (Poi.Origin.X < MinX || Poi.Origin.Y < MinY
					|| Poi.Origin.X + Poi.RotatedSize.X > MinX + CellSize
					|| Poi.Origin.Y + Poi.RotatedSize.Y > MinY + CellSize)
				{
					++Escaped;
				}

				if (Planner.GetPrefab(Poi)->Tier > Poi.CellTier)
				{
					++TierViolations;
				}

				TArray<FMadPoiWorldMarker> Markers;
				Planner.GetWorldMarkers(Poi, Markers);
				for (const FMadPoiWorldMarker& Marker : Markers)
				{
					const FIntVector Rel = Marker.WorldPosition - Poi.Origin;
					if (Rel.X < 0 || Rel.Y < 0 || Rel.Z < 0
						|| Rel.X >= Poi.RotatedSize.X || Rel.Y >= Poi.RotatedSize.Y || Rel.Z >= Poi.RotatedSize.Z)
					{
						++StrayMarkers;
					}
				}
			}
		}

		AddInfo(FString::Printf(TEXT("%d POIs in 169 cells"), Found));

		TestTrue(TEXT("POIs are actually placed"), Found > 10);

		// The whole cell-based design rests on this: a POI that leaks out of its
		// cell would be stamped by its own cell's chunks and missed by the
		// neighbour's, leaving half a building.
		TestEqual(TEXT("no POI extends outside its cell"), Escaped, 0);
		TestEqual(TEXT("no marker lies outside its POI"), StrayMarkers, 0);
		TestEqual(TEXT("no prefab appears in a cell below its tier"), TierViolations, 0);
	}

	// --- difficulty rises with distance ---
	TestEqual(TEXT("the origin cell is tier 1"), Planner.GetCellTier(0, 0), 1);
	TestTrue(TEXT("a distant cell is a higher tier"), Planner.GetCellTier(40, 40) > Planner.GetCellTier(0, 0));
	TestTrue(TEXT("tier never exceeds 5"), Planner.GetCellTier(10000, 10000) <= 5);

	// --- every POI survives being split across independently generated chunks ---
	// Checked for every POI in a 5x5 cell area rather than one: a single sample
	// happened to be a 7x7 watchtower spanning two chunks, which exercises
	// neither the larger prefabs nor all four yaws nor a building cut by a
	// chunk corner. The report lists what was actually covered.
	int32 PoisChecked = 0;
	int32 TotalChecked = 0;
	int32 WrongBlock = 0;
	int32 WrongOrientation = 0;
	int32 Floating = 0;
	int32 MaxChunksSpanned = 0;
	TSet<int32> YawsSeen;
	TSet<FName> PrefabsSeen;
	FString FirstFailure;

	// Three areas at increasing distance, because tier rises with distance and
	// the larger, higher-tier prefabs only appear far from spawn. Checking only
	// the origin verified the two tier-1 prefabs and nothing else.
	const FIntPoint AreaCentres[] = { FIntPoint(0, 0), FIntPoint(7, 0), FIntPoint(0, 15) };

	for (const FIntPoint& AreaCentre : AreaCentres)
	for (int32 CellY = AreaCentre.Y - 2; CellY <= AreaCentre.Y + 2; ++CellY)
	{
		for (int32 CellX = AreaCentre.X - 2; CellX <= AreaCentre.X + 2; ++CellX)
		{
			FMadPoiInstance Poi;
			if (!Planner.PlanCell(Generator, CellX, CellY, Poi))
			{
				continue;
			}

			const FMadPrefab* Prefab = Planner.GetPrefab(Poi);
			++PoisChecked;
			YawsSeen.Add(Poi.Yaw);
			PrefabsSeen.Add(Prefab->Id);

			// Generate every chunk the POI touches, each one independently.
			TMap<FMadChunkCoord, FMadChunkStorage> Chunks;
			FIntVector BoundsMin, BoundsMax;
			Poi.GetWorldBounds(Prefab->Placement.MaxFoundationDepth, BoundsMin, BoundsMax);

			const FMadChunkCoord ChunkMin = MadFall::WorldToChunk(BoundsMin.X, BoundsMin.Y, BoundsMin.Z);
			const FMadChunkCoord ChunkMax = MadFall::WorldToChunk(BoundsMax.X, BoundsMax.Y, BoundsMax.Z);

			for (int32 CZ = ChunkMin.Z; CZ <= ChunkMax.Z; ++CZ)
			{
				for (int32 CY = ChunkMin.Y; CY <= ChunkMax.Y; ++CY)
				{
					for (int32 CX = ChunkMin.X; CX <= ChunkMax.X; ++CX)
					{
						const FMadChunkCoord Coord(CX, CY, CZ);
						if (Coord.IsValidZ())
						{
							Generator.GenerateChunk(Coord, Chunks.Add(Coord));
						}
					}
				}
			}

			MaxChunksSpanned = FMath::Max(MaxChunksSpanned, Chunks.Num());

			auto ReadWorld = [&Chunks](int32 X, int32 Y, int32 Z) -> FMadVoxel
			{
				const FMadChunkStorage* Storage = Chunks.Find(MadFall::WorldToChunk(X, Y, Z));
				if (Storage == nullptr)
				{
					return FMadVoxel::Air();
				}
				int32 LX, LY, LZ;
				MadFall::WorldToLocal(X, Y, Z, LX, LY, LZ);
				return Storage->GetVoxel(LX, LY, LZ);
			};

			for (int32 LZ = 0; LZ < Prefab->Size.Z; ++LZ)
			{
				for (int32 LY = 0; LY < Prefab->Size.Y; ++LY)
				{
					for (int32 LX = 0; LX < Prefab->Size.X; ++LX)
					{
						const FMadPrefabPaletteEntry& Entry = Prefab->GetEntry(LX, LY, LZ);
						if (Entry.bVoid)
						{
							continue;
						}

						int32 RX, RY;
						MadFall::Orientation::RotateFootprint(LX, LY, Prefab->Size.X, Prefab->Size.Y, Poi.Yaw, RX, RY);

						const FMadVoxel Actual = ReadWorld(Poi.Origin.X + RX, Poi.Origin.Y + RY, Poi.Origin.Z + LZ);
						const uint16 ExpectedId = Entry.IsAir()
							? MadFall::BlockTypeAir : Fixture.Blocks.ResolveRuntimeId(Entry.Block);
						++TotalChecked;

						if (Actual.BlockTypeID != ExpectedId)
						{
							++WrongBlock;
							if (FirstFailure.IsEmpty())
							{
								FirstFailure = FString::Printf(TEXT("%s yaw %d local (%d,%d,%d): expected %s, found %s"),
									*Prefab->Id.ToString(), Poi.Yaw * 90, LX, LY, LZ, *Entry.Block.ToString(),
									*Fixture.Blocks.GetStringId(Actual.BlockTypeID).ToString());
							}
							continue;
						}

						if (!Entry.IsAir()
							&& Actual.GetOrientation() != MadFall::Orientation::ApplyYaw(Entry.Orientation, Poi.Yaw))
						{
							++WrongOrientation;
						}
					}
				}
			}

			// No floating corners: under every solid column of the bottom layer
			// there must be something solid - terrain, or foundation filled to it.
			for (int32 LY = 0; LY < Prefab->Size.Y; ++LY)
			{
				for (int32 LX = 0; LX < Prefab->Size.X; ++LX)
				{
					const FMadPrefabPaletteEntry& Bottom = Prefab->GetEntry(LX, LY, 0);
					if (Bottom.bVoid || Bottom.IsAir())
					{
						continue;
					}

					int32 RX, RY;
					MadFall::Orientation::RotateFootprint(LX, LY, Prefab->Size.X, Prefab->Size.Y, Poi.Yaw, RX, RY);

					if (!ReadWorld(Poi.Origin.X + RX, Poi.Origin.Y + RY, Poi.Origin.Z - 1).IsSolid())
					{
						++Floating;
					}
				}
			}
		}
	}

	TArray<FString> PrefabNames;
	for (const FName& Name : PrefabsSeen) { PrefabNames.Add(Name.ToString()); }
	PrefabNames.Sort();

	AddInfo(FString::Printf(
		TEXT("Stamp check: %d POIs, %d voxels, up to %d chunks per POI, yaws seen %d/4, prefabs: %s"),
		PoisChecked, TotalChecked, MaxChunksSpanned, YawsSeen.Num(), *FString::Join(PrefabNames, TEXT(", "))));

	TestTrue(TEXT("several POIs were checked"), PoisChecked >= 5);
	TestTrue(TEXT("at least one POI crossed a chunk corner (4+ chunks)"), MaxChunksSpanned >= 4);
	TestTrue(TEXT("all four yaws were exercised"), YawsSeen.Num() == 4);
	TestTrue(TEXT("at least three different prefabs were exercised"), PrefabsSeen.Num() >= 3);

	TestEqual(FString::Printf(
		TEXT("every non-void prefab voxel of every POI is at its rotated world position. %s"), *FirstFailure),
		WrongBlock, 0);
	TestEqual(TEXT("every block's orientation is rotated with its building"), WrongOrientation, 0);
	TestEqual(TEXT("no solid bottom column of any POI floats over empty space"), Floating, 0);

	return true;
}

// ===========================================================================
// Roads
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadRoadTest,
	"MadFall.WorldGen.Roads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadRoadTest::RunTest(const FString& Parameters)
{
	MadPoiTests::FFixture Fixture;

	FMadWorldGenSettings Settings;
	Settings.Seed = 20260912u;
	FMadWorldGenerator Generator(Settings, Fixture.Biomes, Fixture.Blocks, &Fixture.Prefabs);
	const FMadPoiPlanner& Planner = Generator.GetPoiPlanner();

	const uint16 RoadId = Fixture.Blocks.ResolveRuntimeId(FName(TEXT("madfall:gravel_path")));
	TestNotEqual(TEXT("the road block is registered"),
		static_cast<int32>(RoadId), static_cast<int32>(MadFall::BlockTypeUnresolved));

	// Find a road that some chunk near the origin reports.
	FMadRoadSegment Road;
	bool bFound = false;

	for (int32 CY = -24; CY <= 24 && !bFound; CY += 2)
	{
		for (int32 CX = -24; CX <= 24 && !bFound; CX += 2)
		{
			TArray<FMadRoadSegment> Roads;
			Planner.GetRoadsNearChunk(Generator, FMadChunkCoord(CX, CY, 0), Roads);
			if (Roads.Num() > 0)
			{
				Road = Roads[0];
				bFound = true;
			}
		}
	}

	if (!TestTrue(TEXT("at least one road connects two POIs near the origin"), bFound))
	{
		return false;
	}

	const float Length = FVector2f(
		static_cast<float>(Road.End.X - Road.Start.X), static_cast<float>(Road.End.Y - Road.Start.Y)).Size();
	AddInfo(FString::Printf(TEXT("Road from (%d,%d,%d) to (%d,%d,%d), %.0f voxels"),
		Road.Start.X, Road.Start.Y, Road.Start.Z, Road.End.X, Road.End.Y, Road.End.Z, Length));

	TestTrue(TEXT("the road respects the maximum length"), Length <= static_cast<float>(Settings.RoadMaxLength));

	// Sample the road's midpoint region: a road that is planned but never
	// stamped is exactly the bug this guards against.
	const int32 MidX = (Road.Start.X + Road.End.X) / 2;
	const int32 MidY = (Road.Start.Y + Road.End.Y) / 2;
	const int32 MidZ = FMath::FloorToInt(Generator.GetSurfaceHeight(static_cast<float>(MidX), static_cast<float>(MidY)));

	int32 RoadVoxels = 0;
	const FMadChunkCoord Centre = MadFall::WorldToChunk(MidX, MidY, MidZ);

	for (int32 DZ = -1; DZ <= 1; ++DZ)
	{
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const FMadChunkCoord Coord(Centre.X + DX, Centre.Y + DY, Centre.Z + DZ);
				if (!Coord.IsValidZ())
				{
					continue;
				}

				FMadChunkStorage Chunk;
				Generator.GenerateChunk(Coord, Chunk);

				for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
				{
					if (Chunk.GetBlockId(Index) == RoadId)
					{
						++RoadVoxels;
					}
				}
			}
		}
	}

	AddInfo(FString::Printf(TEXT("%d road voxels around the midpoint"), RoadVoxels));
	TestTrue(TEXT("the road is actually stamped into the chunks it crosses"), RoadVoxels > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadRoadBanksTest,
	"MadFall.WorldGen.RoadBanks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadRoadBanksTest::RunTest(const FString& Parameters)
{
	using MadFall::Roads::BankHeight;

	// The profile: road height at the edge, terrain past the bank, never steeper than 45 degrees within MaxBank.
	TestEqual(TEXT("on the road"), BankHeight(10.0f, 16.0f, -1.0f), 10.0f);
	TestEqual(TEXT("at the edge"), BankHeight(10.0f, 16.0f, 0.0f), 10.0f);
	TestEqual(TEXT("past a 6-voxel cutting's 9-voxel bank"), BankHeight(10.0f, 16.0f, 9.0f), 16.0f);
	TestEqual(TEXT("an embankment eases down too"), BankHeight(10.0f, 7.0f, 4.5f), 7.0f);
	float Steepest = 0.0f;
	for (float Beyond = 0.0f; Beyond < 9.0f; Beyond += 0.05f)
	{
		Steepest = FMath::Max(Steepest, FMath::Abs(BankHeight(10.0f, 16.0f, Beyond + 0.05f) - BankHeight(10.0f, 16.0f, Beyond)) / 0.05f);
	}
	TestTrue(FString::Printf(TEXT("a 6-voxel cutting's bank is at most 45 degrees (steepest %.2f)"), Steepest), Steepest <= 1.01f);

	// In generated chunks: along real roads, count road columns beside a wall -
	// a neighbouring column whose ground stands 3 or more voxels above the road.
	MadPoiTests::FFixture Fixture;
	FMadWorldGenSettings Settings;
	Settings.Seed = 20260912u;
	FMadWorldGenerator Generator(Settings, Fixture.Biomes, Fixture.Blocks, &Fixture.Prefabs);
	const FMadPoiPlanner& Planner = Generator.GetPoiPlanner();
	const uint16 RoadId = Fixture.Blocks.ResolveRuntimeId(FName(TEXT("madfall:gravel_path")));

	TMap<FMadChunkCoord, FMadChunkStorage> Chunks;
	auto VoxelAt = [&](int32 X, int32 Y, int32 Z)
	{
		const FMadChunkCoord C = MadFall::WorldToChunk(X, Y, Z);
		FMadChunkStorage* Storage = Chunks.Find(C);
		if (Storage == nullptr)
		{
			Storage = &Chunks.Add(C);
			Generator.GenerateChunk(C, *Storage);
		}
		return Storage->GetVoxel(MadFall::VoxelIndex(X - C.X * MadFall::ChunkSize, Y - C.Y * MadFall::ChunkSize, Z - C.Z * MadFall::ChunkSize));
	};
	auto GroundTop = [&](int32 X, int32 Y, uint16& OutBlock)
	{
		const int32 Guess = FMath::FloorToInt(Generator.GetSurfaceHeight(static_cast<float>(X) + 0.5f, static_cast<float>(Y) + 0.5f));
		for (int32 Z = Guess + 12; Z >= Guess - 12; --Z)
		{
			if (!MadFall::IsValidWorldZ(Z))
			{
				continue;
			}
			const FMadVoxel Voxel = VoxelAt(X, Y, Z);
			if (Voxel.Density >= 128 && Voxel.BlockTypeID != MadFall::BlockTypeAir && !Voxel.HasFlag(EMadVoxelFlags::Cubic))
			{
				OutBlock = Voxel.BlockTypeID;
				return Z;
			}
		}
		OutBlock = MadFall::BlockTypeAir;
		return static_cast<int32>(INDEX_NONE);
	};

	TArray<FMadRoadSegment> Found;
	for (int32 CY = -24; CY <= 24 && Found.Num() < 2; CY += 3)
	{
		for (int32 CX = -24; CX <= 24 && Found.Num() < 2; CX += 3)
		{
			TArray<FMadRoadSegment> Roads;
			Planner.GetRoadsNearChunk(Generator, FMadChunkCoord(CX, CY, 0), Roads);
			for (const FMadRoadSegment& Road : Roads)
			{
				if (!Found.ContainsByPredicate([&Road](const FMadRoadSegment& R) { return R.Start == Road.Start && R.End == Road.End; }) && Found.Num() < 2)
				{
					Found.Add(Road);
				}
			}
		}
	}
	if (!TestTrue(TEXT("roads to check"), Found.Num() > 0))
	{
		return false;
	}

	int32 Pairs = 0;
	int32 Walls = 0;
	int32 Cuttings = 0;
	float DeepestCut = 0.0f;
	FIntVector DeepestAt = FIntVector::ZeroValue;
	TSet<FIntPoint> Visited;
	const int32 Window = FMath::CeilToInt(Settings.RoadMeander) + Settings.RoadWidth + 1;
	for (const FMadRoadSegment& Road : Found)
	{
		for (float T = 0.2f; T <= 0.8f; T += 0.05f)
		{
			const int32 PX = FMath::RoundToInt(FMath::Lerp(static_cast<float>(Road.Start.X), static_cast<float>(Road.End.X), T));
			const int32 PY = FMath::RoundToInt(FMath::Lerp(static_cast<float>(Road.Start.Y), static_cast<float>(Road.End.Y), T));
			for (int32 Y = PY - Window; Y <= PY + Window; ++Y)
			{
				for (int32 X = PX - Window; X <= PX + Window; ++X)
				{
					if (Visited.Contains(FIntPoint(X, Y)))
					{
						continue;
					}
					Visited.Add(FIntPoint(X, Y));
					uint16 Block = 0;
					const int32 Top = GroundTop(X, Y, Block);
					if (Top == INDEX_NONE || Block != RoadId)
					{
						continue;
					}
					const float Cut = Generator.GetSurfaceHeight(static_cast<float>(X) + 0.5f, static_cast<float>(Y) + 0.5f) - static_cast<float>(Top);
					if (Cut >= 3.0f)
					{
						++Cuttings;
					}
					if (Cut > DeepestCut)
					{
						DeepestCut = Cut;
						DeepestAt = FIntVector(X, Y, Top);
					}
					for (const FIntPoint& Step : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
					{
						uint16 NeighbourBlock = 0;
						const int32 NeighbourTop = GroundTop(X + Step.X, Y + Step.Y, NeighbourBlock);
						if (NeighbourTop == INDEX_NONE || NeighbourBlock == RoadId)
						{
							continue;
						}
						++Pairs;
						Walls += NeighbourTop - Top >= 3 ? 1 : 0;
					}
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d road edge(s) checked, %d wall(s), %d road column(s) in a cutting of 3+ voxels, %d chunk(s) generated; deepest cut %.1f at %s (seed %u)"),
		Pairs, Walls, Cuttings, Chunks.Num(), DeepestCut, *DeepestAt.ToString(), Settings.Seed));
	TestTrue(TEXT("road edges were found"), Pairs > 0);
	if (Cuttings == 0)
	{
		AddWarning(TEXT("no cutting along these roads; the wall check proves little"));
	}
	// Warped terrain can overhang a road edge now and then; a trench would fail most edges in a cutting.
	TestTrue(FString::Printf(TEXT("road sides are banks, not walls (%d of %d)"), Walls, Pairs), Walls <= FMath::Max(2, Pairs / 50));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWorldGenStreamingCostTest,
	"MadFall.WorldGen.StreamingCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWorldGenStreamingCostTest::RunTest(const FString& Parameters)
{
	// The chunks the frame-budget gate streams (seed 0, walking +X from spawn),
	// with the real prefabs so roads, banks and POIs are all in. A throughput
	// number: generation runs on workers, and a slow generator shows up as the
	// far terrain starving for those workers, not as frame time.
	MadPoiTests::FFixture Fixture;
	FMadWorldGenSettings Settings;
	Settings.Seed = 0u;
	FMadWorldGenerator Generator(Settings, Fixture.Biomes, Fixture.Blocks, &Fixture.Prefabs);

	TArray<FMadChunkCoord> Coords;
	for (int32 CX = -2; CX <= 10; ++CX)
	{
		for (int32 CY = -3; CY <= 3; ++CY)
		{
			const int32 Surface = FMath::FloorToInt(Generator.GetSurfaceHeight(CX * 32.0f + 16.0f, CY * 32.0f + 16.0f));
			const int32 CZ = MadFall::FloorDiv(Surface, MadFall::ChunkSize);
			for (int32 DZ = -2; DZ <= 2; ++DZ)
			{
				if (FMadChunkCoord(CX, CY, CZ + DZ).IsValidZ())
				{
					Coords.Add(FMadChunkCoord(CX, CY, CZ + DZ));
				}
			}
		}
	}

	int32 RoadChunks = 0;
	const double Start = FPlatformTime::Seconds();
	for (const FMadChunkCoord& Coord : Coords)
	{
		FMadChunkStorage Chunk;
		Generator.GenerateChunk(Coord, Chunk);
	}
	const double MeanMs = (FPlatformTime::Seconds() - Start) * 1000.0 / FMath::Max(1, Coords.Num());
	for (const FMadChunkCoord& Coord : Coords)
	{
		TArray<FMadRoadSegment> Roads;
		Generator.GetPoiPlanner().GetRoadsNearChunk(Generator, Coord, Roads);
		RoadChunks += Roads.Num() > 0 ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("Streaming cost: %.2f ms mean over %d chunks near the surface, %d with a road nearby."), MeanMs, Coords.Num(), RoadChunks));
	TestTrue(FString::Printf(TEXT("chunks along the gate's walk generate in under 25 ms (took %.2f ms)"), MeanMs), MeanMs < 25.0);
	return true;
}

// ===========================================================================
// Near-spawn POIs
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadPoiNearSpawnTest,
	"MadFall.WorldGen.NearSpawnPoi",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadPoiNearSpawnTest::RunTest(const FString& Parameters)
{
	MadPoiTests::FFixture Fixture;
	const FName Outpost(TEXT("madfall:trader_outpost"));
	const FMadPrefab* Prefab = nullptr;
	for (int32 Index = 0; Index < Fixture.Prefabs.Num(); ++Index)
	{
		if (Fixture.Prefabs.Get(Index).Id == Outpost)
		{
			Prefab = &Fixture.Prefabs.Get(Index);
		}
	}
	if (!TestNotNull(TEXT("the shipped trader outpost loads"), Prefab))
	{
		return false;
	}
	TestTrue(TEXT("it is a near_spawn prefab"), Prefab->Placement.bNearSpawn);
	TestTrue(TEXT("with a trader marker naming its trader"), Prefab->Markers.ContainsByPredicate([](const FMadPoiMarker& Marker)
	{
		return Marker.Type == FName(TEXT("trader")) && Marker.Trader == FName(TEXT("madfall:quartermaster"));
	}));

	for (const uint32 Seed : { 20260912u, 7u, 123456789u })
	{
		FMadWorldGenSettings Settings;
		Settings.Seed = Seed;
		FMadWorldGenerator Generator(Settings, Fixture.Biomes, Fixture.Blocks, &Fixture.Prefabs);
		const FMadPoiPlanner& Planner = Generator.GetPoiPlanner();

		// Planning a far cell first must not change where the outpost goes.
		FMadPoiInstance Far;
		Planner.PlanCell(Generator, 20, -17, Far);

		FIntPoint Cell;
		if (!TestTrue(FString::Printf(TEXT("seed %u: the outpost has a cell"), Seed), Planner.GetNearSpawnCell(Generator, Outpost, Cell)))
		{
			continue;
		}
		const int32 Ring = FMath::Max(FMath::Max(Cell.X, -1 - Cell.X), FMath::Max(Cell.Y, -1 - Cell.Y));
		TestTrue(FString::Printf(TEXT("seed %u: in the rings around the spawn, not on it (cell %d,%d)"), Seed, Cell.X, Cell.Y),
			Ring >= 1 && Ring <= FMadPoiPlanner::NearSpawnRings);

		FMadWorldGenerator Fresh(Settings, Fixture.Biomes, Fixture.Blocks, &Fixture.Prefabs);
		FIntPoint FreshCell;
		Fresh.GetPoiPlanner().GetNearSpawnCell(Fresh, Outpost, FreshCell);
		TestEqual(FString::Printf(TEXT("seed %u: the same cell from a fresh generator"), Seed), FreshCell, Cell);

		FMadPoiInstance Poi;
		TestTrue(FString::Printf(TEXT("seed %u: the cell plans the outpost"), Seed),
			Planner.PlanCell(Generator, Cell.X, Cell.Y, Poi) && Planner.GetPrefab(Poi) == Prefab);

		TArray<FMadPoiWorldMarker> Markers;
		Planner.GetWorldMarkers(Poi, Markers);
		TestTrue(FString::Printf(TEXT("seed %u: the trader marker reaches the world"), Seed), Markers.ContainsByPredicate([](const FMadPoiWorldMarker& Marker)
		{
			return Marker.Type == FName(TEXT("trader")) && Marker.Trader == FName(TEXT("madfall:quartermaster"));
		}));

		int32 Outposts = 0;
		for (int32 Y = -8; Y <= 8; ++Y)
		{
			for (int32 X = -8; X <= 8; ++X)
			{
				FMadPoiInstance Other;
				if (Planner.PlanCell(Generator, X, Y, Other) && Planner.GetPrefab(Other) == Prefab)
				{
					++Outposts;
				}
			}
		}
		TestEqual(FString::Printf(TEXT("seed %u: exactly one outpost in 17x17 cells"), Seed), Outposts, 1);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

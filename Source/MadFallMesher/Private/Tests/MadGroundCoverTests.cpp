// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBlockRegistry.h"
#include "MadChunkStorage.h"
#include "MadGroundCover.h"
#include "MadSurfaceRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadGroundCoverTest,
	"MadFall.Mesher.GroundCover",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadGroundCoverTest::RunTest(const FString& Parameters)
{
	using MadFall::GroundCover::FTuft;

	const FMadSurfaceDefinition* Grass = MadFall::GetSurfaces().Find(FName(TEXT("madfall:grass")));
	if (!TestNotNull(TEXT("the shipped grass surface exists"), Grass))
	{
		return false;
	}
	TestTrue(TEXT("shipped grass has cover"), Grass->HasCover());

	FMadBlockRegistry Registry;
	TArray<FMadDefinitionError> Errors;
	Registry.BeginLoad();
	FMadBlockDefinitionData Turf;
	Turf.Id = FName(TEXT("test:turf"));
	Turf.MaterialClass = FName(TEXT("madfall:grass"));
	Turf.SourceModId = FName(TEXT("test"));
	Registry.AddFromAsset(Turf, Errors);
	FMadBlockDefinitionData Rock;
	Rock.Id = FName(TEXT("test:rock"));
	Rock.MaterialClass = FName(TEXT("madfall:stone"));
	Rock.SourceModId = FName(TEXT("test"));
	Registry.AddFromAsset(Rock, Errors);
	Registry.FinishLoad(Errors);
	const uint16 TurfId = Registry.ResolveRuntimeId(Turf.Id);
	const uint16 RockId = Registry.ResolveRuntimeId(Rock.Id);

	auto Solid = [](uint16 Id, uint8 Density = 255)
	{
		FMadVoxel Voxel = FMadVoxel::Air();
		Voxel.BlockTypeID = Id;
		Voxel.Density = Density;
		return Voxel;
	};

	// A smooth grass field: the top layer at z 9 is dense, the air above it thin.
	FMadChunkStorage Field;
	for (int32 Y = 0; Y < MadFall::ChunkSize; ++Y)
	{
		for (int32 X = 0; X < MadFall::ChunkSize; ++X)
		{
			for (int32 Z = 0; Z <= 9; ++Z)
			{
				Field.SetVoxel(X, Y, Z, Solid(TurfId, Z == 9 ? 192 : 255));
			}
		}
	}
	TArray<FTuft> Tufts;
	MadFall::GroundCover::Collect(Field, FMadChunkCoord(0, 0, 0), Registry, Tufts);
	const int32 Columns = MadFall::ChunkSize * MadFall::ChunkSize;
	int32 Grasses = 0;
	int32 Flowers = 0;
	bool bAllOnSurface = true;
	bool bAllInChunk = true;
	for (const FTuft& Tuft : Tufts)
	{
		(Tuft.bFlower ? Flowers : Grasses) += 1;
		// Density 192 at z 9's centre (9.5) and 0 at z 10's (10.5): 128 is a third of the way up, at 9.833.
		bAllOnSurface &= FMath::Abs(Tuft.Location.Z - ((9.5f + 64.0f / 192.0f) * MadFall::VoxelSizeUU - 3.0f)) < 1.0f;
		bAllInChunk &= Tuft.Location.X >= 0.0 && Tuft.Location.X < MadFall::ChunkSize * MadFall::VoxelSizeUU
			&& Tuft.Location.Y >= 0.0 && Tuft.Location.Y < MadFall::ChunkSize * MadFall::VoxelSizeUU;
	}
	TestTrue(FString::Printf(TEXT("grass grows at about its density (%d of %d columns, density %.2f)"), Grasses, Columns, Grass->CoverDensity),
		FMath::Abs(Grasses / static_cast<float>(Columns) - Grass->CoverDensity) < 0.08f);
	TestTrue(FString::Printf(TEXT("some flowers, fewer than grass (%d)"), Flowers), Flowers > 0 && Flowers < Grasses);
	TestTrue(TEXT("tufts stand on the smooth surface, not the voxel top"), bAllOnSurface);
	TestTrue(TEXT("tufts stay inside their chunk's columns"), bAllInChunk);

	TArray<FTuft> Again;
	MadFall::GroundCover::Collect(Field, FMadChunkCoord(0, 0, 0), Registry, Again);
	TestTrue(TEXT("the same ground grows the same grass"), Again.Num() == Tufts.Num() && (Tufts.Num() == 0 || Again[0].Location.Equals(Tufts[0].Location)));

	// Nothing on bare rock, nothing under a block, a placed cubic block's top.
	FMadChunkStorage Mixed;
	Mixed.SetVoxel(1, 1, 4, Solid(RockId));
	for (int32 X = 0; X < MadFall::ChunkSize; ++X)
	{
		for (int32 Y = 8; Y < MadFall::ChunkSize; ++Y)
		{
			FMadVoxel Cubic = Solid(TurfId);
			Cubic.SetFlag(EMadVoxelFlags::Cubic, true);
			Mixed.SetVoxel(X, Y, 2, Cubic);
			if (Y >= 20)
			{
				Mixed.SetVoxel(X, Y, 3, Solid(RockId));   // roofed over
			}
		}
	}
	MadFall::GroundCover::Collect(Mixed, FMadChunkCoord(0, 0, 0), Registry, Tufts);
	bool bNoneOnRockOrRoofed = true;
	bool bCubicTop = true;
	for (const FTuft& Tuft : Tufts)
	{
		const int32 Row = FMath::FloorToInt32(Tuft.Location.Y / MadFall::VoxelSizeUU);
		bNoneOnRockOrRoofed &= Row >= 8 && Row < 20;
		bCubicTop &= FMath::Abs(Tuft.Location.Z - (3.0f * MadFall::VoxelSizeUU - 3.0f)) < 1.0f;
	}
	TestTrue(TEXT("no cover on rock or under a roof"), bNoneOnRockOrRoofed);
	TestTrue(TEXT("on a placed block, cover stands on its top face"), bCubicTop);
	TestTrue(TEXT("the uncovered grass blocks do grow some"), Tufts.Num() > 0);

	// Transforms stand the card up over its base.
	FTuft Tuft;
	Tuft.Location = FVector(150.0, 250.0, 1000.0);
	Tuft.Height = 40.0f;
	Tuft.Width = 60.0f;
	FTransform A, B;
	MadFall::GroundCover::MakeTransforms(Tuft, A, B);
	const FVector Up = A.TransformVector(FVector(0.0, 50.0, 0.0));
	TestTrue(FString::Printf(TEXT("the plane's +Y edge points at the sky (%s)"), *Up.ToString()), Up.Z > 19.0 && FMath::Abs(Up.X) < 1.0 && FMath::Abs(Up.Y) < 1.0);
	TestTrue(TEXT("the card's bottom edge sits on the base"), FMath::IsNearlyEqual(A.TransformPosition(FVector(0.0, -50.0, 0.0)).Z, 1000.0, 0.5));
	return true;
}

#endif

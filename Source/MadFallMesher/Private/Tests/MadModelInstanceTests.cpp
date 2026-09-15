// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadBlockDefinitionJson.h"
#include "MadBlockRegistry.h"
#include "MadChunkMesher.h"
#include "MadChunkSampleGrid.h"
#include "MadChunkStorage.h"
#include "MadModelInstances.h"
#include "MadOrientation.h"
#include "MadVoxelWorldSubsystem.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadModelTests
{
	struct FBlocks
	{
		uint16 Rock = 0;
		uint16 Barrel = 0;
	};

	FBlocks BuildRegistry(FMadBlockRegistry& Registry)
	{
		TArray<FMadDefinitionError> Errors;
		Registry.BeginLoad();

		FMadBlockDefinitionData Rock;
		Rock.Id = FName(TEXT("test:rock_block"));
		Rock.MaterialClass = FName(TEXT("test:rock"));
		Rock.SourceModId = FName(TEXT("test"));
		Registry.AddFromAsset(Rock, Errors);

		FMadBlockDefinitionData Barrel;
		Barrel.Id = FName(TEXT("test:barrel"));
		Barrel.MaterialClass = FName(TEXT("test:wood"));
		Barrel.SourceModId = FName(TEXT("test"));
		Barrel.ShapeKind = EMadBlockShapeKind::Model;
		Barrel.MeshOffset = FVector(0.0, 0.0, -0.25);
		Barrel.MeshScale = FVector(0.8, 0.8, 0.5);
		Registry.AddFromAsset(Barrel, Errors);

		Registry.FinishLoad(Errors);

		FBlocks Blocks;
		Blocks.Rock = Registry.ResolveRuntimeId(Rock.Id);
		Blocks.Barrel = Registry.ResolveRuntimeId(Barrel.Id);
		return Blocks;
	}

	FMadVoxel MakeVoxel(uint16 BlockId, uint8 Orientation = 0)
	{
		FMadVoxel Voxel = FMadVoxel::Air();
		Voxel.BlockTypeID = BlockId;
		Voxel.Density = 255;
		Voxel.SetOrientation(Orientation);
		Voxel.SetFlag(EMadVoxelFlags::Cubic, true);
		return Voxel;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadModelMesherTest,
	"MadFall.Mesher.ModelBlocks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadModelMesherTest::RunTest(const FString& Parameters)
{
	using namespace MadModelTests;

	FMadBlockRegistry Registry;
	const FBlocks Blocks = BuildRegistry(Registry);
	TestTrue(TEXT("both test blocks registered"), Blocks.Rock != 0 && Blocks.Barrel != 0);

	// A grid with a rock at (5,5,5) and, optionally, a barrel beside it at (6,5,5).
	auto Build = [&](bool bRock, bool bBarrel, bool bCubicFlag)
	{
		FMadChunkSampleGrid Grid;
		for (int32 Z = -1; Z <= MadFall::ChunkSize; ++Z)
		{
			for (int32 Y = -1; Y <= MadFall::ChunkSize; ++Y)
			{
				for (int32 X = -1; X <= MadFall::ChunkSize; ++X)
				{
					const int32 Index = FMadChunkSampleGrid::Index(X, Y, Z);
					const bool bIsRock = bRock && X == 5 && Y == 5 && Z == 5;
					const bool bIsBarrel = bBarrel && X == 6 && Y == 5 && Z == 5;
					Grid.Density[Index] = (bIsRock || bIsBarrel) ? 255 : 0;
					Grid.BlockId[Index] = bIsRock ? Blocks.Rock : (bIsBarrel ? Blocks.Barrel : MadFall::BlockTypeAir);
					Grid.Flags[Index] = ((bIsRock || bIsBarrel) && bCubicFlag) ? static_cast<uint8>(EMadVoxelFlags::Cubic) : 0;
				}
			}
		}
		FMadChunkMesh Mesh;
		MadFall::ChunkMesher::BuildChunkMesh(Grid, Registry, MadFall::ChunkMesher::FMeshSettings(), Mesh);
		return Mesh.TotalTriangles();
	};

	TestEqual(TEXT("a lone cubic rock is 12 triangles"), Build(true, false, true), 12);
	TestEqual(TEXT("a model voxel emits no chunk faces (cubic flag)"), Build(false, true, true), 0);
	TestEqual(TEXT("a model voxel emits no isosurface either (no cubic flag)"), Build(false, true, false), 0);
	TestEqual(TEXT("a barrel does not hide the rock face beside it"), Build(true, true, true), 12);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadModelInstancesTest,
	"MadFall.Mesher.ModelInstances",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadModelInstancesTest::RunTest(const FString& Parameters)
{
	using namespace MadModelTests;

	// --- the transform agrees with the voxel orientation table, for all 24 ---
	for (uint8 Orientation = 0; Orientation < MadFall::Orientation::Count; ++Orientation)
	{
		const MadFall::Orientation::FIntMatrix3& M = MadFall::Orientation::GetMatrix(Orientation);
		const FTransform Transform = MadFall::Models::MakeTransform(FIntVector::ZeroValue, Orientation, FVector::ZeroVector, FVector::OneVector);
		for (const FIntVector& Axis : { FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(0, 0, 1) })
		{
			const FVector Expected(M.Transform(Axis));
			const FVector Actual = Transform.GetRotation().RotateVector(FVector(Axis));
			if (!Actual.Equals(Expected, 1e-4))
			{
				AddError(FString::Printf(TEXT("orientation %d maps %s to %s, the voxel table says %s"),
					Orientation, *Axis.ToString(), *Actual.ToString(), *Expected.ToString()));
			}
		}
	}

	// --- location, offset and scale ---
	{
		const FTransform Transform = MadFall::Models::MakeTransform(FIntVector(2, -3, 4), 0, FVector(0.0, 0.0, -0.25), FVector(0.8, 0.8, 0.5));
		TestTrue(TEXT("centre of voxel (2,-3,4), offset a quarter voxel down"),
			Transform.GetLocation().Equals(FVector(250.0, -250.0, 425.0), 1e-3));
		TestTrue(TEXT("scale carried through"), Transform.GetScale3D().Equals(FVector(0.8, 0.8, 0.5), 1e-4));
	}

	// --- the offset turns with the block ---
	{
		const int32 Flipped = MadFall::Orientation::FindIndex([] { MadFall::Orientation::FIntMatrix3 M; M.M[1][1] = -1; M.M[2][2] = -1; return M; }());
		TestTrue(TEXT("upside-down orientation exists"), Flipped != INDEX_NONE);
		if (Flipped != INDEX_NONE)
		{
			const FTransform Transform = MadFall::Models::MakeTransform(FIntVector::ZeroValue, static_cast<uint8>(Flipped), FVector(0.0, 0.0, -0.25), FVector::OneVector);
			TestTrue(TEXT("an upside-down model's downward offset points up"),
				Transform.GetLocation().Equals(FVector(50.0, 50.0, 75.0), 1e-3));
		}
	}

	// --- collection from chunk storage ---
	{
		FMadBlockRegistry Registry;
		const FBlocks Blocks = BuildRegistry(Registry);
		const FMadChunkCoord Coord(1, 0, -1);

		FMadChunkStorage Storage;
		TMap<uint16, TArray<FTransform>> ByBlock;
		MadFall::Models::CollectInstances(Storage, Coord, Registry, ByBlock);
		TestEqual(TEXT("an air chunk has no models"), ByBlock.Num(), 0);

		Storage.SetVoxel(1, 2, 3, MakeVoxel(Blocks.Rock));
		MadFall::Models::CollectInstances(Storage, Coord, Registry, ByBlock);
		TestEqual(TEXT("a chunk of cubes has no models"), ByBlock.Num(), 0);

		Storage.SetVoxel(4, 5, 6, MakeVoxel(Blocks.Barrel));
		Storage.SetVoxel(7, 8, 9, MakeVoxel(Blocks.Barrel, 5));
		MadFall::Models::CollectInstances(Storage, Coord, Registry, ByBlock);
		TestEqual(TEXT("one model block type"), ByBlock.Num(), 1);
		const TArray<FTransform>* Barrels = ByBlock.Find(Blocks.Barrel);
		TestTrue(TEXT("two barrels found"), Barrels != nullptr && Barrels->Num() == 2);
		if (Barrels != nullptr && Barrels->Num() == 2)
		{
			const FTransform Expected = MadFall::Models::MakeTransform(
				FIntVector(MadFall::ChunkSize + 4, 5, -MadFall::ChunkSize + 6), 0, FVector(0.0, 0.0, -0.25), FVector(0.8, 0.8, 0.5));
			TestTrue(TEXT("barrel placed in world space from the chunk coordinate"),
				(*Barrels)[0].GetLocation().Equals(Expected.GetLocation(), 1e-3));
			const FTransform Rotated = MadFall::Models::MakeTransform(
				FIntVector(MadFall::ChunkSize + 7, 8, -MadFall::ChunkSize + 9), 5, FVector(0.0, 0.0, -0.25), FVector(0.8, 0.8, 0.5));
			TestTrue(TEXT("barrel keeps its orientation"),
				(*Barrels)[1].GetRotation().Equals(Rotated.GetRotation(), 1e-4));
		}

		Storage.SetVoxel(4, 5, 6, FMadVoxel::Air());
		Storage.SetVoxel(7, 8, 9, FMadVoxel::Air());
		MadFall::Models::CollectInstances(Storage, Coord, Registry, ByBlock);
		TestEqual(TEXT("removing the barrels removes their instances (stale palette entries are skipped)"), ByBlock.Num(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadBlockLightsTest,
	"MadFall.Mesher.BlockLights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadBlockLightsTest::RunTest(const FString& Parameters)
{
	using namespace MadModelTests;

	// --- parsing: sRGB colour in, linear out; bounds ---
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT(R"({
			"schema": "madfall.block/1", "id": "test:lamp",
			"render": { "light": { "color": [0.5, 1.0, 0.0], "lumens": 200, "radius": 99, "offset": [0, 0, 0.25], "flicker": 1 } }
		})")), Object);
		FMadBlockDefinitionData Data;
		TArray<FMadDefinitionError> Errors;
		MadFall::BlockDefinitionJson::ParseObject(Object.ToSharedRef(), TEXT("lamp.json"), FName(TEXT("test")), Data, Errors);
		TestTrue(TEXT("lamp gives light"), Data.HasLight());
		TestTrue(TEXT("light colour is converted from sRGB"), FMath::IsNearlyEqual(Data.LightColor.R, 0.214f, 0.002f) && Data.LightColor.G == 1.0f);
		TestEqual(TEXT("radius clamped to 32 voxels"), Data.LightRadius, 32.0f);
		TestTrue(TEXT("unknown light field reported"), Errors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.Pointer.Contains(TEXT("flicker")); }));
		TestEqual(TEXT("a light has a torch-sized flame unless it says otherwise"), Data.LightFlame, 1.0f);

		TSharedPtr<FJsonObject> Lamp;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT(R"({
			"schema": "madfall.block/1", "id": "test:lantern",
			"render": { "light": { "lumens": 80, "flame": 0 } }
		})")), Lamp);
		FMadBlockDefinitionData LampData;
		TArray<FMadDefinitionError> LampErrors;
		MadFall::BlockDefinitionJson::ParseObject(Lamp.ToSharedRef(), TEXT("lantern.json"), FName(TEXT("test")), LampData, LampErrors);
		TestEqual(TEXT("flame 0 draws none"), LampData.LightFlame, 0.0f);
		TestEqual(TEXT("flame is a known field"), LampErrors.Num(), 0);
	}

	// --- collection ---
	FMadBlockRegistry Registry;
	TArray<FMadDefinitionError> Errors;
	Registry.BeginLoad();
	FMadBlockDefinitionData Rock;
	Rock.Id = FName(TEXT("test:rock_block"));
	Rock.SourceModId = FName(TEXT("test"));
	Registry.AddFromAsset(Rock, Errors);
	FMadBlockDefinitionData Torch;
	Torch.Id = FName(TEXT("test:torch"));
	Torch.SourceModId = FName(TEXT("test"));
	Torch.ShapeKind = EMadBlockShapeKind::Model;
	Torch.LightLumens = 150.0f;
	Torch.LightOffset = FVector(0.0, 0.0, 0.25);
	Registry.AddFromAsset(Torch, Errors);
	Registry.FinishLoad(Errors);
	const uint16 RockId = Registry.ResolveRuntimeId(Rock.Id);
	const uint16 TorchId = Registry.ResolveRuntimeId(Torch.Id);

	FMadChunkStorage Storage;
	TArray<MadFall::Models::FBlockLight> Lights;
	Storage.SetVoxel(1, 1, 1, MakeVoxel(RockId));
	MadFall::Models::CollectLights(Storage, FMadChunkCoord(0, 0, 0), Registry, Lights);
	TestEqual(TEXT("a chunk without light blocks has no lights"), Lights.Num(), 0);

	Storage.SetVoxel(3, 4, 5, MakeVoxel(TorchId));
	MadFall::Models::CollectLights(Storage, FMadChunkCoord(-1, 0, 0), Registry, Lights);
	if (TestEqual(TEXT("one torch, one light"), Lights.Num(), 1))
	{
		TestEqual(TEXT("light belongs to the torch"), Lights[0].BlockId, TorchId);
		TestTrue(TEXT("light sits at the voxel centre plus its offset, in world space"),
			Lights[0].Location.Equals(FVector((-MadFall::ChunkSize + 3 + 0.5) * 100.0, 450.0, 575.0), 1e-3));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadShippedModelsTest,
	"MadFall.Mesher.ShippedModels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadShippedModelsTest::RunTest(const FString& Parameters)
{
	// A model block whose mesh path is wrong still loads as data and draws a
	// cube with a warning - easy to ship unnoticed, so the shipped (and
	// installed mods') model blocks must all name a mesh that loads.
	int32 Models = 0;
	for (const FMadBlockEntry& Entry : UMadVoxelWorldSubsystem::GetBlockRegistry().GetEntries())
	{
		const FMadBlockDefinitionData& Def = Entry.Definition;
		if (Entry.bUnresolved || Def.ShapeKind != EMadBlockShapeKind::Model)
		{
			continue;
		}
		++Models;
		TestFalse(FString::Printf(TEXT("%s names a render.mesh"), *Def.Id.ToString()), Def.Mesh.IsNull());
		if (!Def.Mesh.IsNull())
		{
			TestNotNull(FString::Printf(TEXT("%s mesh '%s' loads as a static mesh"), *Def.Id.ToString(), *Def.Mesh.ToString()),
				Cast<UStaticMesh>(Def.Mesh.TryLoad()));
		}
		if (!Def.Material.IsNull())
		{
			TestNotNull(FString::Printf(TEXT("%s material '%s' loads"), *Def.Id.ToString(), *Def.Material.ToString()),
				Cast<UMaterialInterface>(Def.Material.TryLoad()));
		}
	}
	TestTrue(TEXT("the shipped storage barrel is a model block"),
		UMadVoxelWorldSubsystem::GetBlockRegistry().GetBlockViewById(FName(TEXT("madfall:storage_barrel"))).ShapeKind == EMadBlockShapeKind::Model);
	AddInfo(FString::Printf(TEXT("%d model blocks checked"), Models));
	return true;
}

#endif

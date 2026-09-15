// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "MadBitPackedArray.h"
#include "MadBlockDefinitionJson.h"
#include "MadBlockRegistry.h"
#include "MadChunkSerializer.h"
#include "MadChunkStorage.h"
#include "MadFallCoordinates.h"
#include "MadRegionFile.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadFallTests
{
	/** A scratch directory that deletes itself, so a failing test cannot poison the next run. */
	struct FScopedTestDirectory
	{
		FString Path;

		FScopedTestDirectory()
		{
			Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MadFallTests"), *FGuid::NewGuid().ToString());
			IFileManager::Get().MakeDirectory(*Path, true);
		}

		~FScopedTestDirectory()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}

		FString File(const TCHAR* Name) const { return FPaths::Combine(Path, Name); }
	};

	/** Builds a registry from inline JSON, exercising the real loader rather than a shortcut. */
	void BuildRegistryFromJson(FMadBlockRegistry& Registry, const FString& Directory,
		const TArray<TPair<FString, FString>>& Files, FName ModId, TArray<FMadDefinitionError>& OutErrors)
	{
		for (const TPair<FString, FString>& Pair : Files)
		{
			FFileHelper::SaveStringToFile(Pair.Value, *FPaths::Combine(Directory, Pair.Key));
		}

		Registry.BeginLoad();
		Registry.AddFromDirectory(Directory, ModId, OutErrors);
		Registry.FinishLoad(OutErrors);
	}
}

// ===========================================================================
// Coordinates
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadCoordinatesTest,
	"MadFall.Core.Coordinates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadCoordinatesTest::RunTest(const FString& Parameters)
{
	using namespace MadFall;

	// Truncating division is the classic voxel bug: -1 / 32 is 0 in C++, which
	// would put world Z = -1 in the same chunk as Z = 0 and silently overlap two
	// chunks' worth of terrain.
	TestEqual(TEXT("FloorDiv(-1, 32)"), FloorDiv(-1, 32), -1);
	TestEqual(TEXT("FloorDiv(-32, 32)"), FloorDiv(-32, 32), -1);
	TestEqual(TEXT("FloorDiv(-33, 32)"), FloorDiv(-33, 32), -2);
	TestEqual(TEXT("FloorDiv(31, 32)"), FloorDiv(31, 32), 0);
	TestEqual(TEXT("FloorDiv(32, 32)"), FloorDiv(32, 32), 1);

	TestEqual(TEXT("FloorMod(-1, 32)"), FloorMod(-1, 32), 31);
	TestEqual(TEXT("FloorMod(-32, 32)"), FloorMod(-32, 32), 0);
	TestEqual(TEXT("FloorMod(-33, 32)"), FloorMod(-33, 32), 31);
	TestEqual(TEXT("FloorMod(33, 32)"), FloorMod(33, 32), 1);

	// World -> chunk + local -> world must be the identity everywhere, and the
	// negative side is where it breaks if anything truncates.
	const int32 Samples[] = { -1025, -129, -33, -32, -31, -1, 0, 1, 31, 32, 33, 383, 1024 };

	for (int32 X : Samples)
	{
		for (int32 Z : Samples)
		{
			if (!IsValidWorldZ(Z))
			{
				continue;
			}

			const FMadChunkCoord Chunk = WorldToChunk(X, 7, Z);

			int32 LocalX, LocalY, LocalZ;
			WorldToLocal(X, 7, Z, LocalX, LocalY, LocalZ);

			TestTrue(FString::Printf(TEXT("local X in range for world X %d"), X), LocalX >= 0 && LocalX < ChunkSize);
			TestTrue(FString::Printf(TEXT("local Z in range for world Z %d"), Z), LocalZ >= 0 && LocalZ < ChunkSize);

			int32 BackX, BackY, BackZ;
			ChunkToWorld(Chunk, LocalX, LocalY, LocalZ, BackX, BackY, BackZ);

			TestEqual(FString::Printf(TEXT("world X %d round trips"), X), BackX, X);
			TestEqual(FString::Printf(TEXT("world Z %d round trips"), Z), BackZ, Z);
		}
	}

	// Region slots must be a bijection over the whole 4096-slot space.
	TSet<int32> SeenSlots;
	for (int32 ChunkZ = WorldMinChunkZ; ChunkZ <= WorldMaxChunkZ; ++ChunkZ)
	{
		for (int32 LocalY = 0; LocalY < RegionChunksXY; ++LocalY)
		{
			for (int32 LocalX = 0; LocalX < RegionChunksXY; ++LocalX)
			{
				const FMadChunkCoord Chunk(LocalX, LocalY, ChunkZ);
				const int32 Slot = ChunkToRegionSlot(Chunk);

				TestTrue(TEXT("slot is in range"), Slot >= 0 && Slot < RegionChunkSlots);
				TestFalse(FString::Printf(TEXT("slot %d is not reused"), Slot), SeenSlots.Contains(Slot));
				SeenSlots.Add(Slot);

				const FMadChunkCoord Back = RegionSlotToChunk(FMadRegionCoord(0, 0), Slot);
				TestTrue(FString::Printf(TEXT("slot %d round trips to %s"), Slot, *Chunk.ToString()), Back == Chunk);
			}
		}
	}
	TestEqual(TEXT("every region slot is reachable"), SeenSlots.Num(), RegionChunkSlots);

	// A chunk at negative coordinates must land in the right region.
	TestTrue(TEXT("chunk (-1,-1) is in region (-1,-1)"),
		ChunkToRegion(FMadChunkCoord(-1, -1, 0)) == FMadRegionCoord(-1, -1));
	TestTrue(TEXT("chunk (-16,-16) is in region (-1,-1)"),
		ChunkToRegion(FMadChunkCoord(-16, -16, 0)) == FMadRegionCoord(-1, -1));
	TestTrue(TEXT("chunk (-17,-17) is in region (-2,-2)"),
		ChunkToRegion(FMadChunkCoord(-17, -17, 0)) == FMadRegionCoord(-2, -2));

	// Out-of-world Z has no slot at all rather than aliasing onto a valid one.
	TestEqual(TEXT("chunk below the world has no region slot"),
		ChunkToRegionSlot(FMadChunkCoord(0, 0, WorldMinChunkZ - 1)), INDEX_NONE);
	TestEqual(TEXT("chunk above the world has no region slot"),
		ChunkToRegionSlot(FMadChunkCoord(0, 0, WorldMaxChunkZ + 1)), INDEX_NONE);

	return true;
}

// ===========================================================================
// Bit-packed array
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadBitPackedArrayTest,
	"MadFall.Core.BitPackedArray",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadBitPackedArrayTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("2 values need 1 bit"), FMadBitPackedArray::BitsForValueCount(2), 1);
	TestEqual(TEXT("3 values need 2 bits"), FMadBitPackedArray::BitsForValueCount(3), 2);
	TestEqual(TEXT("16 values need 4 bits"), FMadBitPackedArray::BitsForValueCount(16), 4);
	TestEqual(TEXT("17 values need 8 bits"), FMadBitPackedArray::BitsForValueCount(17), 8);
	TestEqual(TEXT("257 values need 16 bits"), FMadBitPackedArray::BitsForValueCount(257), 16);

	for (int32 Bits : { 1, 2, 4, 8, 16 })
	{
		FMadBitPackedArray Array(1000, Bits);
		const uint32 MaxValue = static_cast<uint32>(Array.GetMaxValue());

		for (int32 Index = 0; Index < 1000; ++Index)
		{
			Array.Set(Index, static_cast<uint32>(Index) % (MaxValue + 1));
		}

		bool bAllMatch = true;
		for (int32 Index = 0; Index < 1000; ++Index)
		{
			if (Array.Get(Index) != static_cast<uint32>(Index) % (MaxValue + 1))
			{
				bAllMatch = false;
				break;
			}
		}

		TestTrue(FString::Printf(TEXT("%d-bit values round trip"), Bits), bAllMatch);
	}

	// Neighbouring values must not bleed into each other - the failure mode is
	// one block type silently becoming another.
	FMadBitPackedArray Packed(8, 4);
	Packed.Set(0, 0xF);
	Packed.Set(1, 0x0);
	Packed.Set(2, 0xF);
	TestEqual(TEXT("entry 0 unaffected by its neighbour"), Packed.Get(0), 0xFu);
	TestEqual(TEXT("entry 1 unaffected by its neighbours"), Packed.Get(1), 0x0u);
	TestEqual(TEXT("entry 2 unaffected by its neighbour"), Packed.Get(2), 0xFu);

	// Widening must preserve every value.
	FMadBitPackedArray Widening(64, 2);
	for (int32 Index = 0; Index < 64; ++Index)
	{
		Widening.Set(Index, static_cast<uint32>(Index % 4));
	}
	Widening.Repack(8);
	TestEqual(TEXT("repack changed the width"), Widening.GetBitsPerValue(), 8);

	bool bPreserved = true;
	for (int32 Index = 0; Index < 64; ++Index)
	{
		if (Widening.Get(Index) != static_cast<uint32>(Index % 4))
		{
			bPreserved = false;
			break;
		}
	}
	TestTrue(TEXT("repack preserved every value"), bPreserved);

	FMadBitPackedArray Filled(100, 4);
	Filled.Fill(7);
	bool bAllSeven = true;
	for (int32 Index = 0; Index < 100; ++Index)
	{
		if (Filled.Get(Index) != 7u) { bAllSeven = false; break; }
	}
	TestTrue(TEXT("Fill sets every entry"), bAllSeven);

	return true;
}

// ===========================================================================
// Chunk storage
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadChunkStorageTest,
	"MadFall.Core.ChunkStorage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadChunkStorageTest::RunTest(const FString& Parameters)
{
	FMadChunkStorage Chunk;

	// A fresh chunk is uniform air and must cost almost nothing. This is the
	// number the whole storage design exists to protect: a dense array would be
	// 196608 bytes here.
	TestTrue(TEXT("a fresh chunk is uniform"), Chunk.IsUniform());
	TestEqual(TEXT("a fresh chunk is air"), static_cast<int32>(Chunk.GetBlockId(0)), 0);
	TestTrue(FString::Printf(TEXT("a uniform chunk is under 256 bytes (was %lld)"), Chunk.GetAllocatedSize()),
		Chunk.GetAllocatedSize() < 256);

	// Placing one block must not allocate all four side arrays.
	FMadVoxel Stone;
	Stone.BlockTypeID = 5;
	Stone.Density = 255;
	Stone.Damage = 0;
	Stone.Rotation = 0;
	Stone.Flags = 0;

	Chunk.SetVoxel(MadFall::VoxelIndex(1, 2, 3), Stone);

	TestFalse(TEXT("a chunk with two block types is not uniform"), Chunk.IsUniform());
	TestEqual(TEXT("the written voxel reads back"), Chunk.GetVoxel(1, 2, 3), Stone);
	TestEqual(TEXT("a neighbouring voxel is still air"), static_cast<int32>(Chunk.GetBlockId(MadFall::VoxelIndex(2, 2, 3))), 0);
	TestEqual(TEXT("two distinct block types"), Chunk.GetDistinctBlockCount(), 2);

	// Rotation and flags were both zero, so their arrays must not exist yet.
	TestNull(TEXT("rotation array is not allocated for an unrotated block"), Chunk.GetRotationArray());
	TestNull(TEXT("flags array is not allocated for a flagless block"), Chunk.GetFlagsArray());
	TestNotNull(TEXT("density array is allocated once a density differs from the default"), Chunk.GetDensityArray());

	// Palette growth must re-tier the index array rather than truncate.
	for (int32 BlockType = 1; BlockType <= 40; ++BlockType)
	{
		FMadVoxel Voxel = Stone;
		Voxel.BlockTypeID = static_cast<uint16>(BlockType);
		Chunk.SetVoxel(MadFall::VoxelIndex(BlockType, 0, 0), Voxel);
	}

	TestTrue(TEXT("index width grew past 4 bits for 40+ block types"), Chunk.GetIndices().GetBitsPerValue() >= 8);

	bool bAllTypesCorrect = true;
	for (int32 BlockType = 1; BlockType <= 40; ++BlockType)
	{
		if (Chunk.GetBlockId(MadFall::VoxelIndex(BlockType, 0, 0)) != static_cast<uint16>(BlockType))
		{
			bAllTypesCorrect = false;
			break;
		}
	}
	TestTrue(TEXT("every block type survived the re-tier"), bAllTypesCorrect);

	// Compacting a chunk back to a single type must give the index memory back.
	FMadChunkStorage Demolished;
	FMadVoxel Air = FMadVoxel::Air();
	for (int32 BlockType = 1; BlockType <= 20; ++BlockType)
	{
		FMadVoxel Voxel = Stone;
		Voxel.BlockTypeID = static_cast<uint16>(BlockType);
		Demolished.SetVoxel(BlockType, Voxel);
	}
	for (int32 BlockType = 1; BlockType <= 20; ++BlockType)
	{
		Demolished.SetVoxel(BlockType, Air);
	}

	const int64 BeforeCompact = Demolished.GetAllocatedSize();
	Demolished.Compact();
	const int64 AfterCompact = Demolished.GetAllocatedSize();

	TestEqual(TEXT("compaction drops unreferenced palette entries"), Demolished.GetPalette().Num(), 1);
	TestTrue(FString::Printf(TEXT("compaction freed memory (%lld -> %lld)"), BeforeCompact, AfterCompact),
		AfterCompact < BeforeCompact);
	TestTrue(TEXT("indices are released once one block type remains"), Demolished.GetIndices().IsEmpty());

	// Fill must reset to a genuinely uniform chunk, not merely overwrite values.
	FMadChunkStorage Filled;
	FMadVoxel Bedrock = Stone;
	Bedrock.BlockTypeID = 9;
	Filled.SetVoxel(100, Stone);
	Filled.Fill(Bedrock);

	TestTrue(TEXT("Fill produces a uniform chunk"), Filled.IsUniform());
	TestEqual(TEXT("Fill sets every voxel"), Filled.GetVoxel(12345), Bedrock);
	TestTrue(TEXT("a filled chunk is still tiny"), Filled.GetAllocatedSize() < 256);

	// Damage is sparse and must not allocate 32 KiB for one damaged voxel.
	FMadChunkStorage Damaged;
	FMadVoxel Hurt = Stone;
	Hurt.Damage = 200;
	Damaged.SetVoxel(500, Hurt);
	TestEqual(TEXT("damage is recorded"), static_cast<int32>(Damaged.GetVoxel(500).Damage), 200);
	TestEqual(TEXT("damage map holds exactly one entry"), Damaged.GetDamageMap().Num(), 1);

	Hurt.Damage = 0;
	Damaged.SetVoxel(500, Hurt);
	TestEqual(TEXT("clearing damage removes the entry rather than storing a zero"),
		Damaged.GetDamageMap().Num(), 0);

	return true;
}

// ===========================================================================
// Block definitions and the registry
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadBlockIdValidationTest,
	"MadFall.Core.BlockRegistry.IdValidation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadBlockIdValidationTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::BlockDefinitionJson;

	FString Reason;

	TestTrue(TEXT("madfall:stone is valid"), IsValidBlockId(FName(TEXT("madfall:stone")), Reason));
	TestTrue(TEXT("my_mod:rebar_concrete_2 is valid"), IsValidBlockId(FName(TEXT("my_mod:rebar_concrete_2")), Reason));

	TestFalse(TEXT("an id with no namespace is rejected"), IsValidBlockId(FName(TEXT("stone")), Reason));
	TestFalse(TEXT("an empty namespace is rejected"), IsValidBlockId(FName(TEXT(":stone")), Reason));
	TestFalse(TEXT("an empty name is rejected"), IsValidBlockId(FName(TEXT("madfall:")), Reason));
	TestFalse(TEXT("two colons are rejected"), IsValidBlockId(FName(TEXT("a:b:c")), Reason));

	// Uppercase is rejected on purpose: ids reach case-sensitive and
	// case-insensitive filesystems alike, and per-platform resolution
	// differences are not a class of bug worth allowing in.
	TestFalse(TEXT("uppercase is rejected"), IsValidBlockId(FName(TEXT("MyMod:Stone")), Reason));
	TestFalse(TEXT("a hyphen is rejected"), IsValidBlockId(FName(TEXT("my-mod:stone")), Reason));

	TestEqual(TEXT("namespace extraction"), GetNamespace(FName(TEXT("madfall:stone"))), FName(TEXT("madfall")));
	TestEqual(TEXT("namespace of a malformed id is None"), GetNamespace(FName(TEXT("stone"))), FName(NAME_None));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadBlockDefinitionParseTest,
	"MadFall.Core.BlockRegistry.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadBlockDefinitionParseTest::RunTest(const FString& Parameters)
{
	// --- a good definition ---
	{
		const FString Json = TEXT(R"({
			"schema": "madfall.block/1",
			"id": "testmod:stone",
			"material": { "class": "madfall:stone", "mass_kg": 2600.0, "hardness": 400.0,
				"resistances": { "madfall:blunt": 0.6 } },
			"structure": { "support_strength": 6000.0, "max_horizontal_span": 6, "is_anchor": true },
			"shape": { "kind": "isosurface", "rotation_mode": "none" },
			"flags": { "transparent": false, "flammable": true }
		})");

		TArray<FMadBlockDefinitionData> Definitions;
		TArray<FMadDefinitionError> Errors;

		TestTrue(TEXT("a well-formed definition parses"),
			MadFall::BlockDefinitionJson::ParseText(Json, TEXT("test.json"), FName(TEXT("testmod")), Definitions, Errors));
		TestEqual(TEXT("one definition"), Definitions.Num(), 1);
		TestEqual(TEXT("no errors"), Errors.Num(), 0);

		if (Definitions.Num() == 1)
		{
			TestEqual(TEXT("id"), Definitions[0].Id, FName(TEXT("testmod:stone")));
			TestEqual(TEXT("mass"), Definitions[0].MassKg, 2600.0f);
			TestEqual(TEXT("hardness"), Definitions[0].Hardness, 400.0f);
			TestEqual(TEXT("support"), Definitions[0].SupportStrength, 6000.0f);
			TestTrue(TEXT("anchor flag"), Definitions[0].bIsAnchor);
			TestTrue(TEXT("flammable flag"), Definitions[0].bFlammable);
			TestTrue(TEXT("shape kind"), Definitions[0].ShapeKind == EMadBlockShapeKind::Isosurface);
			TestEqual(TEXT("resistance entry"), Definitions[0].Resistances.FindRef(FName(TEXT("madfall:blunt"))), 0.6f);
		}
	}

	// --- a wrong schema is refused rather than best-effort parsed ---
	{
		const FString Json = TEXT(R"({ "schema": "madfall.block/99", "id": "testmod:stone" })");

		TArray<FMadBlockDefinitionData> Definitions;
		TArray<FMadDefinitionError> Errors;

		TestFalse(TEXT("an unknown schema version is refused"),
			MadFall::BlockDefinitionJson::ParseText(Json, TEXT("test.json"), FName(TEXT("testmod")), Definitions, Errors));
		TestEqual(TEXT("nothing loaded"), Definitions.Num(), 0);
		TestTrue(TEXT("the schema error is reported"), Errors.Num() > 0);
	}

	// --- a mod may not define into another mod's namespace ---
	{
		const FString Json = TEXT(R"({ "schema": "madfall.block/1", "id": "madfall:stone" })");

		TArray<FMadBlockDefinitionData> Definitions;
		TArray<FMadDefinitionError> Errors;

		TestFalse(TEXT("cross-namespace definition is refused"),
			MadFall::BlockDefinitionJson::ParseText(Json, TEXT("test.json"), FName(TEXT("testmod")), Definitions, Errors));
		TestTrue(TEXT("the namespace error names the patch mechanism"),
			Errors.Num() > 0 && Errors[0].Message.Contains(TEXT("patch")));
	}

	// --- a bad field type is reported precisely and does not kill the block ---
	{
		const FString Json = TEXT(R"({
			"schema": "madfall.block/1",
			"id": "testmod:stone",
			"material": { "mass_kg": "heavy" }
		})");

		TArray<FMadBlockDefinitionData> Definitions;
		TArray<FMadDefinitionError> Errors;

		TestTrue(TEXT("the block still loads despite one bad field"),
			MadFall::BlockDefinitionJson::ParseText(Json, TEXT("test.json"), FName(TEXT("testmod")), Definitions, Errors));

		bool bFoundPointer = false;
		for (const FMadDefinitionError& Error : Errors)
		{
			if (Error.Pointer == TEXT("/material/mass_kg") && Error.Message.Contains(TEXT("a number")))
			{
				bFoundPointer = true;
			}
		}
		TestTrue(TEXT("the error names the exact JSON pointer and the expected type"), bFoundPointer);
	}

	// --- a misspelled field is reported rather than silently ignored ---
	{
		const FString Json = TEXT(R"({
			"schema": "madfall.block/1",
			"id": "testmod:stone",
			"materials": {}
		})");

		TArray<FMadBlockDefinitionData> Definitions;
		TArray<FMadDefinitionError> Errors;
		MadFall::BlockDefinitionJson::ParseText(Json, TEXT("test.json"), FName(TEXT("testmod")), Definitions, Errors);

		bool bFoundUnknown = false;
		for (const FMadDefinitionError& Error : Errors)
		{
			if (Error.Pointer == TEXT("/materials"))
			{
				bFoundUnknown = true;
			}
		}
		TestTrue(TEXT("an unknown field is reported"), bFoundUnknown);
	}

	// --- damage stages are sorted, and an out-of-range threshold is caught ---
	{
		const FString Json = TEXT(R"({
			"schema": "madfall.block/1",
			"id": "testmod:stone",
			"damage_states": [
				{ "at": 200, "support_multiplier": 0.2 },
				{ "at": 0, "support_multiplier": 1.0 },
				{ "at": 900, "support_multiplier": 0.0 }
			]
		})");

		TArray<FMadBlockDefinitionData> Definitions;
		TArray<FMadDefinitionError> Errors;
		MadFall::BlockDefinitionJson::ParseText(Json, TEXT("test.json"), FName(TEXT("testmod")), Definitions, Errors);

		if (TestEqual(TEXT("the definition loaded"), Definitions.Num(), 1))
		{
			const TArray<FMadBlockDamageStage>& Stages = Definitions[0].DamageStages;
			TestEqual(TEXT("three stages"), Stages.Num(), 3);
			TestEqual(TEXT("stages are sorted ascending"), Stages[0].At, 0);
			TestEqual(TEXT("second stage"), Stages[1].At, 200);
			TestEqual(TEXT("an out-of-range threshold is clamped to 255"), Stages[2].At, 255);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadBlockRegistryLoadTest,
	"MadFall.Core.BlockRegistry.LoadOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadBlockRegistryLoadTest::RunTest(const FString& Parameters)
{
	// These warnings are the behaviour under test, not noise. Declaring them
	// asserts they are actually emitted AND keeps the automation report clean -
	// a report with permanent warnings trains people to ignore warnings.
	AddExpectedMessagePlain(TEXT("is part of an inheritance cycle"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("does not exist or failed to load"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	MadFallTests::FScopedTestDirectory Directory;

	// --- inheritance via `extends` ---
	{
		TArray<TPair<FString, FString>> Files;
		Files.Emplace(TEXT("a_base.json"), TEXT(R"({
			"schema": "madfall.block/1",
			"id": "testmod:base",
			"material": { "class": "madfall:concrete", "mass_kg": 1800.0, "hardness": 500.0 },
			"structure": { "support_strength": 5000.0, "max_horizontal_span": 6 },
			"flags": { "conductive": true }
		})"));
		Files.Emplace(TEXT("b_child.json"), TEXT(R"({
			"schema": "madfall.block/1",
			"id": "testmod:child",
			"extends": "testmod:base",
			"material": { "hardness": 850.0 }
		})"));

		FMadBlockRegistry Registry;
		TArray<FMadDefinitionError> Errors;
		MadFallTests::BuildRegistryFromJson(Registry, Directory.Path, Files, FName(TEXT("testmod")), Errors);

		TestTrue(TEXT("the child registered"), Registry.IsRegistered(FName(TEXT("testmod:child"))));

		const uint16 ChildId = Registry.ResolveRuntimeId(FName(TEXT("testmod:child")));
		const FMadBlockDefinitionData* Child = Registry.FindDefinition(ChildId);

		if (TestNotNull(TEXT("the child definition exists"), Child))
		{
			// Overridden by the child.
			TestEqual(TEXT("the child overrides hardness"), Child->Hardness, 850.0f);
			// Inherited, because the child never mentions them.
			TestEqual(TEXT("the child inherits mass"), Child->MassKg, 1800.0f);
			TestEqual(TEXT("the child inherits support strength"), Child->SupportStrength, 5000.0f);
			TestEqual(TEXT("the child inherits the material class"),
				Child->MaterialClass, FName(TEXT("madfall:concrete")));
			TestTrue(TEXT("the child inherits flags"), Child->bConductive);
		}
	}

	// --- an inheritance cycle is reported, not a stack overflow ---
	{
		MadFallTests::FScopedTestDirectory CycleDirectory;

		TArray<TPair<FString, FString>> Files;
		Files.Emplace(TEXT("a.json"), TEXT(R"({ "schema": "madfall.block/1", "id": "testmod:a", "extends": "testmod:b" })"));
		Files.Emplace(TEXT("b.json"), TEXT(R"({ "schema": "madfall.block/1", "id": "testmod:b", "extends": "testmod:a" })"));

		FMadBlockRegistry Registry;
		TArray<FMadDefinitionError> Errors;
		MadFallTests::BuildRegistryFromJson(Registry, CycleDirectory.Path, Files, FName(TEXT("testmod")), Errors);

		bool bFoundCycle = false;
		for (const FMadDefinitionError& Error : Errors)
		{
			if (Error.Message.Contains(TEXT("cycle")))
			{
				bFoundCycle = true;
			}
		}
		TestTrue(TEXT("an inheritance cycle is reported by name"), bFoundCycle);
	}

	// --- reserved ids ---
	{
		FMadBlockRegistry Registry;
		TestEqual(TEXT("air is always runtime id 0"),
			static_cast<int32>(Registry.ResolveRuntimeId(FName(TEXT("madfall:air")))), 0);
		TestEqual(TEXT("an empty id resolves to air"),
			static_cast<int32>(Registry.ResolveRuntimeId(NAME_None)), 0);
		TestEqual(TEXT("an unknown id resolves to the unresolved sentinel"),
			static_cast<int32>(Registry.ResolveRuntimeId(FName(TEXT("nosuchmod:nosuchblock")))),
			static_cast<int32>(MadFall::BlockTypeUnresolved));
	}

	// --- runtime ids are deterministic across loads ---
	{
		MadFallTests::FScopedTestDirectory OrderDirectory;

		TArray<TPair<FString, FString>> Files;
		Files.Emplace(TEXT("z.json"), TEXT(R"({ "schema": "madfall.block/1", "id": "testmod:zinc" })"));
		Files.Emplace(TEXT("a.json"), TEXT(R"({ "schema": "madfall.block/1", "id": "testmod:alpha" })"));

		FMadBlockRegistry First;
		FMadBlockRegistry Second;
		TArray<FMadDefinitionError> Errors;

		MadFallTests::BuildRegistryFromJson(First, OrderDirectory.Path, Files, FName(TEXT("testmod")), Errors);
		MadFallTests::BuildRegistryFromJson(Second, OrderDirectory.Path, {}, FName(TEXT("testmod")), Errors);

		TestEqual(TEXT("the same mod set produces the same id for alpha"),
			static_cast<int32>(First.ResolveRuntimeId(FName(TEXT("testmod:alpha")))),
			static_cast<int32>(Second.ResolveRuntimeId(FName(TEXT("testmod:alpha")))));
		TestEqual(TEXT("the same mod set produces the same id for zinc"),
			static_cast<int32>(First.ResolveRuntimeId(FName(TEXT("testmod:zinc")))),
			static_cast<int32>(Second.ResolveRuntimeId(FName(TEXT("testmod:zinc")))));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadUnresolvedBlockTest,
	"MadFall.Core.BlockRegistry.UnresolvedBlocks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadUnresolvedBlockTest::RunTest(const FString& Parameters)
{
	// Encountering a block from an uninstalled mod must warn - loudly enough
	// that a player can see why a wall turned grey, quietly enough that it is
	// not an error, because the world is fine.
	AddExpectedMessagePlain(TEXT("is not provided by any installed definition"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	FMadBlockRegistry Registry;
	TArray<FMadDefinitionError> Errors;
	Registry.FinishLoad(Errors);

	const FName Missing(TEXT("somemod:rebar_concrete"));

	const uint16 First = Registry.GetOrCreateUnresolvedId(Missing);
	const uint16 Second = Registry.GetOrCreateUnresolvedId(Missing);

	TestEqual(TEXT("the same missing id gets the same placeholder twice"),
		static_cast<int32>(First), static_cast<int32>(Second));
	TestNotEqual(TEXT("a placeholder is not air"), static_cast<int32>(First), 0);
	TestTrue(TEXT("the placeholder is recognised as unresolved"), Registry.IsUnresolvedId(First));

	// THE property the save format exists to guarantee: the original string
	// comes back, so the block can be written out unchanged and restored when
	// the mod is reinstalled.
	TestEqual(TEXT("the placeholder returns the ORIGINAL id, not a generic name"),
		Registry.GetStringId(First), Missing);

	const FMadBlockDefView View = Registry.GetBlockView(First);
	TestTrue(TEXT("the view is marked unresolved"), View.bUnresolved);
	TestEqual(TEXT("the view keeps the original id"), View.Id, Missing);
	TestTrue(TEXT("an unresolved block is an anchor so it cannot collapse a structure"), View.bIsAnchor);

	TestFalse(TEXT("an unresolved block is not 'registered'"), Registry.IsRegistered(Missing));
	TestEqual(TEXT("one unresolved id so far"), Registry.NumUnresolved(), 1);

	const uint16 Other = Registry.GetOrCreateUnresolvedId(FName(TEXT("othermod:thing")));
	TestNotEqual(TEXT("two different missing ids get different placeholders"),
		static_cast<int32>(First), static_cast<int32>(Other));
	TestEqual(TEXT("two unresolved ids"), Registry.NumUnresolved(), 2);

	// Placeholders grow down from the top so they can never collide with the
	// sequentially assigned registered ids.
	TestTrue(TEXT("placeholders are allocated near the top of the id space"),
		First > static_cast<uint16>(Registry.Num()));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

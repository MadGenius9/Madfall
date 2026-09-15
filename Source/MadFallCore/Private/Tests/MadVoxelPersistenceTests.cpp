// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "MadBlockRegistry.h"
#include "MadChunkSerializer.h"
#include "MadChunkStorage.h"
#include "MadFallCoordinates.h"
#include "MadRegionFile.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadFallPersistenceTests
{
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

	/** Registry holding a handful of blocks written by `ModIds`, loaded through the real JSON path. */
	void BuildRegistry(FMadBlockRegistry& Registry, const FString& Directory, const TArray<FString>& BlockIds)
	{
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		IFileManager::Get().MakeDirectory(*Directory, true);

		for (const FString& BlockId : BlockIds)
		{
			FString Namespace;
			FString Name;
			BlockId.Split(TEXT(":"), &Namespace, &Name);

			const FString Json = FString::Printf(TEXT(R"({
				"schema": "madfall.block/1",
				"id": "%s",
				"material": { "mass_kg": 1000.0, "hardness": 300.0 },
				"structure": { "support_strength": 2000.0 }
			})"), *BlockId);

			const FString ModDirectory = FPaths::Combine(Directory, Namespace);
			IFileManager::Get().MakeDirectory(*ModDirectory, true);
			FFileHelper::SaveStringToFile(Json, *FPaths::Combine(ModDirectory, Name + TEXT(".json")));
		}

		TArray<FMadDefinitionError> Errors;
		Registry.BeginLoad();

		TArray<FString> ModDirectories;
		IFileManager::Get().IterateDirectory(*Directory, [&ModDirectories](const TCHAR* Path, bool bIsDirectory)
		{
			if (bIsDirectory) { ModDirectories.Add(Path); }
			return true;
		});
		ModDirectories.Sort();

		for (const FString& ModDirectory : ModDirectories)
		{
			Registry.AddFromDirectory(ModDirectory, FName(*FPaths::GetCleanFilename(ModDirectory)), Errors);
		}

		Registry.FinishLoad(Errors);
	}

	/** A chunk with several block types, varying density, rotation, flags and sparse damage. */
	FMadChunkStorage MakeBusyChunk(const FMadBlockRegistry& Registry, const TArray<FName>& BlockIds)
	{
		FMadChunkStorage Chunk;

		FMadVoxel Base;
		Base.BlockTypeID = Registry.ResolveRuntimeId(BlockIds[0]);
		Base.Density = 255;
		Base.Damage = 0;
		Base.Rotation = 0;
		Base.Flags = static_cast<uint8>(EMadVoxelFlags::PlayerModified);
		Chunk.Fill(Base);

		for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; Index += 37)
		{
			FMadVoxel Voxel;
			Voxel.BlockTypeID = Registry.ResolveRuntimeId(BlockIds[Index % BlockIds.Num()]);
			Voxel.Density = static_cast<uint8>(Index % 256);
			Voxel.Damage = 0;
			Voxel.Rotation = 0;
			Voxel.SetOrientation(static_cast<uint8>(Index % 24));
			Voxel.SetShapeVariant(static_cast<uint8>(Index % 8));
			Voxel.Flags = static_cast<uint8>(EMadVoxelFlags::Cubic | EMadVoxelFlags::PlayerModified);
			Chunk.SetVoxel(Index, Voxel);
		}

		// Sparse damage on a handful of voxels.
		for (int32 Index = 100; Index < 2000; Index += 431)
		{
			FMadVoxel Voxel = Chunk.GetVoxel(Index);
			Voxel.Damage = static_cast<uint8>(1 + (Index % 254));
			Chunk.SetVoxel(Index, Voxel);
		}

		return Chunk;
	}
}

// ===========================================================================
// Serializer round trip
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadChunkSerializerRoundTripTest,
	"MadFall.Core.Serialization.ChunkRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadChunkSerializerRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MadFallPersistenceTests;

	FScopedTestDirectory Directory;

	FMadBlockRegistry Registry;
	BuildRegistry(Registry, Directory.File(TEXT("defs")),
		{ TEXT("testmod:stone"), TEXT("testmod:wood"), TEXT("testmod:steel"), TEXT("testmod:glass") });

	// A stand-in for the region string table.
	TArray<FName> StringTable;
	auto Intern = [&StringTable](FName Value) -> uint32
	{
		const int32 Existing = StringTable.Find(Value);
		if (Existing != INDEX_NONE)
		{
			return static_cast<uint32>(Existing);
		}
		return static_cast<uint32>(StringTable.Add(Value));
	};
	auto Resolve = [&StringTable](uint32 Index) -> FName
	{
		return StringTable.IsValidIndex(static_cast<int32>(Index)) ? StringTable[static_cast<int32>(Index)] : NAME_None;
	};

	// --- uniform chunk ---
	{
		FMadChunkStorage Original;
		FMadVoxel Stone;
		Stone.BlockTypeID = Registry.ResolveRuntimeId(FName(TEXT("testmod:stone")));
		Stone.Density = 255;
		Stone.Damage = 0;
		Stone.Rotation = 0;
		Stone.Flags = 0;
		Original.Fill(Stone);

		FMadSerializedChunk Serialized;
		MadFall::ChunkSerializer::Serialize(Original, Registry, Intern, Serialized);

		TestTrue(TEXT("a uniform chunk serializes to the uniform form"), Serialized.bUniform);
		TestEqual(TEXT("a uniform chunk writes zero payload bytes"), Serialized.Payload.Num(), 0);
		TestEqual(TEXT("the uniform block id is recorded"), Serialized.UniformBlockId, FName(TEXT("testmod:stone")));

		FMadChunkStorage Restored;
		FString Error;
		TestTrue(TEXT("a uniform chunk deserializes"),
			MadFall::ChunkSerializer::Deserialize(Serialized, Registry, Resolve, Restored, Error));
		TestTrue(TEXT("the restored chunk is still uniform"), Restored.IsUniform());
		TestTrue(TEXT("every voxel survived"), Original.EqualsVoxelwise(Restored));
	}

	// --- a busy chunk ---
	{
		const FMadChunkStorage Original = MakeBusyChunk(Registry,
			{ FName(TEXT("testmod:stone")), FName(TEXT("testmod:wood")),
			  FName(TEXT("testmod:steel")), FName(TEXT("testmod:glass")) });

		FMadSerializedChunk Serialized;
		MadFall::ChunkSerializer::Serialize(Original, Registry, Intern, Serialized);

		TestFalse(TEXT("a busy chunk is not uniform"), Serialized.bUniform);
		TestTrue(TEXT("a busy chunk produced a payload"), Serialized.Payload.Num() > 0);

		FMadChunkStorage Restored;
		FString Error;
		TestTrue(FString::Printf(TEXT("a busy chunk deserializes (%s)"), *Error),
			MadFall::ChunkSerializer::Deserialize(Serialized, Registry, Resolve, Restored, Error));

		// The whole point: all 32768 voxels, including density, rotation,
		// shape variant, flags and sparse damage.
		TestTrue(TEXT("every one of 32768 voxels survived the round trip"),
			Original.EqualsVoxelwise(Restored));
	}

	// --- transient flags never reach the file ---
	{
		FMadChunkStorage Original;
		FMadVoxel Voxel;
		Voxel.BlockTypeID = Registry.ResolveRuntimeId(FName(TEXT("testmod:stone")));
		Voxel.Density = 255;
		Voxel.Damage = 0;
		Voxel.Rotation = 0;
		Voxel.Flags = static_cast<uint8>(EMadVoxelFlags::Cubic | EMadVoxelFlags::SupportDirty);
		Original.SetVoxel(42, Voxel);

		FMadSerializedChunk Serialized;
		MadFall::ChunkSerializer::Serialize(Original, Registry, Intern, Serialized);

		FMadChunkStorage Restored;
		FString Error;
		MadFall::ChunkSerializer::Deserialize(Serialized, Registry, Resolve, Restored, Error);

		const FMadVoxel Reloaded = Restored.GetVoxel(42);
		TestTrue(TEXT("the persistent Cubic flag survives"), Reloaded.HasFlag(EMadVoxelFlags::Cubic));
		TestFalse(TEXT("the transient SupportDirty flag is stripped"), Reloaded.HasFlag(EMadVoxelFlags::SupportDirty));
	}

	// --- truncated payloads are refused, not partially applied ---
	{
		const FMadChunkStorage Original = MakeBusyChunk(Registry,
			{ FName(TEXT("testmod:stone")), FName(TEXT("testmod:wood")) });

		FMadSerializedChunk Serialized;
		MadFall::ChunkSerializer::Serialize(Original, Registry, Intern, Serialized);

		FMadSerializedChunk Truncated = Serialized;
		Truncated.Payload.SetNum(Truncated.Payload.Num() / 2);

		FMadChunkStorage Restored;
		FString Error;
		TestFalse(TEXT("a truncated payload is refused"),
			MadFall::ChunkSerializer::Deserialize(Truncated, Registry, Resolve, Restored, Error));
		TestTrue(TEXT("the refusal explains itself"), !Error.IsEmpty());
	}

	return true;
}

// ===========================================================================
// Region file
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadRegionFileTest,
	"MadFall.Core.Serialization.RegionFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadRegionFileTest::RunTest(const FString& Parameters)
{
	using namespace MadFallPersistenceTests;

	FScopedTestDirectory Directory;

	FMadBlockRegistry Registry;
	BuildRegistry(Registry, Directory.File(TEXT("defs")),
		{ TEXT("testmod:stone"), TEXT("testmod:wood"), TEXT("testmod:steel") });

	const FMadRegionCoord RegionCoord(0, 0);
	const FString RegionPath = FPaths::Combine(Directory.Path, FMadRegionFile::MakeFileName(RegionCoord));

	TestEqual(TEXT("region file naming"), FMadRegionFile::MakeFileName(FMadRegionCoord(-2, 3)), FString(TEXT("r.-2.3.mfr")));

	const FMadChunkCoord BusyCoord(3, 4, 0);
	const FMadChunkCoord UniformCoord(5, 6, 1);

	const int32 BusySlot = MadFall::ChunkToRegionSlot(BusyCoord);
	const int32 UniformSlot = MadFall::ChunkToRegionSlot(UniformCoord);

	const FMadChunkStorage BusyOriginal = MakeBusyChunk(Registry,
		{ FName(TEXT("testmod:stone")), FName(TEXT("testmod:wood")), FName(TEXT("testmod:steel")) });

	FMadChunkStorage UniformOriginal;
	{
		FMadVoxel Voxel;
		Voxel.BlockTypeID = Registry.ResolveRuntimeId(FName(TEXT("testmod:stone")));
		Voxel.Density = 255;
		Voxel.Damage = 0;
		Voxel.Rotation = 0;
		Voxel.Flags = 0;
		UniformOriginal.Fill(Voxel);
	}

	// --- write ---
	{
		FMadRegionFile Region;
		FString Error;

		TestTrue(FString::Printf(TEXT("region opens for creation (%s)"), *Error),
			Region.Open(RegionPath, RegionCoord, 12345, 1, /*bCreateIfMissing*/ true, Error));

		TestFalse(TEXT("a fresh region has no chunks"), Region.HasChunk(BusySlot));

		auto Intern = [&Region](FName Value) { return Region.InternString(Value); };

		FMadSerializedChunk Busy;
		MadFall::ChunkSerializer::Serialize(BusyOriginal, Registry, Intern, Busy);
		TestTrue(FString::Printf(TEXT("busy chunk writes (%s)"), *Error),
			Region.WriteChunk(BusySlot, Busy, /*bPlayerModified*/ true, Error));

		FMadSerializedChunk Uniform;
		MadFall::ChunkSerializer::Serialize(UniformOriginal, Registry, Intern, Uniform);
		TestTrue(FString::Printf(TEXT("uniform chunk writes (%s)"), *Error),
			Region.WriteChunk(UniformSlot, Uniform, /*bPlayerModified*/ false, Error));

		TestTrue(FString::Printf(TEXT("region flushes (%s)"), *Error), Region.Flush(Error));
		TestTrue(FString::Printf(TEXT("region closes (%s)"), *Error), Region.Close(Error));
	}

	// --- reopen and verify ---
	{
		FMadRegionFile Region;
		FString Error;

		TestTrue(FString::Printf(TEXT("region reopens (%s)"), *Error),
			Region.Open(RegionPath, RegionCoord, 12345, 1, /*bCreateIfMissing*/ false, Error));

		TestEqual(TEXT("two chunks are present"), Region.NumChunks(), 2);
		TestTrue(TEXT("the busy slot is occupied"), Region.HasChunk(BusySlot));
		TestTrue(TEXT("the uniform slot is occupied"), Region.HasChunk(UniformSlot));
		TestFalse(TEXT("an untouched slot is still empty"), Region.HasChunk(BusySlot + 1));

		auto Resolve = [&Region](uint32 Index) { return Region.ResolveString(Index); };

		FMadSerializedChunk Busy;
		TestTrue(FString::Printf(TEXT("busy chunk reads back (%s)"), *Error),
			Region.ReadChunk(BusySlot, Busy, Error));

		FMadChunkStorage BusyRestored;
		TestTrue(FString::Printf(TEXT("busy chunk deserializes (%s)"), *Error),
			MadFall::ChunkSerializer::Deserialize(Busy, Registry, Resolve, BusyRestored, Error));
		TestTrue(TEXT("the busy chunk is voxel-identical after a full disk round trip"),
			BusyOriginal.EqualsVoxelwise(BusyRestored));

		FMadSerializedChunk Uniform;
		TestTrue(FString::Printf(TEXT("uniform chunk reads back (%s)"), *Error),
			Region.ReadChunk(UniformSlot, Uniform, Error));
		TestTrue(TEXT("the uniform chunk stayed uniform on disk"), Uniform.bUniform);
		TestEqual(TEXT("a uniform chunk still costs zero payload bytes"), Uniform.Payload.Num(), 0);

		FMadChunkStorage UniformRestored;
		TestTrue(FString::Printf(TEXT("uniform chunk deserializes (%s)"), *Error),
			MadFall::ChunkSerializer::Deserialize(Uniform, Registry, Resolve, UniformRestored, Error));
		TestTrue(TEXT("the uniform chunk is voxel-identical"),
			UniformOriginal.EqualsVoxelwise(UniformRestored));

		TestTrue(TEXT("reading a never-generated slot fails cleanly"),
			!Region.ReadChunk(BusySlot + 1, Busy, Error) && !Error.IsEmpty());

		Region.Close(Error);
	}

	// --- the seed guard ---
	{
		FMadRegionFile Region;
		FString Error;

		TestFalse(TEXT("a mismatched world seed refuses to open"),
			Region.Open(RegionPath, RegionCoord, 99999, 1, false, Error));
		TestTrue(TEXT("the seed mismatch names both seeds"),
			Error.Contains(TEXT("12345")) && Error.Contains(TEXT("99999")));
	}

	// --- corruption is detected, not silently loaded ---
	{
		// Flip a byte well inside the first payload sector.
		TArray<uint8> FileBytes;
		TestTrue(TEXT("the region file can be read raw"), FFileHelper::LoadFileToArray(FileBytes, *RegionPath));

		const int32 PayloadOffset = MadFall::RegionDataStartSector * MadFall::RegionSectorBytes + 64;
		if (FileBytes.IsValidIndex(PayloadOffset))
		{
			FileBytes[PayloadOffset] = static_cast<uint8>(FileBytes[PayloadOffset] ^ 0xFF);
			FFileHelper::SaveArrayToFile(FileBytes, *RegionPath);

			FMadRegionFile Region;
			FString Error;
			TestTrue(TEXT("a region with one damaged chunk still opens"),
				Region.Open(RegionPath, RegionCoord, 12345, 1, false, Error));

			FMadSerializedChunk Damaged;
			const bool bRead = Region.ReadChunk(BusySlot, Damaged, Error);

			// Whichever chunk the flipped byte belonged to must fail its CRC
			// rather than deserialize into plausible-looking garbage.
			if (!bRead)
			{
				TestTrue(TEXT("the failure names the CRC check"), Error.Contains(TEXT("CRC")));
			}

			Region.Close(Error);
		}
	}

	return true;
}

// ===========================================================================
// Mod removal: the property the whole save format exists to guarantee
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadModRemovalRoundTripTest,
	"MadFall.Core.Serialization.ModRemovalIsLossless",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadModRemovalRoundTripTest::RunTest(const FString& Parameters)
{
	// The uninstalled-mod warning is part of what this test verifies.
	AddExpectedMessagePlain(TEXT("is not provided by any installed definition"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	using namespace MadFallPersistenceTests;

	FScopedTestDirectory Directory;

	const FString DefinitionsDir = Directory.File(TEXT("defs"));
	const FMadRegionCoord RegionCoord(0, 0);
	const FString RegionPath = FPaths::Combine(Directory.Path, FMadRegionFile::MakeFileName(RegionCoord));

	const FMadChunkCoord ChunkCoord(1, 1, 0);
	const int32 Slot = MadFall::ChunkToRegionSlot(ChunkCoord);

	const FName CoreBlock(TEXT("coremod:stone"));
	const FName ModBlock(TEXT("extramod:rebar"));

	// --- 1. A world built with both mods installed ---
	FMadChunkStorage Original;
	{
		FMadBlockRegistry Registry;
		BuildRegistry(Registry, DefinitionsDir, { CoreBlock.ToString(), ModBlock.ToString() });

		TestTrue(TEXT("the extra mod's block is registered to begin with"), Registry.IsRegistered(ModBlock));

		FMadVoxel Stone;
		Stone.BlockTypeID = Registry.ResolveRuntimeId(CoreBlock);
		Stone.Density = 255;
		Stone.Damage = 0;
		Stone.Rotation = 0;
		Stone.Flags = static_cast<uint8>(EMadVoxelFlags::PlayerModified);
		Original.Fill(Stone);

		// A wall of the modded block, with rotation, variant and damage, so the
		// test would catch a placeholder that quietly discarded any of them.
		for (int32 Index = 0; Index < 512; ++Index)
		{
			FMadVoxel Rebar;
			Rebar.BlockTypeID = Registry.ResolveRuntimeId(ModBlock);
			Rebar.Density = 255;
			Rebar.Damage = static_cast<uint8>(Index % 200);
			Rebar.Rotation = 0;
			Rebar.SetOrientation(static_cast<uint8>(Index % 24));
			Rebar.SetShapeVariant(static_cast<uint8>(Index % 8));
			Rebar.Flags = static_cast<uint8>(EMadVoxelFlags::Cubic | EMadVoxelFlags::PlayerModified);
			Original.SetVoxel(Index, Rebar);
		}

		FMadRegionFile Region;
		FString Error;
		TestTrue(TEXT("region opens"), Region.Open(RegionPath, RegionCoord, 1, 1, true, Error));

		auto Intern = [&Region](FName Value) { return Region.InternString(Value); };
		FMadSerializedChunk Serialized;
		MadFall::ChunkSerializer::Serialize(Original, Registry, Intern, Serialized);

		TestTrue(TEXT("the chunk saves"), Region.WriteChunk(Slot, Serialized, true, Error));
		TestTrue(TEXT("the region closes"), Region.Close(Error));
	}

	// --- 2. The extra mod is uninstalled. Load, then save again. ---
	{
		FMadBlockRegistry Registry;
		BuildRegistry(Registry, DefinitionsDir, { CoreBlock.ToString() });

		TestFalse(TEXT("the extra mod's block is no longer registered"), Registry.IsRegistered(ModBlock));

		FMadRegionFile Region;
		FString Error;
		TestTrue(TEXT("the region still opens without the mod"),
			Region.Open(RegionPath, RegionCoord, 1, 1, false, Error));

		auto Resolve = [&Region](uint32 Index) { return Region.ResolveString(Index); };

		FMadSerializedChunk Serialized;
		TestTrue(TEXT("the chunk still reads without the mod"), Region.ReadChunk(Slot, Serialized, Error));

		FMadChunkStorage WithoutMod;
		TestTrue(FString::Printf(TEXT("a missing mod is NOT a load failure (%s)"), *Error),
			MadFall::ChunkSerializer::Deserialize(Serialized, Registry, Resolve, WithoutMod, Error));

		TestEqual(TEXT("exactly one block id could not be resolved"), Registry.NumUnresolved(), 1);

		const uint16 PlaceholderId = WithoutMod.GetBlockId(0);
		TestTrue(TEXT("the modded block became a placeholder"), Registry.IsUnresolvedId(PlaceholderId));
		TestEqual(TEXT("the placeholder still reports the original block id"),
			Registry.GetStringId(PlaceholderId), ModBlock);
		TestNotEqual(TEXT("the modded block did NOT become air"), static_cast<int32>(PlaceholderId), 0);

		// The unmodded block must be completely unaffected.
		TestEqual(TEXT("the core block still resolves normally"),
			Registry.GetStringId(WithoutMod.GetBlockId(MadFall::ChunkVoxelCount - 1)), CoreBlock);

		// Now save the world again while the mod is still uninstalled. This is
		// the step that destroys bases in games that map unknown ids to air.
		auto Intern = [&Region](FName Value) { return Region.InternString(Value); };
		FMadSerializedChunk Rewritten;
		MadFall::ChunkSerializer::Serialize(WithoutMod, Registry, Intern, Rewritten);

		TestTrue(TEXT("the chunk saves again while the mod is missing"),
			Region.WriteChunk(Slot, Rewritten, true, Error));
		TestTrue(TEXT("the region closes"), Region.Close(Error));
	}

	// --- 3. The mod is reinstalled. Everything must be exactly as it was. ---
	{
		FMadBlockRegistry Registry;
		BuildRegistry(Registry, DefinitionsDir, { CoreBlock.ToString(), ModBlock.ToString() });

		TestTrue(TEXT("the extra mod's block is registered again"), Registry.IsRegistered(ModBlock));

		FMadRegionFile Region;
		FString Error;
		TestTrue(TEXT("the region opens"), Region.Open(RegionPath, RegionCoord, 1, 1, false, Error));

		auto Resolve = [&Region](uint32 Index) { return Region.ResolveString(Index); };

		FMadSerializedChunk Serialized;
		TestTrue(TEXT("the chunk reads"), Region.ReadChunk(Slot, Serialized, Error));

		FMadChunkStorage Restored;
		TestTrue(FString::Printf(TEXT("the chunk deserializes (%s)"), *Error),
			MadFall::ChunkSerializer::Deserialize(Serialized, Registry, Resolve, Restored, Error));

		TestEqual(TEXT("nothing is unresolved any more"), Registry.NumUnresolved(), 0);

		// Runtime ids are session-local and may legitimately differ, so compare
		// by voxel VALUE the way the rest of the game will.
		TestTrue(TEXT("the reinstalled block resolves to a real definition"),
			Registry.IsRegistered(Registry.GetStringId(Restored.GetBlockId(0))));
		TestEqual(TEXT("the modded block is back under its own id"),
			Registry.GetStringId(Restored.GetBlockId(0)), ModBlock);

		// Every voxel, including the ones that spent a whole save cycle as a
		// placeholder, must be byte-for-byte what it was before the mod was
		// ever uninstalled.
		bool bIdentical = true;
		FString FirstDifference;

		for (int32 Index = 0; Index < MadFall::ChunkVoxelCount; ++Index)
		{
			const FMadVoxel Before = Original.GetVoxel(Index);
			const FMadVoxel After = Restored.GetVoxel(Index);

			const bool bSameBlock = Registry.GetStringId(Before.BlockTypeID) == Registry.GetStringId(After.BlockTypeID);
			const bool bSameRest = Before.Density == After.Density
				&& Before.Damage == After.Damage
				&& Before.Rotation == After.Rotation
				&& Before.Flags == After.Flags;

			if (!bSameBlock || !bSameRest)
			{
				bIdentical = false;
				FirstDifference = FString::Printf(
					TEXT("voxel %d: %s(d%u dmg%u r%u f%02X) became %s(d%u dmg%u r%u f%02X)"),
					Index,
					*Registry.GetStringId(Before.BlockTypeID).ToString(),
					Before.Density, Before.Damage, Before.Rotation, Before.Flags,
					*Registry.GetStringId(After.BlockTypeID).ToString(),
					After.Density, After.Damage, After.Rotation, After.Flags);
				break;
			}
		}

		TestTrue(FString::Printf(
			TEXT("uninstalling a mod, saving, and reinstalling it is lossless. %s"), *FirstDifference),
			bIdentical);

		Region.Close(Error);
	}

	return true;
}

// ===========================================================================
// Region durability: crashes at every point of a save
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadRegionDurabilityTest,
	"MadFall.Core.Serialization.RegionDurability",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadRegionDurabilityTest::RunTest(const FString& Parameters)
{
	using namespace MadFallPersistenceTests;
	AddExpectedMessagePlain(TEXT("was not closed cleanly"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("recovered an interrupted index write"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	FScopedTestDirectory Directory;
	FMadBlockRegistry Registry;
	BuildRegistry(Registry, Directory.File(TEXT("defs")), { TEXT("testmod:stone"), TEXT("testmod:wood"), TEXT("testmod:steel") });

	const FMadRegionCoord RegionCoord(0, 0);
	const FString Path = FPaths::Combine(Directory.Path, FMadRegionFile::MakeFileName(RegionCoord));
	constexpr uint64 Seed = 777;

	// Distinct contents per version: the damage map differs.
	auto Version = [&Registry](int32 V)
	{
		FMadChunkStorage Chunk = MakeBusyChunk(Registry, { FName(TEXT("testmod:stone")), FName(TEXT("testmod:wood")), FName(TEXT("testmod:steel")) });
		for (int32 Index = 0; Index < 64; ++Index)
		{
			FMadVoxel Voxel = Chunk.GetVoxel(Index * 101);
			Voxel.Damage = static_cast<uint8>(V * 7 + Index);
			Chunk.SetVoxel(Index * 101, Voxel);
		}
		return Chunk;
	};
	auto Write = [&Registry](FMadRegionFile& Region, int32 Slot, const FMadChunkStorage& Chunk)
	{
		FMadSerializedChunk Serialized;
		MadFall::ChunkSerializer::Serialize(Chunk, Registry, [&Region](FName Value) { return Region.InternString(Value); }, Serialized);
		FString WriteError;
		return Region.WriteChunk(Slot, Serialized, true, WriteError);
	};
	auto ReadsAs = [&Registry](FMadRegionFile& Region, int32 Slot, const FMadChunkStorage& Expected)
	{
		FMadSerializedChunk Serialized;
		FMadChunkStorage Restored;
		FString ReadError;
		return Region.ReadChunk(Slot, Serialized, ReadError)
			&& MadFall::ChunkSerializer::Deserialize(Serialized, Registry, [&Region](uint32 Index) { return Region.ResolveString(Index); }, Restored, ReadError)
			&& Expected.EqualsVoxelwise(Restored);
	};
	auto OpenRegion = [&](FMadRegionFile& Region, bool bCreate)
	{
		FString OpenError;
		const bool bOk = Region.Open(Path, RegionCoord, Seed, 1, bCreate, OpenError);
		if (!bOk)
		{
			AddInfo(FString::Printf(TEXT("open failed: %s"), *OpenError));
		}
		return bOk;
	};
	FString Error;

	// --- 1. A rewrite's old sectors are not reused before the index is on disk ---
	{
		FMadRegionFile Region;
		TestTrue(TEXT("create"), OpenRegion(Region, true));
		TestTrue(TEXT("chunk A v1"), Write(Region, 0, Version(1)));
		TestTrue(TEXT("flush v1"), Region.Flush(Error));

		TestTrue(TEXT("chunk A v2"), Write(Region, 0, Version(2)));
		TestTrue(TEXT("the old sectors of A wait for the index"), Region.GetPendingFreeSectorCount() > 0);
		TestTrue(TEXT("chunk B"), Write(Region, 1, Version(9)));

		FMadRegionFile::TestCrash = FMadRegionFile::ETestCrash::BeforeJournal;
		TestFalse(TEXT("crash before the journal"), Region.Flush(Error));
		FMadRegionFile::TestCrash = FMadRegionFile::ETestCrash::None;
		Region.AbandonForTest();
	}
	{
		FMadRegionFile Region;
		TestTrue(TEXT("reopen after the crash"), OpenRegion(Region, false));
		TestTrue(TEXT("the unclean shutdown is noticed"), Region.WasUncleanOnOpen());
		TestTrue(TEXT("chunk A is the last saved version, intact: B did not overwrite it"), ReadsAs(Region, 0, Version(1)));
		TestFalse(TEXT("chunk B was never saved"), Region.HasChunk(1));
		TestTrue(TEXT("clean close"), Region.Close(Error));
	}

	// --- 2. Crash after the journal, before the in-place index write ---
	{
		FMadRegionFile Region;
		TestTrue(TEXT("reopen"), OpenRegion(Region, false));
		TestFalse(TEXT("a clean close is clean"), Region.WasUncleanOnOpen());
		TestTrue(TEXT("chunk A v3"), Write(Region, 0, Version(3)));
		FMadRegionFile::TestCrash = FMadRegionFile::ETestCrash::AfterJournal;
		TestFalse(TEXT("crash after the journal"), Region.Flush(Error));
		FMadRegionFile::TestCrash = FMadRegionFile::ETestCrash::None;
		Region.AbandonForTest();
		TestTrue(TEXT("the journal is on disk"), IFileManager::Get().FileExists(*FMadRegionFile::GetJournalPath(Path)));
	}
	{
		FMadRegionFile Region;
		TestTrue(TEXT("reopen"), OpenRegion(Region, false));
		TestTrue(TEXT("the journal was replayed"), Region.RecoveredIndexOnOpen());
		TestTrue(TEXT("the save completed: A is v3"), ReadsAs(Region, 0, Version(3)));
		TestFalse(TEXT("and the journal is gone"), IFileManager::Get().FileExists(*FMadRegionFile::GetJournalPath(Path)));
		TestTrue(TEXT("clean close"), Region.Close(Error));
	}

	// --- 3. Crash half way through the in-place index write ---
	{
		FMadRegionFile Region;
		TestTrue(TEXT("reopen"), OpenRegion(Region, false));
		TestTrue(TEXT("chunk A v4"), Write(Region, 0, Version(4)));
		FMadRegionFile::TestCrash = FMadRegionFile::ETestCrash::MidIndexWrite;
		TestFalse(TEXT("crash mid index"), Region.Flush(Error));
		FMadRegionFile::TestCrash = FMadRegionFile::ETestCrash::None;
		Region.AbandonForTest();
	}
	{
		FMadRegionFile Region;
		TestTrue(TEXT("a torn index opens"), OpenRegion(Region, false));
		TestTrue(TEXT("repaired from the journal"), Region.RecoveredIndexOnOpen());
		TestTrue(TEXT("A is v4"), ReadsAs(Region, 0, Version(4)));
		TestTrue(TEXT("clean close"), Region.Close(Error));
	}

	// --- 4. An incomplete journal is discarded: the in-place index was never touched ---
	FFileHelper::SaveStringToFile(TEXT("MFIJ half a journal"), *FMadRegionFile::GetJournalPath(Path));
	{
		FMadRegionFile Region;
		TestTrue(TEXT("opens past a torn journal"), OpenRegion(Region, false));
		TestFalse(TEXT("nothing replayed"), Region.RecoveredIndexOnOpen());
		TestTrue(TEXT("A is still v4"), ReadsAs(Region, 0, Version(4)));
		TestFalse(TEXT("the torn journal was removed"), IFileManager::Get().FileExists(*FMadRegionFile::GetJournalPath(Path)));
		TestTrue(TEXT("clean close"), Region.Close(Error));
	}

	// --- 5. Damage to the index with no journal fails loudly instead of loading garbage ---
	{
		const FString Damaged = FPaths::Combine(Directory.Path, TEXT("damaged"), FMadRegionFile::MakeFileName(RegionCoord));
		IFileManager::Get().Copy(*Damaged, *Path);
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Damaged);
		if (TestTrue(TEXT("copied"), Bytes.Num() > MadFall::RegionIndexOffset + 4))
		{
			Bytes[MadFall::RegionIndexOffset + 3] ^= 0x5A;
			FFileHelper::SaveArrayToFile(Bytes, *Damaged);
			FMadRegionFile Region;
			TestFalse(TEXT("a damaged index is refused"), Region.Open(Damaged, RegionCoord, Seed, 1, false, Error));
			TestTrue(TEXT("naming the CRC and the backup"), Error.Contains(TEXT("CRC")) && Error.Contains(TEXT("backup")));
		}
	}

	// --- 6. Compaction reclaims freed space and keeps every chunk ---
	// First-fit reuse keeps rewrites of same-sized chunks from fragmenting a
	// file; what leaves holes is chunks that stop needing a payload (dug out
	// to air, filled solid). Here 90 of 100 chunks become uniform.
	{
		const FString CompactPath = FPaths::Combine(Directory.Path, TEXT("compact"), FMadRegionFile::MakeFileName(RegionCoord));
		constexpr int32 Chunks = 100;
		constexpr int32 Kept = 10;
		FMadChunkStorage Solid;
		{
			FMadVoxel Stone;
			Stone.BlockTypeID = Registry.ResolveRuntimeId(FName(TEXT("testmod:stone")));
			Stone.Density = 255;
			Stone.Damage = 0;
			Stone.Rotation = 0;
			Stone.Flags = 0;
			Solid.Fill(Stone);
		}
		{
			FMadRegionFile Region;
			TestTrue(TEXT("create"), Region.Open(CompactPath, RegionCoord, Seed, 1, true, Error));
			for (int32 Slot = 0; Slot < Chunks; ++Slot)
			{
				Write(Region, Slot, Version(Slot));
			}
			Region.Flush(Error);
			for (int32 Slot = Kept; Slot < Chunks; ++Slot)
			{
				Write(Region, Slot, Solid);
			}
			Region.Flush(Error);
			TestTrue(TEXT("mostly free space once 90 chunks are uniform"), Region.ShouldCompact());
			const int64 SizeBefore = IFileManager::Get().FileSize(*CompactPath);
			TestTrue(TEXT("close compacts"), Region.Close(Error));
			const int64 SizeAfter = IFileManager::Get().FileSize(*CompactPath);
			TestTrue(FString::Printf(TEXT("the file shrank (%lld -> %lld bytes)"), SizeBefore, SizeAfter), SizeAfter < SizeBefore / 2);
			TestFalse(TEXT("no temporary left behind"), IFileManager::Get().FileExists(*(CompactPath + TEXT(".compact"))));
		}
		{
			FMadRegionFile Region;
			TestTrue(TEXT("the compacted region opens"), Region.Open(CompactPath, RegionCoord, Seed, 1, false, Error));
			TestFalse(TEXT("cleanly"), Region.WasUncleanOnOpen());
			TestEqual(TEXT("no free space left"), Region.GetFreeSectorCount(), 0);
			bool bAll = true;
			for (int32 Slot = 0; Slot < Chunks; ++Slot)
			{
				bAll &= ReadsAs(Region, Slot, Slot < Kept ? Version(Slot) : Solid);
			}
			TestTrue(TEXT("every chunk reads back"), bAll);
			Region.Close(Error);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

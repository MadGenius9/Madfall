// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadFallApiVersion.h"
#include "MadFallVoxelTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * FMadVoxel is an on-disk-adjacent type: its field order and widths are baked
 * into the save format and into the network delta encoder. The static_asserts
 * in the header catch a layout change at compile time; this test covers the
 * behaviour that asserts cannot - bit packing, flag masking, and the boundary
 * values of the rotation field.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadVoxelLayoutTest,
	"MadFall.Core.Voxel.Layout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadVoxelLayoutTest::RunTest(const FString& Parameters)
{
	// --- size and packing --------------------------------------------------
	TestEqual(TEXT("FMadVoxel is 6 bytes"), static_cast<int32>(sizeof(FMadVoxel)), 6);
	TestEqual(TEXT("FMadVoxel alignment is 1"), static_cast<int32>(alignof(FMadVoxel)), 1);
	TestEqual(TEXT("A dense array of 4 voxels is 24 bytes"), static_cast<int32>(sizeof(FMadVoxel[4])), 24);

	// --- chunk geometry ----------------------------------------------------
	TestEqual(TEXT("Chunk holds 32768 voxels"), MadFall::ChunkVoxelCount, 32768);
	TestEqual(TEXT("World has 16 chunk layers"), MadFall::WorldChunkLayers, 16);
	TestEqual(TEXT("Lowest chunk Z is -4"), MadFall::WorldMinChunkZ, -4);
	TestEqual(TEXT("Highest chunk Z is 11"), MadFall::WorldMaxChunkZ, 11);
	TestEqual(TEXT("Region holds 4096 chunk slots"), MadFall::RegionChunkSlots, 4096);

	// --- voxel index round trip, X fastest ---------------------------------
	TestEqual(TEXT("Index of (0,0,0)"), MadFall::VoxelIndex(0, 0, 0), 0);
	TestEqual(TEXT("Index of (1,0,0) - X is fastest"), MadFall::VoxelIndex(1, 0, 0), 1);
	TestEqual(TEXT("Index of (0,1,0)"), MadFall::VoxelIndex(0, 1, 0), 32);
	TestEqual(TEXT("Index of (0,0,1)"), MadFall::VoxelIndex(0, 0, 1), 1024);
	TestEqual(TEXT("Index of (31,31,31)"), MadFall::VoxelIndex(31, 31, 31), 32767);

	for (int32 Index : { 0, 1, 32, 1024, 12345, 32767 })
	{
		int32 X = 0, Y = 0, Z = 0;
		MadFall::VoxelCoords(Index, X, Y, Z);
		TestEqual(FString::Printf(TEXT("VoxelCoords round trip for %d"), Index),
			MadFall::VoxelIndex(X, Y, Z), Index);
	}

	// --- defaults ----------------------------------------------------------
	const FMadVoxel Air = FMadVoxel::Air();
	TestTrue(TEXT("Air voxel reports air"), Air.IsAir());
	TestFalse(TEXT("Air voxel is not solid"), Air.IsSolid());
	TestEqual(TEXT("Air block type is 0"), static_cast<int32>(Air.BlockTypeID), 0);

	// --- density threshold -------------------------------------------------
	FMadVoxel V = FMadVoxel::Air();
	V.Density = 127;
	TestFalse(TEXT("Density 127 is outside the surface"), V.IsSolid());
	V.Density = 128;
	TestTrue(TEXT("Density 128 is the surface crossing and counts as solid"), V.IsSolid());

	// --- rotation bit packing ---------------------------------------------
	// Orientation occupies bits 0-4, shape variant bits 5-7. They must not
	// bleed into each other: a modular beam rotated in place must keep its
	// corner/tee variant, and re-shaping must keep its facing.
	V.Rotation = 0;
	V.SetOrientation(23);
	V.SetShapeVariant(7);
	TestEqual(TEXT("Orientation survives shape variant write"), static_cast<int32>(V.GetOrientation()), 23);
	TestEqual(TEXT("Shape variant survives orientation write"), static_cast<int32>(V.GetShapeVariant()), 7);
	// 0b111_10111: variant 7 in bits 5-7, orientation 23 in bits 0-4. Not 0xFF -
	// 0xFF would mean orientation 31, which is a reserved encoding, not a valid
	// rotation. Getting this wrong is how a rotation table read walks off the end.
	TestEqual(TEXT("Packed rotation byte is 0xF7"), static_cast<int32>(V.Rotation), 0xF7);

	V.SetOrientation(0);
	TestEqual(TEXT("Clearing orientation leaves shape variant intact"), static_cast<int32>(V.GetShapeVariant()), 7);
	TestEqual(TEXT("Orientation is cleared"), static_cast<int32>(V.GetOrientation()), 0);

	// Reserved orientation encodings 24-31 must read back as 0 rather than as
	// an out-of-range index that would walk off the rotation matrix table.
	V.Rotation = 31;
	TestEqual(TEXT("Reserved orientation 31 reads back as 0"), static_cast<int32>(V.GetOrientation()), 0);

	// --- flags -------------------------------------------------------------
	V.Flags = 0;
	V.SetFlag(EMadVoxelFlags::Cubic, true);
	V.SetFlag(EMadVoxelFlags::PlayerModified, true);
	TestTrue(TEXT("Cubic flag set"), V.HasFlag(EMadVoxelFlags::Cubic));
	TestTrue(TEXT("PlayerModified flag set"), V.HasFlag(EMadVoxelFlags::PlayerModified));
	TestFalse(TEXT("Anchor flag untouched"), V.HasFlag(EMadVoxelFlags::Anchor));

	V.SetFlag(EMadVoxelFlags::Cubic, false);
	TestFalse(TEXT("Cubic flag cleared"), V.HasFlag(EMadVoxelFlags::Cubic));
	TestTrue(TEXT("Clearing one flag leaves the others"), V.HasFlag(EMadVoxelFlags::PlayerModified));

	// The transient mask is what the serializer ANDs off. If a new runtime-only
	// flag is added without updating the mask, dirty state leaks into save files.
	V.SetFlag(EMadVoxelFlags::SupportDirty, true);
	const uint8 Persisted = static_cast<uint8>(V.Flags & ~MadFall::TransientVoxelFlagMask);
	TestEqual(TEXT("SupportDirty is stripped by the transient mask"),
		static_cast<int32>(Persisted & static_cast<uint8>(EMadVoxelFlags::SupportDirty)), 0);
	TestEqual(TEXT("Persistent flags survive the transient mask"),
		static_cast<int32>(Persisted & static_cast<uint8>(EMadVoxelFlags::PlayerModified)),
		static_cast<int32>(EMadVoxelFlags::PlayerModified));

	// --- equality ----------------------------------------------------------
	const FMadVoxel A{ 42, 200, 10, 5, 1 };
	FMadVoxel B = A;
	TestTrue(TEXT("Identical voxels compare equal"), A == B);
	B.Damage = 11;
	TestTrue(TEXT("Differing damage compares unequal"), A != B);

	return true;
}

/** The mod-facing version string is parsed by the loader and shown to users. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadFallApiVersionTest,
	"MadFall.ModAPI.Version",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadFallApiVersionTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Version string is major.minor.patch"),
		MadFall::ModApi::GetVersionString(),
		FString::Printf(TEXT("%d.%d.%d"),
			MadFall::ModApi::VersionMajor,
			MadFall::ModApi::VersionMinor,
			MadFall::ModApi::VersionPatch));

	TestTrue(TEXT("Running version is compatible with itself"),
		MadFall::ModApi::IsCompatible(MadFall::ModApi::VersionMajor, MadFall::ModApi::VersionMinor));

	TestFalse(TEXT("A different major version is rejected"),
		MadFall::ModApi::IsCompatible(MadFall::ModApi::VersionMajor + 1, MadFall::ModApi::VersionMinor));

	// Pre-1.0 the minor version is allowed to break, so an older minor must not
	// be silently accepted - a mod built against 0.1 should refuse to load on 0.2.
	if (MadFall::ModApi::VersionMajor == 0)
	{
		TestFalse(TEXT("Pre-1.0 requires an exact minor match"),
			MadFall::ModApi::IsCompatible(0, MadFall::ModApi::VersionMinor + 1));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

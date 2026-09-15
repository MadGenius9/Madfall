// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadBlockDefinitionJson.h"
#include "MadBlockRegistry.h"
#include "MadTraps.h"
#include "MadVoxelWorldSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadTrapTest,
	"MadFall.AI.Traps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadTrapTest::RunTest(const FString& Parameters)
{
	// Parsing, and the mistake of a trap nothing can walk into.
	{
		TArray<FMadBlockDefinitionData> Parsed;
		TArray<FMadDefinitionError> Errors;
		MadFall::BlockDefinitionJson::ParseText(TEXT(R"([
			{ "schema": "madfall.block/1", "id": "test:spikes", "shape": { "kind": "model", "collision": "none" },
			  "trap": { "damage": 10, "seconds": 0.5, "wear": 4, "slow": 0.5 } },
			{ "schema": "madfall.block/1", "id": "test:wall_spikes", "shape": { "kind": "model", "collision": "box" },
			  "trap": { "damage": 10, "slow": 2 } }
		])"), TEXT("t"), FName(TEXT("test")), Parsed, Errors);
		if (TestEqual(TEXT("both parse"), Parsed.Num(), 2))
		{
			TestTrue(TEXT("trap stats"), Parsed[0].HasTrap() && Parsed[0].TrapDamage == 10.0f && Parsed[0].TrapSeconds == 0.5f && Parsed[0].TrapSlow == 0.5f);
			TestTrue(TEXT("a slow above 1 is clamped"), Parsed[1].TrapSlow <= 1.0f);
		}
		TestTrue(TEXT("a solid trap is reported"), Errors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.ToString().Contains(TEXT("collision")); }));
	}

	// Shipped: every trap is walk-through, and walk-through blocks read as open to walkers.
	const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
	int32 Traps = 0;
	for (const FMadBlockEntry& Entry : Blocks.GetEntries())
	{
		if (!Entry.Definition.HasTrap())
		{
			continue;
		}
		++Traps;
		TestTrue(*FString::Printf(TEXT("%s is walk-through"), *Entry.Definition.Id.ToString()), MadFall::Traps::IsWalkThrough(Entry.RuntimeId));
		FMadVoxel Voxel;
		Voxel.BlockTypeID = Entry.RuntimeId;
		Voxel.Density = 255;
		Voxel.Damage = 0;
		Voxel.Rotation = 0;
		Voxel.Flags = 0;
		TestFalse(*FString::Printf(TEXT("%s is open space to the pathfinder"), *Entry.Definition.Id.ToString()), MadFall::Traps::ForPathing(Voxel).IsSolid());
	}
	TestTrue(TEXT("spikes and wire ship"), Traps >= 3);

	const uint16 Stone = Blocks.ResolveRuntimeId(FName(TEXT("madfall:stone")));
	const uint16 OpenDoor = Blocks.ResolveRuntimeId(FName(TEXT("madfall:wood_door_open")));
	const uint16 ClosedDoor = Blocks.ResolveRuntimeId(FName(TEXT("madfall:wood_door")));
	TestFalse(TEXT("stone is not walk-through"), MadFall::Traps::IsWalkThrough(Stone));
	TestTrue(TEXT("an open door is walk-through"), MadFall::Traps::IsWalkThrough(OpenDoor));
	TestFalse(TEXT("a closed door is not"), MadFall::Traps::IsWalkThrough(ClosedDoor));
	return true;
}

#endif

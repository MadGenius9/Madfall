// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "MadFallCoordinates.h"
#include "MadGameplaySave.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadSaveTests
{
	FMadGameplaySave MakeSave()
	{
		FMadGameplaySave Save;
		Save.WorldSeed = 0x1234567890ABCDEFll;
		Save.Day = 12;
		Save.TimeOfDay = 21.5f;

		Save.bHasPlayer = true;
		FMadPlayerSaveData& P = Save.Player;
		P.Location = FVector(1234.5, -987.25, 2290.125);
		P.ViewRotation = FRotator(-12.5, 270.0, 0.0);
		P.SpawnColumn = FIntPoint(-40, 17);
		P.Stats.Health = 61.5f;
		P.Stats.Food = 42.0f;
		P.Stats.Water = 13.25f;
		P.Stats.CoreTemperature = 36.4f;
		P.Stats.Infection = 22.0f;
		P.Level = 7;
		P.Experience = 345;
		P.PerkRanks.Add(FName(TEXT("madfall:miner")), 2);
		P.PerkRanks.Add(FName(TEXT("gonemod:telekinesis")), 1);
		P.SelectedSlot = 4;

		P.Inventory.SetNum(36);
		P.Inventory[0] = FMadItemStack{ FName(TEXT("madfall:wood_plank")), 150, -1, {} };
		P.Inventory[4] = FMadItemStack{ FName(TEXT("madfall:iron_pickaxe")), 1, 431, { FName(TEXT("madfall:mod_reinforced_grip")) } };
		// An item from a mod that is not installed: must survive untouched.
		P.Inventory[20] = FMadItemStack{ FName(TEXT("gonemod:laser_drill")), 1, 77, {} };

		FMadContainerSaveData& Crate = Save.Containers.AddDefaulted_GetRef();
		Crate.Position = FIntVector(-100, 250, 31);
		Crate.LootTable = FName(TEXT("madfall:loot/ammo_crate"));
		Crate.Tier = 4;
		Crate.bRolled = true;
		Crate.Contents.SetNum(16);
		Crate.Contents[3] = FMadItemStack{ FName(TEXT("madfall:scrap_iron")), 6, -1, {} };

		Save.SleeperDays.Add(FIntVector(5, 6, 7), 11);
		Save.SleeperDays.Add(FIntVector(-5, 60, 2), 12);

		FMadPickupSaveData& Backpack = Save.Pickups.AddDefaulted_GetRef();
		Backpack.Location = FVector(10.0, 20.5, 2230.0);
		Backpack.bIsBackpack = true;
		Backpack.Lifetime = 3000.0f;
		Backpack.Stacks = { FMadItemStack{ FName(TEXT("madfall:rock")), 12, -1, {} } };

		TMap<FString, FMadScriptValue>& Store = Save.ModStore.Add(FName(TEXT("example_scripted")));
		Store.Add(TEXT("kills"), FMadScriptValue::MakeNumber(25.0));
		Store.Add(TEXT("pi"), FMadScriptValue::MakeNumber(3.25));
		Store.Add(TEXT("pending"), FMadScriptValue::MakeBool(true));
		Store.Add(TEXT("name"), FMadScriptValue::MakeString(TEXT("Night \"Watch\"")));
		return Save;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadGameplaySaveRoundTripTest,
	"MadFall.Save.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadGameplaySaveRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MadSaveTests;
	const FMadGameplaySave Original = MakeSave();

	const FString Json = MadFall::GameplaySave::ToJson(Original);
	FMadGameplaySave Loaded;
	TArray<FString> Warnings;
	TestTrue(TEXT("parses"), MadFall::GameplaySave::FromJson(Json, Loaded, Warnings));
	TestEqual(TEXT("no warnings"), Warnings.Num(), 0);

	TestEqual(TEXT("64-bit seed exact"), Loaded.WorldSeed, Original.WorldSeed);
	TestEqual(TEXT("day"), Loaded.Day, 12);
	TestEqual(TEXT("time"), Loaded.TimeOfDay, 21.5f, 1.0e-4f);

	TestTrue(TEXT("has player"), Loaded.bHasPlayer);
	TestTrue(TEXT("exact location"), Loaded.Player.Location.Equals(Original.Player.Location, 1.0e-6));
	TestTrue(TEXT("a location in the world is restored"), Loaded.Player.bHasLocation);

	// A location outside the world (a survivor once pushed 10 km up by the sky
	// dome's collision) starts the player at the spawn column, keeping the rest.
	{
		FMadGameplaySave Stranded = Original;
		Stranded.Player.Location = FVector(20522.0, 20565.0, 999670.0);
		FMadGameplaySave Reloaded;
		TArray<FString> StrandedWarnings;
		TestTrue(TEXT("stranded save parses"), MadFall::GameplaySave::FromJson(MadFall::GameplaySave::ToJson(Stranded), Reloaded, StrandedWarnings));
		TestFalse(TEXT("a location 10 km up is not restored"), Reloaded.Player.bHasLocation);
		TestTrue(TEXT("and says so"), StrandedWarnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("outside the world")); }));
		TestEqual(TEXT("the stranded player keeps their level"), Reloaded.Player.Level, 7);
	}
	TestFalse(TEXT("below bedrock is not restorable"), MadFall::GameplaySave::IsRestorableLocation(FVector(0.0, 0.0, (MadFall::WorldMinZ - 2) * MadFall::VoxelSizeUU)));
	TestTrue(TEXT("standing on the build ceiling is restorable"), MadFall::GameplaySave::IsRestorableLocation(FVector(0.0, 0.0, (MadFall::WorldMaxZ + 1) * MadFall::VoxelSizeUU)));
	TestFalse(TEXT("NaN is not restorable"), MadFall::GameplaySave::IsRestorableLocation(FVector(NAN, 0.0, 0.0)));
	TestTrue(TEXT("view"), Loaded.Player.ViewRotation.Equals(Original.Player.ViewRotation, 1.0e-4f));
	TestEqual(TEXT("spawn column"), Loaded.Player.SpawnColumn, Original.Player.SpawnColumn);
	TestEqual(TEXT("health"), Loaded.Player.Stats.Health, 61.5f, 1.0e-4f);
	TestEqual(TEXT("water"), Loaded.Player.Stats.Water, 13.25f, 1.0e-4f);
	TestEqual(TEXT("infection"), Loaded.Player.Stats.Infection, 22.0f, 1.0e-4f);
	TestEqual(TEXT("level"), Loaded.Player.Level, 7);
	TestEqual(TEXT("xp"), Loaded.Player.Experience, 345);
	TestTrue(TEXT("perk ranks, including an unknown mod's perk"), Loaded.Player.PerkRanks.OrderIndependentCompareEqual(Original.Player.PerkRanks));
	TestEqual(TEXT("selected slot"), Loaded.Player.SelectedSlot, 4);

	TestEqual(TEXT("all 36 slots come back, empty ones included"), Loaded.Player.Inventory.Num(), 36);
	TestTrue(TEXT("inventory identical slot for slot"), Loaded.Player.Inventory == Original.Player.Inventory);
	TestEqual(TEXT("unknown mod item kept verbatim"), Loaded.Player.Inventory[20].Item, FName(TEXT("gonemod:laser_drill")));

	TestEqual(TEXT("one container"), Loaded.Containers.Num(), 1);
	if (Loaded.Containers.Num() == 1)
	{
		const FMadContainerSaveData& C = Loaded.Containers[0];
		TestEqual(TEXT("container position"), C.Position, FIntVector(-100, 250, 31));
		TestEqual(TEXT("container table"), C.LootTable, FName(TEXT("madfall:loot/ammo_crate")));
		TestTrue(TEXT("rolled flag"), C.bRolled);
		TestTrue(TEXT("contents identical"), C.Contents == Original.Containers[0].Contents);
	}
	TestTrue(TEXT("sleeper days identical"), Loaded.SleeperDays.OrderIndependentCompareEqual(Original.SleeperDays));
	TestTrue(TEXT("script store identical"), Loaded.ModStore.Num() == 1
		&& Loaded.ModStore.FindRef(FName(TEXT("example_scripted"))).OrderIndependentCompareEqual(Original.ModStore.FindRef(FName(TEXT("example_scripted")))));
	TestEqual(TEXT("one pickup"), Loaded.Pickups.Num(), 1);
	if (Loaded.Pickups.Num() == 1)
	{
		TestTrue(TEXT("backpack flag"), Loaded.Pickups[0].bIsBackpack);
		TestTrue(TEXT("backpack contents"), Loaded.Pickups[0].Stacks == Original.Pickups[0].Stacks);
		TestTrue(TEXT("backpack location"), Loaded.Pickups[0].Location.Equals(Original.Pickups[0].Location, 1.0e-6));
	}

	TestEqual(TEXT("saving the loaded save is byte-identical"), MadFall::GameplaySave::ToJson(Loaded), Json);

	// --- rejection and damage tolerance -----------------------------------------
	{
		FMadGameplaySave Bad;
		TArray<FString> BadWarnings;
		TestFalse(TEXT("not JSON"), MadFall::GameplaySave::FromJson(TEXT("{ nope"), Bad, BadWarnings));
		TestFalse(TEXT("wrong schema"), MadFall::GameplaySave::FromJson(TEXT(R"({"schema":"madfall.save/99"})"), Bad, BadWarnings));

		// One broken inventory slot costs that slot, not the save.
		// The plank stack is the only 150 in the file: make its count a word.
		FString Damaged = Json.Replace(TEXT("150"), TEXT("\"lots\""));
		BadWarnings.Reset();
		TestTrue(TEXT("damaged slot still loads"), MadFall::GameplaySave::FromJson(Damaged, Bad, BadWarnings));
		TestTrue(TEXT("damaged slot reported"), BadWarnings.Num() > 0);
		TestTrue(TEXT("damaged slot emptied"), Bad.Player.Inventory[0].IsEmpty());
		TestEqual(TEXT("the rest survives"), Bad.Player.Inventory[4].Durability, 431);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadGameplaySaveFileTest,
	"MadFall.Save.AtomicFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadGameplaySaveFileTest::RunTest(const FString& Parameters)
{
	using namespace MadSaveTests;

	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("SaveTest_") + FGuid::NewGuid().ToString());
	const FString Path = FPaths::Combine(Dir, TEXT("gameplay.json"));
	FString Error;

	FMadGameplaySave First = MakeSave();
	First.Day = 3;
	FMadGameplaySave Second = MakeSave();
	Second.Day = 4;

	TestTrue(TEXT("first write"), MadFall::GameplaySave::WriteFile(Path, First, Error));
	TestTrue(TEXT("second write"), MadFall::GameplaySave::WriteFile(Path, Second, Error));
	TestTrue(TEXT("backup kept"), IFileManager::Get().FileExists(*(Path + TEXT(".bak"))));
	TestFalse(TEXT("no temp file left behind"), IFileManager::Get().FileExists(*(Path + TEXT(".tmp"))));

	FMadGameplaySave Read;
	TArray<FString> Warnings;
	bool bUsedBackup = true;
	TestTrue(TEXT("reads the latest"), MadFall::GameplaySave::ReadFile(Path, Read, Warnings, bUsedBackup));
	TestEqual(TEXT("latest day"), Read.Day, 4);
	TestFalse(TEXT("did not need the backup"), bUsedBackup);

	// Simulate a crash that truncated the live file.
	FFileHelper::SaveStringToFile(TEXT("{\"schema\": \"madfall.sa"), *Path);
	Warnings.Reset();
	TestTrue(TEXT("falls back to the backup"), MadFall::GameplaySave::ReadFile(Path, Read, Warnings, bUsedBackup));
	TestTrue(TEXT("used backup"), bUsedBackup);
	TestEqual(TEXT("previous day recovered"), Read.Day, 3);

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

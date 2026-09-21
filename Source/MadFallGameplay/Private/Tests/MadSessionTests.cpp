// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadDifficulty.h"

#include "HAL/FileManager.h"
#include "MadSession.h"
#include "MadSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSessionWorldsTest,
	"MadFall.Session.Worlds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSessionWorldsTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Session;

	// --- where worlds live ---
	// A packaged game's worlds are the player's, not the build's: rebuilding the
	// folder a game was played from deleted a player's first world.
	{
		const FString Packaged = ChooseWorldsRoot(true, FString(), TEXT("C:/Game/MadFall/Saved/"), TEXT("C:/Users/P/AppData/Local/"));
		TestTrue(TEXT("a packaged game saves in the player's folder"), Packaged.StartsWith(TEXT("C:/Users/P/AppData/Local/MadFall")));
		TestFalse(TEXT("and not beside the executable"), Packaged.StartsWith(TEXT("C:/Game")));
		TestTrue(TEXT("the editor keeps the project's Saved folder"),
			ChooseWorldsRoot(false, FString(), TEXT("C:/Project/Saved/"), TEXT("C:/Users/P/AppData/Local/")).StartsWith(TEXT("C:/Project/Saved/MadFallWorlds")));
		TestEqual(TEXT("-MadWorldsDir wins, so CI never touches a player's worlds"),
			ChooseWorldsRoot(true, TEXT("C:/CI/Worlds"), TEXT("C:/Game/Saved/"), TEXT("C:/Users/P/AppData/Local/")), FString(TEXT("C:/CI/Worlds")));
	}

	// --- names and seeds ---
	TestEqual(TEXT("spaces become underscores"), MakeWorldName(TEXT("  My First  World ")), FString(TEXT("My_First_World")));
	TestEqual(TEXT("unsafe characters dropped"), MakeWorldName(TEXT("../..\\evil:world")), FString(TEXT("evilworld")));
	TestEqual(TEXT("leading underscores stripped (reserved)"), MakeWorldName(TEXT("_Title")), FString(TEXT("Title")));
	TestFalse(TEXT("reserved title name is not a world name"), IsValidWorldName(TitleWorldName));
	TestFalse(TEXT("empty is not a world name"), IsValidWorldName(TEXT("")));
	TestTrue(TEXT("plain name is"), IsValidWorldName(TEXT("Base-2_final")));
	TestEqual(TEXT("numeric seed is used as typed"), ParseSeed(TEXT(" 12345 ")), static_cast<int64>(12345));
	TestEqual(TEXT("text seed is stable"), ParseSeed(TEXT("zombies")), ParseSeed(TEXT("zombies")));
	TestNotEqual(TEXT("different text, different seed"), ParseSeed(TEXT("zombies")), ParseSeed(TEXT("Zombies")));

	// --- world.json, listing, legacy folders, deletion ---
	const FString Root = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("SessionWorlds"), FGuid::NewGuid().ToString());
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	FMadWorldInfo Older;
	Older.Name = TEXT("Older");
	Older.DisplayName = TEXT("Older world");
	Older.Seed = 9007199254740993ll;   // not representable as a double
	Older.Created = FDateTime(2026, 1, 1);
	Older.LastPlayed = FDateTime(2026, 2, 1);
	Older.Day = 12;
	Older.Difficulty = FName(TEXT("hard"));
	Older.Directory = FPaths::Combine(Root, Older.Name);
	FString Error;
	TestTrue(TEXT("world.json written"), WriteWorldInfo(Older, Error));

	FMadWorldInfo Newer = Older;
	Newer.Name = TEXT("Newer");
	Newer.DisplayName = TEXT("Newer");
	Newer.Seed = -42;
	Newer.LastPlayed = FDateTime(2026, 9, 1);
	Newer.Directory = FPaths::Combine(Root, Newer.Name);
	TestTrue(TEXT("second world.json written"), WriteWorldInfo(Newer, Error));

	FMadWorldInfo Read;
	if (TestTrue(TEXT("world.json reads back"), ReadWorldInfo(Older.Directory, Read)))
	{
		TestEqual(TEXT("seed survives exactly, beyond double precision"), Read.Seed, Older.Seed);
		TestEqual(TEXT("display name"), Read.DisplayName, Older.DisplayName);
		TestEqual(TEXT("day"), Read.Day, 12);
		TestEqual(TEXT("difficulty"), Read.Difficulty, FName(TEXT("hard")));
		TestEqual(TEXT("last played"), Read.LastPlayed, Older.LastPlayed);
	}

	// Difficulty levels scale what they say they scale, and normal changes nothing.
	TestTrue(TEXT("three levels"), MadFall::Difficulty::GetLevels().Num() == 3 && MadFall::Difficulty::IsValid(FName(TEXT("easy"))));
	TestFalse(TEXT("nightmare is not a level"), MadFall::Difficulty::IsValid(FName(TEXT("nightmare"))));
	for (const TCHAR* Key : { TEXT("zombie_damage"), TEXT("zombie_health"), TEXT("horde_size"), TEXT("animal_damage"), TEXT("survival_drain") })
	{
		const float Easy = MadFall::Difficulty::GetScale(FName(TEXT("easy")), FName(Key));
		const float Normal = MadFall::Difficulty::GetScale(FName(TEXT("normal")), FName(Key));
		const float Hard = MadFall::Difficulty::GetScale(FName(TEXT("hard")), FName(Key));
		TestTrue(*FString::Printf(TEXT("%s: easy %.2f < normal %.2f < hard %.2f"), Key, Easy, Normal, Hard), Easy < Normal && Normal < Hard);
		TestEqual(*FString::Printf(TEXT("%s is 1 on normal"), Key), Normal, 1.0f);
	}
	TestEqual(TEXT("an unknown key is 1"), MadFall::Difficulty::GetScale(FName(TEXT("hard")), FName(TEXT("no_such_key"))), 1.0f);

	// A world from before world.json: regions only.
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Legacy"), TEXT("regions")), true);
	// Not worlds: a stray folder, and a reserved name.
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Screenshots")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TitleWorldName, TEXT("regions")), true);

	TArray<FMadWorldInfo> Worlds;
	ListWorlds(Worlds, Root);
	TestEqual(TEXT("three worlds listed (two with world.json, one legacy)"), Worlds.Num(), 3);
	// (The legacy folder was made just now, so its timestamp is the newest.)
	const int32 NewerIndex = Worlds.IndexOfByPredicate([](const FMadWorldInfo& W) { return W.Name == TEXT("Newer"); });
	const int32 OlderIndex = Worlds.IndexOfByPredicate([](const FMadWorldInfo& W) { return W.Name == TEXT("Older"); });
	TestTrue(TEXT("most recently played first"), NewerIndex != INDEX_NONE && OlderIndex != INDEX_NONE && NewerIndex < OlderIndex);
	for (int32 Index = 1; Index < Worlds.Num(); ++Index)
	{
		TestTrue(TEXT("sorted by last played"), Worlds[Index - 1].LastPlayed >= Worlds[Index].LastPlayed);
	}
	const FMadWorldInfo* Legacy = Worlds.FindByPredicate([](const FMadWorldInfo& W) { return W.Name == TEXT("Legacy"); });
	TestTrue(TEXT("legacy world listed with seed 0"), Legacy != nullptr && Legacy->Seed == 0);

	TestFalse(TEXT("a folder that is not a world is never deleted"), DeleteWorld(TEXT("Screenshots"), Root, Error));
	TestTrue(TEXT("...and is still there"), IFileManager::Get().DirectoryExists(*FPaths::Combine(Root, TEXT("Screenshots"))));
	TestFalse(TEXT("path tricks refused"), DeleteWorld(TEXT(".."), Root, Error));
	TestTrue(TEXT("a world deletes"), DeleteWorld(TEXT("Older"), Root, Error));
	ListWorlds(Worlds, Root);
	TestEqual(TEXT("two worlds left"), Worlds.Num(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSettingsTest,
	"MadFall.Session.Settings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSettingsTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::Combine(FPaths::AutomationTransientDir(), FGuid::NewGuid().ToString() + TEXT("_settings.json"));
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };

	FMadSettings Defaults = MadFall::Settings::Load(Path);
	TestEqual(TEXT("missing file gives defaults"), Defaults.FieldOfView, 90.0f);

	FMadSettings Custom;
	Custom.LookSensitivity = 1.75f;
	Custom.bInvertY = true;
	Custom.FieldOfView = 105.0f;
	Custom.ViewDistance = 12;
	Custom.Language = TEXT("de");
	Custom.Volume = 0.35f;
	Custom.MusicVolume = 0.2f;
	Custom.UiScale = 1.35f;
	Custom.Quality = 1;
	FString Error;
	TestTrue(TEXT("settings save"), MadFall::Settings::Save(Custom, Path, Error));
	const FMadSettings Loaded = MadFall::Settings::Load(Path);
	TestEqual(TEXT("sensitivity"), Loaded.LookSensitivity, 1.75f);
	TestTrue(TEXT("invert"), Loaded.bInvertY);
	TestEqual(TEXT("fov"), Loaded.FieldOfView, 105.0f);
	TestEqual(TEXT("view distance"), Loaded.ViewDistance, 12);
	TestEqual(TEXT("language"), Loaded.Language, FString(TEXT("de")));
	TestEqual(TEXT("volume"), Loaded.Volume, 0.35f);
	TestEqual(TEXT("music volume"), Loaded.MusicVolume, 0.2f);
	TestEqual(TEXT("HUD size"), Loaded.UiScale, 1.35f);
	TestEqual(TEXT("quality"), Loaded.Quality, 1);

	// A hand-edited file with nonsense is clamped, not trusted.
	FFileHelper::SaveStringToFile(TEXT(R"({"schema":"madfall.settings/1","field_of_view":500,"view_distance":-3,"look_sensitivity":0,"quality":9,"ui_scale":40})"), *Path);
	const FMadSettings Clamped = MadFall::Settings::Load(Path);
	TestEqual(TEXT("fov clamped"), Clamped.FieldOfView, 120.0f);
	TestEqual(TEXT("view distance clamped"), Clamped.ViewDistance, 3);
	TestEqual(TEXT("sensitivity clamped"), Clamped.LookSensitivity, 0.1f);
	TestEqual(TEXT("quality clamped"), Clamped.Quality, 3);
	TestEqual(TEXT("HUD size clamped"), Clamped.UiScale, 2.0f);

	// Applying writes the console variables the game reads, and reading them back round-trips.
	const FMadSettings Before = MadFall::Settings::FromConsoleVariables();
	MadFall::Settings::Apply(Custom);
	const FMadSettings Applied = MadFall::Settings::FromConsoleVariables();
	TestEqual(TEXT("applied fov"), Applied.FieldOfView, 105.0f);
	TestEqual(TEXT("applied view distance"), Applied.ViewDistance, 12);
	TestEqual(TEXT("applied quality sets every scalability group"), Applied.Quality, 1);
	const IConsoleVariable* LightShadows = IConsoleManager::Get().FindConsoleVariable(TEXT("mad.models.LightShadows"));
	TestTrue(TEXT("medium quality turns block light shadows off"), LightShadows != nullptr && !LightShadows->GetBool());
	MadFall::Settings::Apply(Before);

	return true;
}

#endif

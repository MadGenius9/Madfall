// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadDefinitionSources.h"
#include "MadLocalization.h"
#include "MadPrefabRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadLocalizationTests
{
	TSharedRef<FJsonObject> Json(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadStringTableTest,
	"MadFall.Mods.Strings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadStringTableTest::RunTest(const FString& Parameters)
{
	using namespace MadLocalizationTests;

	FMadStringTable Table;
	TArray<FMadDefinitionError> Errors;
	const FName Core(TEXT("madfall"));
	const FName Mod(TEXT("translator"));

	TestTrue(TEXT("english loads"), Table.AddJson(Json(TEXT(R"({"schema":"madfall.strings/1","language":"en",
		"strings":{"items.wood_plank":"Wood Plank","items.rock":"Rock","hud.day":"Day"}})")), TEXT("en.json"), Core, Errors));
	TestTrue(TEXT("german loads"), Table.AddJson(Json(TEXT(R"({"schema":"madfall.strings/1","language":"de",
		"strings":{"items.wood_plank":"Holzbrett"}})")), TEXT("de.json"), Mod, Errors));
	// A later source renames a first-party string: later wins, key by key.
	Table.AddJson(Json(TEXT(R"({"schema":"madfall.strings/1","language":"en","strings":{"items.rock":"Stone Chunk"}})")),
		TEXT("rename.json"), Mod, Errors);
	TestEqual(TEXT("valid files produce no errors"), Errors.Num(), 0);

	Table.SetLanguage(TEXT("en"));
	TestEqual(TEXT("english key"), Table.Resolve(TEXT("@items.wood_plank")), FString(TEXT("Wood Plank")));
	TestEqual(TEXT("override wins"), Table.Resolve(TEXT("@items.rock")), FString(TEXT("Stone Chunk")));
	TestEqual(TEXT("plain text passes through"), Table.Resolve(TEXT("Scrap Knife")), FString(TEXT("Scrap Knife")));

	Table.SetLanguage(TEXT("DE"));
	TestEqual(TEXT("language codes are case-insensitive"), Table.GetLanguage(), FString(TEXT("de")));
	TestEqual(TEXT("translated key"), Table.Resolve(TEXT("@items.wood_plank")), FString(TEXT("Holzbrett")));
	TestEqual(TEXT("untranslated key falls back to English"), Table.Resolve(TEXT("@hud.day")), FString(TEXT("Day")));
	TestEqual(TEXT("unknown key reads as a name, never a raw key"), Table.Resolve(TEXT("@items.iron_pickaxe")), FString(TEXT("Iron Pickaxe")));

	TestEqual(TEXT("readable from an id"), FMadStringTable::MakeReadable(TEXT("gonemod:scrap_knife")), FString(TEXT("Scrap Knife")));
	TestEqual(TEXT("readable from a path id"), FMadStringTable::MakeReadable(TEXT("madfall:loot/tools")), FString(TEXT("Loot Tools")));

	TArray<FString> Languages = Table.GetLanguages();
	TestTrue(TEXT("two languages"), Languages.Num() == 2 && Languages[0] == TEXT("de") && Languages[1] == TEXT("en"));

	// --- validation ---------------------------------------------------------
	{
		FMadStringTable Bad;
		TArray<FMadDefinitionError> BadErrors;
		TestFalse(TEXT("wrong schema rejected"), Bad.AddJson(Json(TEXT(R"({"schema":"madfall.item/1","language":"en","strings":{}})")), TEXT("a.json"), Mod, BadErrors));
		TestFalse(TEXT("bad language rejected"), Bad.AddJson(Json(TEXT(R"({"schema":"madfall.strings/1","language":"English","strings":{}})")), TEXT("b.json"), Mod, BadErrors));
		TestTrue(TEXT("a file with one bad entry still loads the good ones"), Bad.AddJson(Json(TEXT(R"({"schema":"madfall.strings/1","language":"en",
			"strings":{"Items.Bad":"x","items.number":5,"items.good":"Good"}})")), TEXT("c.json"), Mod, BadErrors));
		TestEqual(TEXT("good entry kept"), Bad.Resolve(TEXT("@items.good")), FString(TEXT("Good")));
		TestNull(TEXT("upper-case key rejected"), Bad.Find(TEXT("Items.Bad"), TEXT("en")));
		TestEqual(TEXT("four errors"), BadErrors.Num(), 4);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadShippedStringsTest,
	"MadFall.Mods.ShippedStrings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadShippedStringsTest::RunTest(const FString& Parameters)
{
	// Loaded privately, like the other shipped-content tests, so file errors
	// fail the test instead of only reaching the log.
	FMadStringTable Table;
	TArray<FMadDefinitionError> Errors;
	MadFall::Definitions::ForEachSource(TEXT("strings"), [&](const FString& Directory, FName ModId)
	{
		Table.AddFromDirectory(Directory, ModId, Errors);
	});
	for (const FMadDefinitionError& Error : Errors)
	{
		AddError(Error.ToString());
	}

	// Every "@key" a registered block, biome or prefab uses has English text.
	// Gameplay definitions (items, perks, zombies) are checked in
	// MadFall.Items.ShippedContent, which owns loading them.
	int32 Checked = 0;
	auto Check = [&](const FString& Owner, const FString& Text)
	{
		if (!Text.StartsWith(TEXT("@")))
		{
			return;
		}
		++Checked;
		TestNotNull(*FString::Printf(TEXT("%s: %s has English text"), *Owner, *Text), Table.Find(Text.Mid(1), TEXT("en")));
	};

	for (const FMadBlockEntry& Entry : UMadVoxelWorldSubsystem::GetBlockRegistry().GetEntries())
	{
		if (!Entry.bUnresolved)
		{
			Check(Entry.Definition.Id.ToString(), Entry.Definition.DisplayName);
		}
	}
	for (const FMadBiomeDefinitionData& Biome : UMadVoxelWorldSubsystem::GetBiomeRegistry().GetAll())
	{
		Check(Biome.Id.ToString(), Biome.DisplayName);
	}
	for (const FMadPrefab& Prefab : UMadVoxelWorldSubsystem::GetPrefabRegistry().GetAll())
	{
		Check(Prefab.Id.ToString(), Prefab.DisplayName);
	}

	AddInfo(FString::Printf(TEXT("%d localized display names checked; %d English strings."), Checked, Table.NumStrings(TEXT("en"))));
	TestTrue(TEXT("the check saw the shipped keys"), Checked >= 30);

	// The example translation pack arrives through mod discovery like any mod,
	// and a key it does not translate still reads in English.
	TestTrue(TEXT("example_german is loaded"), Table.NumStrings(TEXT("de")) > 0);
	Table.SetLanguage(TEXT("de"));
	TestEqual(TEXT("translated through the mod"), Table.Resolve(TEXT("@items.wood_plank")), FString(TEXT("Holzbrett")));
	TestEqual(TEXT("untranslated falls back to English"), Table.Resolve(TEXT("@items.iron_pickaxe")), FString(TEXT("Iron Pickaxe")));
	// A translated key English does not have is a typo: it would never show.
	for (const FString& Key : Table.GetKeys(TEXT("de")))
	{
		TestNotNull(*FString::Printf(TEXT("German key %s exists in English"), *Key), Table.Find(Key, TEXT("en")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadBlockRegistry.h"
#include "MadDefinitionPatches.h"
#include "MadDefinitionSources.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadSurfaceTests
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
	FMadSurfaceRegistryTest,
	"MadFall.Mods.Surfaces",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSurfaceRegistryTest::RunTest(const FString& Parameters)
{
	using namespace MadSurfaceTests;

	FMadSurfaceRegistry Registry;
	TArray<FMadDefinitionError> Errors;
	Registry.BeginLoad();
	Registry.AddJson(Json(TEXT(R"({"schema":"madfall.surface/1","id":"madfall:concrete","color":[0.5,0.5,0.5]})")), TEXT("s.json"), FName(TEXT("madfall")), Errors);
	Registry.AddJson(Json(TEXT(R"({"schema":"madfall.surface/1","id":"madfall:wood","color":[0.5,0.3,0.1],"material":"/Game/Materials/M_MadVoxel.M_MadVoxel"})")), TEXT("s.json"), FName(TEXT("madfall")), Errors);

	// A texture pack re-skins concrete with a patch - it cannot redefine another namespace's surface.
	TestFalse(TEXT("foreign namespace rejected"), Registry.AddJson(Json(TEXT(R"({"schema":"madfall.surface/1","id":"madfall:concrete","color":[1,0,0]})")),
		TEXT("pack/s.json"), FName(TEXT("texture_pack")), Errors));
	TestEqual(TEXT("one namespace error"), Errors.Num(), 1);
	Errors.Reset();

	FMadPatchSet Patches;
	FMadDefinitionPatch Patch;
	MadFall::JsonPatch::Parse(Json(TEXT(R"({"schema":"madfall.patch/1","kind":"surface","target":"madfall:concrete",
		"ops":[{"op":"set","path":"/material","value":"/TexturePack/M_Concrete.M_Concrete"}]})")), TEXT("pack/patch.json"), FName(TEXT("texture_pack")), Patch, Errors);
	Patches.Add(MoveTemp(Patch));
	Registry.FinishLoad(&Patches, Errors);
	TestEqual(TEXT("loads cleanly"), Errors.Num(), 0);
	for (const FMadDefinitionError& Error : Errors) { AddError(Error.ToString()); }

	const FMadSurfaceDefinition* Concrete = Registry.Find(FName(TEXT("madfall:concrete")));
	if (TestNotNull(TEXT("concrete"), Concrete))
	{
		TestEqual(TEXT("patched material"), Concrete->Material.ToString(), FString(TEXT("/TexturePack/M_Concrete.M_Concrete")));
		TestTrue(TEXT("colour kept"), Concrete->bHasColor);
	}

	// Colours: authored linear 0..1, emitted as linear bytes; an undefined class still gets a stable colour.
	const FColor Wood = Registry.GetVertexColor(FName(TEXT("madfall:wood")));
	TestEqual(TEXT("wood colour is the authored sRGB, stored linear"), Wood, FLinearColor(0.214f, 0.0732f, 0.0100f).ToFColor(false));
	TestTrue(TEXT("sRGB 0.5 is about 0.214 linear"), FMath::IsNearlyEqual(MadFall::Surfaces::SRGBToLinear(0.5f), 0.214f, 0.001f));
	const FName Unknown(TEXT("somemod:marble"));
	TestEqual(TEXT("undefined class colour is stable"), Registry.GetVertexColor(Unknown), Registry.GetVertexColor(Unknown));
	TestEqual(TEXT("no class is neutral grey"), Registry.GetVertexColor(NAME_None), FColor(82, 82, 82));

	// Patterns travel in vertex colour alpha; no pattern is alpha 255, as before patterns existed.
	{
		FMadSurfaceRegistry Patterned;
		TArray<FMadDefinitionError> PatternErrors;
		Patterned.BeginLoad();
		Patterned.AddJson(Json(TEXT(R"({"schema":"madfall.surface/1","id":"madfall:boards","color":[0.5,0.3,0.1],"pattern":"planks"})")), TEXT("p.json"), FName(TEXT("madfall")), PatternErrors);
		Patterned.AddJson(Json(TEXT(R"({"schema":"madfall.surface/1","id":"madfall:odd","color":[0.5,0.5,0.5],"pattern":"marble"})")), TEXT("p.json"), FName(TEXT("madfall")), PatternErrors);
		Patterned.FinishLoad(nullptr, PatternErrors);
		TestEqual(TEXT("planks is pattern 5"), Patterned.Find(FName(TEXT("madfall:boards")))->Pattern, 5);
		TestEqual(TEXT("and alpha 255 - 5 x 16"), Patterned.GetVertexColor(FName(TEXT("madfall:boards"))).A, static_cast<uint8>(175));
		TestEqual(TEXT("an unknown pattern is an error"), PatternErrors.Num(), 1);
		TestEqual(TEXT("and falls back to plain"), Patterned.Find(FName(TEXT("madfall:odd")))->Pattern, 0);
		TestEqual(TEXT("names round trip"), MadFall::Surfaces::FindPattern(TEXT("Gravel")), MadFall::Surfaces::NumPatterns - 1);
		TestEqual(TEXT("sixteen patterns fit the alpha encoding"), MadFall::Surfaces::NumPatterns, 16);
	}

	// A texture layer takes the pattern's place in vertex alpha, with the same encoding.
	{
		FMadSurfaceRegistry Layered;
		TArray<FMadDefinitionError> LayerErrors;
		Layered.BeginLoad();
		Layered.AddJson(Json(TEXT(R"({"schema":"madfall.surface/1","id":"madfall:rock","color":[0.4,0.4,0.4],"pattern":"stone","texture_layer":3})")), TEXT("l.json"), FName(TEXT("madfall")), LayerErrors);
		Layered.AddJson(Json(TEXT(R"({"schema":"madfall.surface/1","id":"madfall:far","color":[0.4,0.4,0.4],"texture_layer":16})")), TEXT("l.json"), FName(TEXT("madfall")), LayerErrors);
		Layered.FinishLoad(nullptr, LayerErrors);
		const FMadSurfaceDefinition* Rock = Layered.Find(FName(TEXT("madfall:rock")));
		TestTrue(TEXT("layer parsed"), Rock != nullptr && Rock->TextureLayer == 3);
		TestTrue(TEXT("the pattern is kept for icons and fallbacks"), Rock != nullptr && Rock->Pattern == 1);
		TestEqual(TEXT("alpha carries the layer: 255 - 3 x 16"), Layered.GetVertexColor(FName(TEXT("madfall:rock"))).A, static_cast<uint8>(207));
		TestEqual(TEXT("a layer past what alpha can carry is an error"), LayerErrors.Num(), 1);
		TestTrue(TEXT("and is not applied"), Layered.Find(FName(TEXT("madfall:far")))->TextureLayer == INDEX_NONE);
	}

	// Validation.
	{
		FMadSurfaceRegistry Bad;
		TArray<FMadDefinitionError> BadErrors;
		Bad.BeginLoad();
		Bad.AddJson(Json(TEXT(R"({"schema":"madfall.surface/1","id":"madfall:a","color":[2,"x"],"colour":[1,1,1],"material":"madfall:oops"})")), TEXT("bad.json"), FName(TEXT("madfall")), BadErrors);
		Bad.FinishLoad(nullptr, BadErrors);
		TestEqual(TEXT("bad colour, unknown field, id used as a path"), BadErrors.Num(), 3);
		TestFalse(TEXT("bad colour not applied"), Bad.Find(FName(TEXT("madfall:a")))->bHasColor);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadShippedSurfacesTest,
	"MadFall.Mods.ShippedSurfaces",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadShippedSurfacesTest::RunTest(const FString& Parameters)
{
	FMadSurfaceRegistry Registry;
	TArray<FMadDefinitionError> Errors;
	Registry.BeginLoad();
	MadFall::Definitions::ForEachSource(TEXT("surfaces"), [&](const FString& Directory, FName ModId) { Registry.AddFromDirectory(Directory, ModId, Errors); });
	Registry.FinishLoad(&MadFall::GetPatchSet(), Errors);
	for (const FMadDefinitionError& Error : Errors)
	{
		AddError(Error.ToString());
	}

	// Every material class a registered block uses has a surface, so nothing
	// ships with a hash placeholder colour.
	TSet<FName> Classes;
	for (const FMadBlockEntry& Entry : UMadVoxelWorldSubsystem::GetBlockRegistry().GetEntries())
	{
		if (!Entry.bUnresolved && !Entry.Definition.MaterialClass.IsNone())
		{
			Classes.Add(Entry.Definition.MaterialClass);
		}
	}
	for (const FName& Class : Classes)
	{
		TestNotNull(*FString::Printf(TEXT("material class %s has a surface"), *Class.ToString()), Registry.Find(Class));
	}
	AddInfo(FString::Printf(TEXT("%d material classes in use, %d surfaces."), Classes.Num(), Registry.GetAll().Num()));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

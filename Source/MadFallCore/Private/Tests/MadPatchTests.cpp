// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadDefinitionPatches.h"
#include "MadGameplayDefinitions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MadPatchTests
{
	TSharedRef<FJsonObject> Json(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		check(Object.IsValid());
		return Object.ToSharedRef();
	}

	FString Compact(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		return Out;
	}

	FMadDefinitionPatch Patch(const TCHAR* Text, const TCHAR* ModId, TArray<FMadDefinitionError>& Errors)
	{
		FMadDefinitionPatch Out;
		MadFall::JsonPatch::Parse(Json(Text), FString::Printf(TEXT("%s/patch.json"), ModId), FName(ModId), Out, Errors);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadJsonPatchOpsTest,
	"MadFall.Mods.PatchOps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadJsonPatchOpsTest::RunTest(const FString& Parameters)
{
	using namespace MadPatchTests;

	auto Run = [this](const TCHAR* Document, const TCHAR* OpsJson, const TCHAR* Expected)
	{
		TArray<FMadDefinitionError> Errors;
		const FMadDefinitionPatch P = Patch(*FString::Printf(TEXT(R"({"schema":"madfall.patch/1","kind":"block","target":"x:y","ops":%s})"), OpsJson), TEXT("m"), Errors);
		TSharedRef<FJsonObject> Doc = Json(Document);
		FString Error;
		bool bAllOk = Errors.Num() == 0;
		for (const FMadPatchOp& Op : P.Ops)
		{
			bAllOk &= MadFall::JsonPatch::Apply(Doc, Op, Error);
		}
		if (Expected == nullptr)
		{
			TestFalse(FString::Printf(TEXT("%s fails"), OpsJson), bAllOk);
			return;
		}
		TestTrue(FString::Printf(TEXT("%s applies (%s)"), OpsJson, *Error), bAllOk);
		TestEqual(OpsJson, Compact(Doc), FString(Expected));
	};

	Run(TEXT(R"({"material":{"hardness":100}})"), TEXT(R"([{"op":"set","path":"/material/hardness","value":60}])"), TEXT(R"({"material":{"hardness":60}})"));
	Run(TEXT(R"({})"), TEXT(R"([{"op":"set","path":"/structure/support_strength","value":5}])"), TEXT(R"({"structure":{"support_strength":5}})"));
	Run(TEXT(R"({"tags":["a"]})"), TEXT(R"([{"op":"append","path":"/tags","value":"b"}])"), TEXT(R"({"tags":["a","b"]})"));
	Run(TEXT(R"({})"), TEXT(R"([{"op":"append","path":"/tags","value":"b"}])"), TEXT(R"({"tags":["b"]})"));
	Run(TEXT(R"({"tags":["a","b","c"]})"), TEXT(R"([{"op":"remove","path":"/tags/1"}])"), TEXT(R"({"tags":["a","c"]})"));
	Run(TEXT(R"({"tags":["a"]})"), TEXT(R"([{"op":"set","path":"/tags/-","value":"z"}])"), TEXT(R"({"tags":["a","z"]})"));
	Run(TEXT(R"({"stages":[{"at":0},{"at":100}]})"), TEXT(R"([{"op":"set","path":"/stages/1/at","value":90}])"), TEXT(R"({"stages":[{"at":0},{"at":90}]})"));
	Run(TEXT(R"({"a":1,"b":2})"), TEXT(R"([{"op":"remove","path":"/a"}])"), TEXT(R"({"b":2})"));
	Run(TEXT(R"({"a~b/c":1})"), TEXT(R"([{"op":"set","path":"/a~0b~1c","value":2}])"), TEXT(R"({"a~b/c":2})"));

	Run(TEXT(R"({"a":1})"), TEXT(R"([{"op":"remove","path":"/missing"}])"), nullptr);
	Run(TEXT(R"({"tags":"not an array"})"), TEXT(R"([{"op":"append","path":"/tags","value":"b"}])"), nullptr);
	Run(TEXT(R"({"tags":["a"]})"), TEXT(R"([{"op":"remove","path":"/tags/5"}])"), nullptr);

	// A parsed patch value is copied on apply: applying one patch to two
	// documents must not make them share (and later co-mutate) a value.
	{
		TArray<FMadDefinitionError> Errors;
		const FMadDefinitionPatch P = Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"block","target":"x:y","ops":[{"op":"set","path":"/obj","value":{"n":1}}]})"), TEXT("m"), Errors);
		TSharedRef<FJsonObject> First = Json(TEXT("{}"));
		TSharedRef<FJsonObject> Second = Json(TEXT("{}"));
		FString Error;
		MadFall::JsonPatch::Apply(First, P.Ops[0], Error);
		MadFall::JsonPatch::Apply(Second, P.Ops[0], Error);
		First->GetObjectField(TEXT("obj"))->SetNumberField(TEXT("n"), 99);
		TestEqual(TEXT("values are not aliased"), Second->GetObjectField(TEXT("obj"))->GetNumberField(TEXT("n")), 1.0);
	}

	// Parse-time validation.
	{
		TArray<FMadDefinitionError> Errors;
		Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"spaceship","target":"x:y","ops":[{"op":"set","path":"/a","value":1}]})"), TEXT("m"), Errors);
		TestTrue(TEXT("unknown kind rejected"), Errors.Num() == 1);
		Errors.Reset();
		Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"block","target":"x:y","ops":[{"op":"merge","path":"/a","value":1}]})"), TEXT("m"), Errors);
		TestTrue(TEXT("unknown op rejected"), Errors.Num() == 1);
		Errors.Reset();
		Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"block","target":"x:y","ops":[{"op":"set","path":"/a"}]})"), TEXT("m"), Errors);
		TestTrue(TEXT("set without value rejected"), Errors.Num() == 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadDefinitionPatchesTest,
	"MadFall.Mods.Patches",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadDefinitionPatchesTest::RunTest(const FString& Parameters)
{
	using namespace MadPatchTests;

	TArray<FMadDefinitionError> Errors;
	FMadPatchSet Patches;

	// Two mods edit different fields of the same tool; a third edits one of the
	// same fields and loads later; a fourth patches the parent tool family.
	Patches.Add(Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"item","target":"madfall:pick","ops":[{"op":"set","path":"/tool/durability","value":999}]})"), TEXT("durable"), Errors));
	Patches.Add(Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"item","target":"madfall:pick","ops":[{"op":"append","path":"/tags","value":"tool.shiny"}]})"), TEXT("shiny"), Errors));
	Patches.Add(Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"item","target":"madfall:pick","ops":[{"op":"set","path":"/tool/durability","value":5}]})"), TEXT("fragile"), Errors));
	Patches.Add(Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"item","target":"madfall:base_tool","ops":[{"op":"set","path":"/tool/use_seconds","value":0.25}]})"), TEXT("fast"), Errors));
	// A recipe patch that breaks the recipe, and a patch for something nobody defines.
	Patches.Add(Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"recipe","target":"madfall:pick","ops":[{"op":"remove","path":"/output"}]})"), TEXT("broken"), Errors));
	Patches.Add(Patch(TEXT(R"({"schema":"madfall.patch/1","kind":"loot","target":"ghostmod:loot/x","ops":[{"op":"set","path":"/rolls","value":2}]})"), TEXT("typo"), Errors));
	TestEqual(TEXT("all patches parse"), Errors.Num(), 0);

	FMadGameplayDefinitions Defs;
	Defs.BeginLoad();
	const FName Core(TEXT("madfall"));
	Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"madfall:base_tool","tool":{"damage":{"madfall:blunt":5},"use_seconds":1.0,"durability":10}})")), TEXT("items.json"), Core, Errors);
	Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"madfall:pick","extends":"madfall:base_tool","tags":["tool.pickaxe"],"tool":{"durability":50}})")), TEXT("items.json"), Core, Errors);
	Defs.AddItemJson(Json(TEXT(R"({"schema":"madfall.item/1","id":"madfall:rock"})")), TEXT("items.json"), Core, Errors);
	Defs.AddRecipeJson(Json(TEXT(R"({"schema":"madfall.recipe/1","id":"madfall:pick","output":{"item":"madfall:pick"},"ingredients":[{"item":"madfall:rock","count":3}]})")), TEXT("recipes.json"), Core, Errors);

	TArray<FMadDefinitionError> PatchErrors;
	Defs.ApplyPatches(Patches, PatchErrors);
	Defs.FinishLoad(nullptr, PatchErrors);

	const FMadItemDefinition* Pick = Defs.FindItem(FName(TEXT("madfall:pick")));
	if (TestNotNull(TEXT("pick exists"), Pick))
	{
		TestEqual(TEXT("same path: the later mod wins"), Pick->Tool.Durability, 5);
		TestTrue(TEXT("different path from another mod also applied"), Pick->Tags.Contains(FName(TEXT("tool.shiny"))));
		TestTrue(TEXT("original array entry kept by append"), Pick->Tags.Contains(FName(TEXT("tool.pickaxe"))));
		TestEqual(TEXT("patching the parent reaches the child"), Pick->Tool.UseSeconds, 0.25f);
	}

	auto HasError = [&PatchErrors](const TCHAR* Needle)
	{
		return PatchErrors.ContainsByPredicate([Needle](const FMadDefinitionError& E) { return E.Message.Contains(Needle); });
	};
	TestTrue(TEXT("same-path conflict warned, naming both mods"), HasError(TEXT("also written by mod 'durable'")));
	TestNull(TEXT("a patch that breaks a recipe disables it"), Defs.FindRecipe(FName(TEXT("madfall:pick"))));
	TestTrue(TEXT("and says why, at the field the patch removed"),
		PatchErrors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.Pointer == TEXT("/output"); }));
	TestTrue(TEXT("patch with no target reported"), HasError(TEXT("no loot named 'ghostmod:loot/x'")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

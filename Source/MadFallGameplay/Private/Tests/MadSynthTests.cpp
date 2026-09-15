// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "MadSurfaceRegistry.h"
#include "MadSynth.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSynthTest,
	"MadFall.Audio.Synth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSynthTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Synth;

	for (int32 Index = 0; Index < static_cast<int32>(EMadSound::Num); ++Index)
	{
		const EMadSound Sound = static_cast<EMadSound>(Index);
		TArray<int16> A, B, Other;
		Generate(Sound, 1, A);
		Generate(Sound, 1, B);
		Generate(Sound, 2, Other);

		const float Seconds = static_cast<float>(A.Num()) / SampleRate;
		int32 Peak = 0;
		double Energy = 0.0;
		for (int16 Sample : A)
		{
			Peak = FMath::Max(Peak, FMath::Abs(static_cast<int32>(Sample)));
			Energy += static_cast<double>(Sample) * Sample;
		}
		const double Rms = A.Num() > 0 ? FMath::Sqrt(Energy / A.Num()) : 0.0;

		if (A != B)
		{
			AddError(FString::Printf(TEXT("%s: the same seed generated different audio"), GetName(Sound)));
		}
		if (Seconds < 0.02f || Seconds > 3.5f)
		{
			AddError(FString::Printf(TEXT("%s: %.3f s long"), GetName(Sound), Seconds));
		}
		// Normalised to 80% of full scale: loud enough to hear, never clipping.
		if (Peak < 25000 || Peak > 26300)
		{
			AddError(FString::Printf(TEXT("%s: peak %d, expected about 26214"), GetName(Sound), Peak));
		}
		if (Rms < 300.0)
		{
			AddError(FString::Printf(TEXT("%s: effectively silent (rms %.0f)"), GetName(Sound), Rms));
		}
		// Tones (UI, pickups, crafting, the horn) are meant to be identical each
		// time; everything struck or voiced varies with the seed.
		const bool bTonal = Sound == EMadSound::Pickup || Sound == EMadSound::CraftDone || Sound == EMadSound::HordeHorn || Sound == EMadSound::UIClick || Sound == EMadSound::WindLoop;
		if (!bTonal && A == Other)
		{
			AddError(FString::Printf(TEXT("%s: a different seed gave the same sound"), GetName(Sound)));
		}
	}

	TestEqual(TEXT("wood hit variant"), ForKind(EMadSound::Hit, EMadImpactKind::Wood), EMadSound::HitWood);
	TestEqual(TEXT("metal break variant"), ForKind(EMadSound::Break, EMadImpactKind::Metal), EMadSound::BreakMetal);
	TestEqual(TEXT("foliage step variant"), ForKind(EMadSound::Step, EMadImpactKind::Foliage), EMadSound::StepFoliage);
	TestEqual(TEXT("metal creak variant"), ForKind(EMadSound::Creak, EMadImpactKind::Metal), EMadSound::CreakMetal);
	TestEqual(TEXT("creak names"), FString(GetName(EMadSound::CreakWood)), FString(TEXT("creak_wood")));
	TestEqual(TEXT("non-material sounds are unchanged"), ForKind(EMadSound::ZombieGroan, EMadImpactKind::Wood), EMadSound::ZombieGroan);
	TestEqual(TEXT("unknown impact is stone"), ParseImpactKind(FName(TEXT("jelly"))), EMadImpactKind::Stone);
	TestEqual(TEXT("impact names are case-insensitive"), ParseImpactKind(FName(TEXT("Metal"))), EMadImpactKind::Metal);

	// Every shipped surface names a valid impact, and a bad one is reported.
	for (const FMadSurfaceDefinition& Surface : MadFall::GetSurfaces().GetAll())
	{
		const FString Kind = Surface.Impact.ToString();
		if (Kind != TEXT("stone") && Kind != TEXT("wood") && Kind != TEXT("dirt") && Kind != TEXT("metal") && Kind != TEXT("foliage"))
		{
			AddError(FString::Printf(TEXT("surface %s has impact '%s'"), *Surface.Id.ToString(), *Kind));
		}
	}
	{
		FMadSurfaceRegistry Registry;
		TArray<FMadDefinitionError> Errors;
		Registry.BeginLoad();
		TSharedPtr<FJsonObject> Json;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT(R"({"schema":"madfall.surface/1","id":"test:glass","impact":"glass"})")), Json);
		Registry.AddJson(Json.ToSharedRef(), TEXT("glass.json"), FName(TEXT("test")), Errors);
		Registry.FinishLoad(nullptr, Errors);
		TestTrue(TEXT("an unknown impact kind is reported"), Errors.ContainsByPredicate([](const FMadDefinitionError& E) { return E.Pointer == TEXT("/impact"); }));
		const FMadSurfaceDefinition* Glass = Registry.Find(FName(TEXT("test:glass")));
		TestTrue(TEXT("...and falls back to stone"), Glass != nullptr && Glass->Impact == FName(TEXT("stone")));
	}

	return true;
}

#endif

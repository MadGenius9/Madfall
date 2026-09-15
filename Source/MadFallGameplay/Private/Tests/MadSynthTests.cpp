// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MadAudioSubsystem.h"
#include "MadMusicSubsystem.h"
#include "Sound/SoundWave.h"

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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSoundRecordingsTest,
	"MadFall.Audio.Recordings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSoundRecordingsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a folder name finds its sound"), UMadAudioSubsystem::FindSoundByName(TEXT("step_wood")), EMadSound::StepWood);
	TestEqual(TEXT("and a sound its folder"), UMadAudioSubsystem::GetRecordingFolder(EMadSound::ZombieGroan), FString(TEXT("/Game/Audio/zombie_groan")));
	TestEqual(TEXT("anything else finds nothing"), UMadAudioSubsystem::FindSoundByName(TEXT("step_wod")), EMadSound::Num);

	// The shipped recordings: every folder names a sound (a typo would be
	// silently never played), every wave has sound in it, and loops loop.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.ScanPathsSynchronous({ TEXT("/Game/Audio") }, false);
	TArray<FAssetData> Assets;
	FARFilter Filter;
	Filter.PackagePaths.Add(TEXT("/Game/Audio"));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(USoundWave::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, Assets);

	TSet<EMadSound> Recorded;
	for (const FAssetData& Asset : Assets)
	{
		const FString Folder = FPaths::GetCleanFilename(Asset.PackagePath.ToString());
		const EMadSound Sound = UMadAudioSubsystem::FindSoundByName(Folder);
		if (!TestNotEqual(*FString::Printf(TEXT("%s is in a folder named after a sound (%s)"), *Asset.AssetName.ToString(), *Folder), Sound, EMadSound::Num))
		{
			continue;
		}
		Recorded.Add(Sound);
		const USoundWave* Wave = Cast<USoundWave>(Asset.GetAsset());
		if (TestNotNull(*FString::Printf(TEXT("%s loads"), *Asset.AssetName.ToString()), Wave))
		{
			TestTrue(*FString::Printf(TEXT("%s is not empty (%.2f s)"), *Asset.AssetName.ToString(), Wave->Duration), Wave->Duration > 0.05f);
			const bool bLoopSound = Sound == EMadSound::RainLoop || Sound == EMadSound::WindLoop;
			TestEqual(*FString::Printf(TEXT("%s loops exactly when its sound is a loop"), *Asset.AssetName.ToString()), static_cast<bool>(Wave->bLooping), bLoopSound);
		}
	}
	TestTrue(*FString::Printf(TEXT("most sounds are recorded (%d)"), Recorded.Num()), Recorded.Num() >= 25);
	TestTrue(TEXT("footsteps, zombies and rain are among them"), Recorded.Contains(EMadSound::StepDirt) && Recorded.Contains(EMadSound::ZombieGroan) && Recorded.Contains(EMadSound::RainLoop));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadMusicTest,
	"MadFall.Audio.Music",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadMusicTest::RunTest(const FString& Parameters)
{
	using MadFall::Music::ChooseTrack;
	TestEqual(TEXT("by day, the day theme"), ChooseTrack(false, false), EMadMusicTrack::Day);
	TestEqual(TEXT("by night, the night theme"), ChooseTrack(true, false), EMadMusicTrack::Night);
	TestEqual(TEXT("a horde overrides the clock"), ChooseTrack(true, true), EMadMusicTrack::Horde);
	TestEqual(TEXT("even by day"), ChooseTrack(false, true), EMadMusicTrack::Horde);

	float Shortest = TNumericLimits<float>::Max();
	float Longest = 0.0f;
	for (uint32 Seed = 0; Seed < 200; ++Seed)
	{
		Shortest = FMath::Min(Shortest, MadFall::Music::SilenceAfter(Seed));
		Longest = FMath::Max(Longest, MadFall::Music::SilenceAfter(Seed));
	}
	TestTrue(*FString::Printf(TEXT("silences last one to two and a half minutes (%.0f-%.0f s)"), Shortest, Longest), Shortest >= 60.0f && Longest <= 150.0f && Longest - Shortest > 45.0f);

	// The shipped tracks load; only the horde's loops.
	for (const TCHAR* Name : { TEXT("day"), TEXT("night"), TEXT("horde") })
	{
		const USoundWave* Wave = LoadObject<USoundWave>(nullptr, *FString::Printf(TEXT("/Game/Music/SW_Music_%s.SW_Music_%s"), Name, Name));
		if (TestNotNull(*FString::Printf(TEXT("the %s track loads"), Name), Wave))
		{
			TestTrue(*FString::Printf(TEXT("the %s track is at least a minute long (%.0f s)"), Name, Wave->Duration), Wave->Duration >= 60.0f);
			TestEqual(*FString::Printf(TEXT("the %s track loops only if it is the horde's"), Name), static_cast<bool>(Wave->bLooping), FCString::Strcmp(Name, TEXT("horde")) == 0);
		}
	}
	return true;
}

#endif

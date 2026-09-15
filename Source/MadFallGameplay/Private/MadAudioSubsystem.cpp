// Copyright MadFall. All Rights Reserved.

#include "MadAudioSubsystem.h"

#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadSurfaceRegistry.h"
#include "Misc/StringBuilder.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"

namespace
{
	TAutoConsoleVariable<float> CVarVolume(
		TEXT("mad.audio.Volume"), 0.8f,
		TEXT("Master volume for MadFall's sound effects, 0..1."));

	TAutoConsoleVariable<int32> CVarMaxVoices(
		TEXT("mad.audio.MaxVoices"), 24,
		TEXT("Most MadFall sounds playing at once; more are dropped rather than queued."));

	/** Seconds before the same sound may start again: a swing of a pick is one hit, a horde is not forty groans at once. */
	double MinimumGap(EMadSound Sound)
	{
		switch (Sound)
		{
		case EMadSound::ZombieGroan: return 0.35;
		case EMadSound::Collapse:    return 0.5;
		case EMadSound::HordeHorn:   return 5.0;
		case EMadSound::Creak:
		case EMadSound::CreakWood:
		case EMadSound::CreakDirt:
		case EMadSound::CreakMetal:
		case EMadSound::CreakFoliage: return 0.6;
		default:                     return 0.03;
		}
	}
}

bool UMadAudioSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadAudioSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadAudioSubsystem, STATGROUP_Tickables);
}

void UMadAudioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Every variation of every sound, off the game thread: the three-second horde
	// horn alone measured 4.5 ms to synthesise, and doing it on first play put
	// that stall on the frame the horde arrived.
	SynthTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
	{
		for (int32 Sound = 0; Sound < static_cast<int32>(EMadSound::Num); ++Sound)
		{
			for (int32 Variation = 0; Variation < Variations; ++Variation)
			{
				MadFall::Synth::Generate(static_cast<EMadSound>(Sound), static_cast<uint32>(Variation + 1) * 7919u, Cache[Sound][Variation]);
			}
		}
		bReady = true;
	});
}

void UMadAudioSubsystem::Deinitialize()
{
	SynthTask.Wait();
	for (const FVoice& Voice : Voices)
	{
		if (UAudioComponent* Component = Voice.Component.Get())
		{
			Component->Stop();
		}
	}
	Voices.Reset();
	Super::Deinitialize();
}

void UMadAudioSubsystem::SetLoop(int32 Channel, EMadSound Sound, float Volume)
{
	if (Channel < 0 || Channel >= LoopChannels)
	{
		return;
	}
	FLoop& Loop = Loops[Channel];
	if (Loop.Sound != Sound)
	{
		if (UAudioComponent* Component = Loop.Component.Get())
		{
			Component->Stop();
		}
		Loop = FLoop();
		Loop.Sound = Sound;
	}
	Loop.Volume = FMath::Clamp(Volume, 0.0f, 2.0f);
	if (Loop.Volume > 0.01f && !Loop.bCounted)
	{
		// Counted when it becomes audible, even headless, like any other sound.
		Loop.bCounted = true;
		++PlayCounts[static_cast<int32>(Sound)];
	}
}

void UMadAudioSubsystem::TickLoops()
{
	UWorld* World = GetWorld();
	const bool bDevice = World != nullptr && World->GetAudioDeviceRaw() != nullptr && bReady;
	for (FLoop& Loop : Loops)
	{
		if (Loop.Sound == EMadSound::Num)
		{
			continue;
		}
		UAudioComponent* Component = Loop.Component.Get();
		const float Volume = Loop.Volume * FMath::Clamp(CVarVolume.GetValueOnGameThread(), 0.0f, 1.0f);
		// The same threshold SetLoop counts at, so a loop fading in is counted once.
		if (Loop.Volume <= 0.01f || Volume <= 0.0f)
		{
			if (Component != nullptr)
			{
				Component->Stop();
				Loop.Component.Reset();
				Loop.Wave.Reset();
			}
			Loop.bCounted = false;
			continue;
		}
		if (!bDevice)
		{
			continue;
		}

		const TArray<int16>& Samples = GetVariation(Loop.Sound, 0);
		if (Samples.Num() == 0)
		{
			continue;
		}
		USoundWaveProcedural* Wave = Loop.Wave.Get();
		if (Component == nullptr || Wave == nullptr)
		{
			Wave = NewObject<USoundWaveProcedural>(this);
			Wave->SetSampleRate(MadFall::Synth::SampleRate);
			Wave->NumChannels = 1;
			Wave->Duration = INDEFINITELY_LOOPING_DURATION;
			Wave->SoundGroup = SOUNDGROUP_Default;
			Wave->bLooping = false;
			LoopWaves.Add(Wave);
			Component = UGameplayStatics::SpawnSound2D(World, Wave, Volume, 1.0f, 0.0f, nullptr, true, false);
			Loop.Component = Component;
			Loop.Wave = Wave;
		}
		if (Component == nullptr)
		{
			continue;
		}
		// Keep at least one copy queued ahead of the playhead.
		if (Wave->GetAvailableAudioByteCount() < Samples.Num() * static_cast<int32>(sizeof(int16)))
		{
			Wave->QueueAudio(reinterpret_cast<const uint8*>(Samples.GetData()), Samples.Num() * sizeof(int16));
		}
		Component->SetVolumeMultiplier(Volume);
	}
	LoopWaves.RemoveAll([](const TObjectPtr<UObject>& Wave) { return Wave == nullptr; });
}

void UMadAudioSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	TickLoops();
	if (Voices.Num() == 0)
	{
		return;
	}
	MAD_FRAME_SCOPE(Other);

	const double Now = FPlatformTime::Seconds();
	Voices.RemoveAll([Now](const FVoice& Voice)
	{
		UAudioComponent* Component = Voice.Component.Get();
		if (Component == nullptr)
		{
			return true;
		}
		if (Now >= Voice.StopAt)
		{
			Component->Stop();
			return true;
		}
		return false;
	});
}

USoundAttenuation* UMadAudioSubsystem::GetAttenuation()
{
	if (Attenuation == nullptr)
	{
		Attenuation = NewObject<USoundAttenuation>(this);
		FSoundAttenuationSettings& S = Attenuation->Attenuation;
		S.bAttenuate = true;
		S.bSpatialize = true;
		S.AttenuationShape = EAttenuationShape::Sphere;
		S.AttenuationShapeExtents = FVector(150.0);
		// Audible across a base and to about the edge of a zombie's hearing.
		S.FalloffDistance = 3600.0f;
		S.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
	}
	return Attenuation;
}

const TArray<int16>& UMadAudioSubsystem::GetVariation(EMadSound Sound, int32 Variation)
{
	check(bReady);
	return Cache[static_cast<int32>(Sound)][Variation];
}

void UMadAudioSubsystem::PlayAt(EMadSound Sound, const FVector& Location, float Volume)
{
	PlayInternal(Sound, &Location, Volume);
}

void UMadAudioSubsystem::Play2D(EMadSound Sound, float Volume)
{
	PlayInternal(Sound, nullptr, Volume);
}

void UMadAudioSubsystem::PlayForMaterial(EMadSound Base, FName MaterialClass, const FVector& Location, float Volume)
{
	const FMadSurfaceDefinition* Surface = MadFall::GetSurfaces().Find(MaterialClass);
	const EMadImpactKind Kind = MadFall::Synth::ParseImpactKind(Surface ? Surface->Impact : NAME_None);
	PlayInternal(MadFall::Synth::ForKind(Base, Kind), &Location, Volume);
}

void UMadAudioSubsystem::PlayInternal(EMadSound Sound, const FVector* Location, float Volume)
{
	const int32 Index = static_cast<int32>(Sound);
	if (Index < 0 || Index >= static_cast<int32>(EMadSound::Num))
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastPlayed[Index] < MinimumGap(Sound))
	{
		return;
	}
	LastPlayed[Index] = Now;
	++PlayCounts[Index];

	UWorld* World = GetWorld();
	const float FinalVolume = FMath::Clamp(Volume, 0.0f, 2.0f) * FMath::Clamp(CVarVolume.GetValueOnGameThread(), 0.0f, 1.0f);
	if (World == nullptr || FinalVolume <= 0.0f || World->GetAudioDeviceRaw() == nullptr)
	{
		return;   // counted, not heard: headless runs have no device
	}

	if (!bReady)
	{
		return;   // still synthesising (the first moments of a session)
	}
	Voices.RemoveAll([](const FVoice& Voice) { return !Voice.Component.IsValid(); });
	if (Voices.Num() >= FMath::Max(1, CVarMaxVoices.GetValueOnGameThread()))
	{
		++Dropped;
		return;
	}

	const TArray<int16>& Samples = GetVariation(Sound, static_cast<int32>(NextVariation++ % Variations));
	if (Samples.Num() == 0)
	{
		return;
	}

	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this);
	Wave->SetSampleRate(MadFall::Synth::SampleRate);
	Wave->NumChannels = 1;
	Wave->Duration = static_cast<float>(Samples.Num()) / MadFall::Synth::SampleRate;
	Wave->SoundGroup = SOUNDGROUP_Default;
	Wave->bLooping = false;
	Wave->QueueAudio(reinterpret_cast<const uint8*>(Samples.GetData()), Samples.Num() * sizeof(int16));

	// A little pitch spread on top of the variations.
	const float Pitch = FMath::FRandRange(0.94f, 1.06f);
	UAudioComponent* Component = Location
		? UGameplayStatics::SpawnSoundAtLocation(World, Wave, *Location, FRotator::ZeroRotator, FinalVolume, Pitch, 0.0f, GetAttenuation())
		: UGameplayStatics::SpawnSound2D(World, Wave, FinalVolume, Pitch);
	if (Component != nullptr)
	{
		Voices.Add(FVoice{ Component, Now + Wave->Duration / Pitch + 0.1 });
	}
}

FString UMadAudioSubsystem::DescribeStats() const
{
	TStringBuilder<1024> Builder;
	Builder.Appendf(TEXT("Audio: %d voice(s) playing, %d dropped, device %s; plays:"), Voices.Num(), Dropped,
		GetWorld() && GetWorld()->GetAudioDeviceRaw() ? TEXT("yes") : TEXT("none"));
	for (int32 Index = 0; Index < static_cast<int32>(EMadSound::Num); ++Index)
	{
		if (PlayCounts[Index] > 0)
		{
			Builder.Appendf(TEXT(" %s=%d"), MadFall::Synth::GetName(static_cast<EMadSound>(Index)), PlayCounts[Index]);
		}
	}
	return Builder.ToString();
}

static FAutoConsoleCommandWithWorld GMadAudioStatsCommand(
	TEXT("mad.audio.stats"),
	TEXT("How many times each MadFall sound has played, voices and drops."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadAudioSubsystem* Audio = World ? World->GetSubsystem<UMadAudioSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Audio->DescribeStats());
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadAudioPlayCommand(
	TEXT("mad.audio.play"),
	TEXT("mad.audio.play <sound name> - plays a sound in front of the player (hit_stone, zombie_groan, horde_horn, ...)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadAudioSubsystem* Audio = World ? World->GetSubsystem<UMadAudioSubsystem>() : nullptr;
		if (Audio == nullptr || Args.Num() < 1)
		{
			return;
		}
		for (int32 Index = 0; Index < static_cast<int32>(EMadSound::Num); ++Index)
		{
			if (Args[0].Equals(MadFall::Synth::GetName(static_cast<EMadSound>(Index)), ESearchCase::IgnoreCase))
			{
				Audio->Play2D(static_cast<EMadSound>(Index));
				return;
			}
		}
		UE_LOG(LogMadFallGameplay, Warning, TEXT("No sound named %s."), *Args[0]);
	}));

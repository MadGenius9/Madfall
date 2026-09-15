// Copyright MadFall. All Rights Reserved.

#include "MadMusicSubsystem.h"

#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "MadFallGameplay.h"
#include "MadHordeSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "Sound/SoundWave.h"

namespace
{
	TAutoConsoleVariable<float> CVarMusicVolume(
		TEXT("mad.audio.MusicVolume"), 0.6f,
		TEXT("Volume of the background music, 0..1 (0 turns it off)."));

	constexpr float FadeSeconds = 4.0f;
	/** A quiet moment between one track fading out and the next beginning. */
	constexpr double ChangeGapSeconds = 6.0;

	const TCHAR* WavePath(EMadMusicTrack Track)
	{
		switch (Track)
		{
		case EMadMusicTrack::Day:   return TEXT("/Game/Music/SW_Music_day.SW_Music_day");
		case EMadMusicTrack::Night: return TEXT("/Game/Music/SW_Music_night.SW_Music_night");
		case EMadMusicTrack::Horde: return TEXT("/Game/Music/SW_Music_horde.SW_Music_horde");
		default:                    return nullptr;
		}
	}
}

EMadMusicTrack MadFall::Music::ChooseTrack(bool bNight, bool bHordeActive)
{
	if (bHordeActive)
	{
		return EMadMusicTrack::Horde;
	}
	return bNight ? EMadMusicTrack::Night : EMadMusicTrack::Day;
}

float MadFall::Music::SilenceAfter(uint32 Seed)
{
	return 60.0f + static_cast<float>(HashCombineFast(Seed, 0x9E3779B9u) % 91u);
}

const TCHAR* MadFall::Music::GetName(EMadMusicTrack Track)
{
	switch (Track)
	{
	case EMadMusicTrack::Day:   return TEXT("day");
	case EMadMusicTrack::Night: return TEXT("night");
	case EMadMusicTrack::Horde: return TEXT("horde");
	default:                    return TEXT("none");
	}
}

bool UMadMusicSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadMusicSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadMusicSubsystem, STATGROUP_Tickables);
}

void UMadMusicSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Waves.SetNum(4);
}

void UMadMusicSubsystem::Deinitialize()
{
	if (UAudioComponent* Playing = Component.Get())
	{
		Playing->Stop();
	}
	Super::Deinitialize();
}

USoundWave* UMadMusicSubsystem::GetWave(EMadMusicTrack InTrack)
{
	const int32 Index = static_cast<int32>(InTrack);
	if (Waves[Index] == nullptr && WavePath(InTrack) != nullptr)
	{
		// Loaded on first need: a few megabytes of compressed audio, the first
		// time a track is wanted, which is the start of the session or a dusk.
		Waves[Index] = LoadObject<USoundWave>(nullptr, WavePath(InTrack));
	}
	return Waves[Index];
}

void UMadMusicSubsystem::Fade()
{
	if (UAudioComponent* Playing = Component.Get())
	{
		Playing->FadeOut(FadeSeconds, 0.0f);
	}
	Component.Reset();
	bPlaying = false;
}

void UMadMusicSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	const double Now = FPlatformTime::Seconds();
	if (Now < NextCheck)
	{
		return;
	}
	NextCheck = Now + 0.5;

	UWorld* World = GetWorld();
	const UMadWorldClockSubsystem* Clock = World ? World->GetSubsystem<UMadWorldClockSubsystem>() : nullptr;
	const UMadHordeSubsystem* Horde = World ? World->GetSubsystem<UMadHordeSubsystem>() : nullptr;
	const EMadMusicTrack Wanted = MadFall::Music::ChooseTrack(Clock != nullptr && Clock->IsNight(), Horde != nullptr && Horde->IsHordeActive());
	const float Volume = FMath::Clamp(CVarMusicVolume.GetValueOnGameThread(), 0.0f, 1.0f);

	if (Wanted != Track)
	{
		Fade();
		// A horde arriving should be heard at once; dusk and dawn can wait for
		// the old theme to die away.
		StartAt = Now + (Wanted == EMadMusicTrack::Horde || Track == EMadMusicTrack::None ? 0.0 : ChangeGapSeconds);
		Track = Wanted;
	}

	UAudioComponent* Playing = Component.Get();
	if (Playing != nullptr)
	{
		if (Playing->IsPlaying())
		{
			Playing->SetVolumeMultiplier(FMath::Max(Volume, 0.001f));
			return;
		}
		// Played through: a spell of quiet before it comes round again.
		Component.Reset();
		bPlaying = false;
		StartAt = Now + MadFall::Music::SilenceAfter(++Played);
		return;
	}

	if (Now < StartAt || Volume <= 0.0f)
	{
		return;
	}
	++Starts[static_cast<int32>(Track)];
	StartAt = TNumericLimits<double>::Max();   // until this start finishes or the track changes
	USoundWave* Wave = GetWave(Track);
	if (Wave == nullptr || World == nullptr || World->GetAudioDeviceRaw() == nullptr)
	{
		// Counted, not heard: headless runs have no device. Try again after a silence.
		StartAt = Now + MadFall::Music::SilenceAfter(++Played);
		return;
	}
	Playing = UGameplayStatics::SpawnSound2D(World, Wave, Volume, 1.0f, 0.0f, nullptr, false, false);
	if (Playing != nullptr)
	{
		Playing->FadeIn(FadeSeconds, Volume);
		Component = Playing;
		bPlaying = true;
	}
}

FString UMadMusicSubsystem::DescribeStatus() const
{
	return FString::Printf(TEXT("Music: %s, %s; starts day=%d night=%d horde=%d"),
		MadFall::Music::GetName(Track), bPlaying ? TEXT("playing") : TEXT("quiet"),
		Starts[1], Starts[2], Starts[3]);
}

static FAutoConsoleCommandWithWorld GMadMusicStatusCommand(
	TEXT("mad.music.status"),
	TEXT("Which music track is wanted, whether it is playing, and how often each has started."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadMusicSubsystem* Music = World ? World->GetSubsystem<UMadMusicSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Music->DescribeStatus());
		}
	}));

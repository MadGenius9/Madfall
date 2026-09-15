// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadMusicSubsystem.generated.h"

class UAudioComponent;
class USoundWave;

UENUM()
enum class EMadMusicTrack : uint8
{
	None,
	Day,
	Night,
	Horde,
};

namespace MadFall::Music
{
	/** The track the moment calls for: the horde's while one is attacking, otherwise night or day by the clock. */
	MADFALLGAMEPLAY_API EMadMusicTrack ChooseTrack(bool bNight, bool bHordeActive);

	/** Seconds of quiet after a day or night track plays through, 60-150 varied by Seed. */
	MADFALLGAMEPLAY_API float SilenceAfter(uint32 Seed);

	/** "day", "night", "horde" or "none". */
	MADFALLGAMEPLAY_API const TCHAR* GetName(EMadMusicTrack Track);
}

/**
 * Background music: an empty-city theme by day, an unsettling one by night, and
 * a driving one while a horde attacks, from /Game/Music (CC0, see
 * Scripts/prepare_audio.py).
 *
 * WHY SILENCE BETWEEN TRACKS: a hundred-second theme on repeat for a whole day
 * is wallpaper at best and grating at worst. Day and night tracks play through
 * and then leave a minute or two of the world's own sound; the horde track
 * loops for as long as the horde lasts, because that is when the tension should
 * not let up. A change of track fades the old one out over four seconds and,
 * unless a horde has just arrived, lets a moment pass before the new one.
 *
 * Volume is `mad.audio.MusicVolume` (a menu setting), separate from effects.
 * Track changes are counted with no audio device too, for headless checks:
 * `mad.music.status`.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadMusicSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual TStatId GetStatId() const override;

	EMadMusicTrack GetTrack() const { return Track; }
	int32 GetStarts(EMadMusicTrack InTrack) const { return Starts[static_cast<int32>(InTrack)]; }
	FString DescribeStatus() const;

private:
	USoundWave* GetWave(EMadMusicTrack InTrack);
	void Fade();

	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundWave>> Waves;

	TWeakObjectPtr<UAudioComponent> Component;
	EMadMusicTrack Track = EMadMusicTrack::None;
	/** Real seconds (game time stops in the pause menu, music does not) before the current track may start. */
	double StartAt = 0.0;
	bool bPlaying = false;
	double NextCheck = 0.0;
	uint32 Played = 0;
	int32 Starts[4] = {};
};

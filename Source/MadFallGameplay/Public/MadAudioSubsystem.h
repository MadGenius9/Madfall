// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/StreamableManager.h"
#include "MadSynth.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include <atomic>
#include "MadAudioSubsystem.generated.h"

class UAudioComponent;
class USoundAttenuation;
class USoundWave;

/** The recordings of one sound. A struct so the array of them can be a UPROPERTY. */
USTRUCT()
struct FMadSoundRecordings
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundWave>> Waves;

	/** The last one played, so the same recording is not heard twice in a row. */
	int32 Last = INDEX_NONE;
};

/**
 * Plays MadFall's sound effects: recordings where they exist, synthesised
 * voices otherwise.
 *
 * RECORDINGS: every USoundWave in /Game/Audio/<sound name>/ (the names
 * MadFall::Synth::GetName gives, e.g. /Game/Audio/step_wood/) is a recording
 * of that sound, loaded asynchronously when the world starts and picked at
 * random, never the same one twice running. The shipped ones are CC0 packs
 * prepared by Scripts/prepare_audio.py and imported by Scripts/import_audio.py.
 * A sound with no folder - the horde horn, the collapse rumble - keeps its
 * synthesised voice, as does every sound until the loads land, so nothing is
 * ever silent for want of an asset.
 *
 * SYNTHESISED:
 * Sounds are generated once per variation (MadFall::Synth, a few variations
 * each, on first use) and played through a USoundWaveProcedural fed that PCM.
 * A procedural wave keeps its voice until stopped, so every play is tracked and
 * stopped when its samples run out; a cap on simultaneous voices and a minimum
 * gap per sound keep a horde from drowning the mix.
 *
 * Game code says what happened (a block of some material class was hit, a
 * zombie attacked) and this decides what it sounds like, so a surface's
 * "impact" in JSON re-voices every block of its class.
 *
 * Every play is counted per sound even with no audio device (headless CI),
 * which is how the gates check that game events make sound: `mad.audio.stats`.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadAudioSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual TStatId GetStatId() const override;

	/** A positional sound. Volume multiplies mad.audio.Volume. */
	void PlayAt(EMadSound Sound, const FVector& Location, float Volume = 1.0f);

	/** A non-positional sound (interface, the survivor's own voice). */
	void Play2D(EMadSound Sound, float Volume = 1.0f);

	/** Hit, Break, Step or Creak for the material class a block uses. */
	void PlayForMaterial(EMadSound Base, FName MaterialClass, const FVector& Location, float Volume = 1.0f);

	/**
	 * A looping ambient sound on one of a few channels (weather uses 0 for rain,
	 * 1 for wind). Volume 0 stops it; changing the sound restarts the channel.
	 * The loop is kept fed by re-queueing its samples before they run out.
	 */
	void SetLoop(int32 Channel, EMadSound Sound, float Volume);

	int32 GetPlayCount(EMadSound Sound) const { return PlayCounts[static_cast<int32>(Sound)]; }

	/** Recordings loaded for a sound; 0 while it plays synthesised. */
	int32 GetRecordingCount(EMadSound Sound) const;

	/** Where a sound's recordings live: /Game/Audio/<sound name>. */
	static FString GetRecordingFolder(EMadSound Sound);

	/** The sound a recordings folder name belongs to, or EMadSound::Num. */
	static EMadSound FindSoundByName(const FString& Name);
	FString DescribeStats() const;

private:
	void PlayInternal(EMadSound Sound, const FVector* Location, float Volume);

	/** Finds every recording under /Game/Audio and starts loading them. */
	void LoadRecordings();

	/** A recording to play for a sound, or null to synthesise. */
	USoundWave* PickRecording(EMadSound Sound);
	const TArray<int16>& GetVariation(EMadSound Sound, int32 Variation);
	USoundAttenuation* GetAttenuation();

	struct FLoop
	{
		TWeakObjectPtr<UAudioComponent> Component;
		TWeakObjectPtr<class USoundWaveProcedural> Wave;
		EMadSound Sound = EMadSound::Num;
		float Volume = 0.0f;
		bool bCounted = false;
	};
	static constexpr int32 LoopChannels = 4;
	FLoop Loops[LoopChannels];
	void TickLoops();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> LoopWaves;

	struct FVoice
	{
		TWeakObjectPtr<UAudioComponent> Component;
		double StopAt = 0.0;
	};
	TArray<FVoice> Voices;

	static constexpr int32 Variations = 4;
	/** Filled by a worker at startup; the game thread reads it only once bReady is set. */
	TArray<int16> Cache[static_cast<int32>(EMadSound::Num)][Variations];
	std::atomic<bool> bReady{ false };
	UE::Tasks::FTask SynthTask;
	int32 PlayCounts[static_cast<int32>(EMadSound::Num)] = {};
	double LastPlayed[static_cast<int32>(EMadSound::Num)] = {};
	int32 Dropped = 0;
	uint32 NextVariation = 0;

	UPROPERTY(Transient)
	TObjectPtr<USoundAttenuation> Attenuation;

	/** One entry per EMadSound. */
	UPROPERTY(Transient)
	TArray<FMadSoundRecordings> Recordings;

	TSharedPtr<FStreamableHandle> RecordingsHandle;
};

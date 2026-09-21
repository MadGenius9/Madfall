// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** What a surface sounds like when struck. Surfaces choose one with "impact". */
enum class EMadImpactKind : uint8
{
	Stone,
	Wood,
	Dirt,
	Metal,
	Foliage,
	Num
};

/** Every sound MadFall synthesises. */
enum class EMadSound : uint8
{
	// Per impact kind, in EMadImpactKind order.
	Hit, HitWood, HitDirt, HitMetal, HitFoliage,
	Break, BreakWood, BreakDirt, BreakMetal, BreakFoliage,
	Step, StepWood, StepDirt, StepMetal, StepFoliage,

	Place,
	FleshHit,
	ZombieGroan,
	ZombieAttack,
	PlayerHurt,
	Collapse,
	Pickup,
	CraftDone,
	HordeHorn,
	UIClick,
	BowRelease,
	/** Seamless two-second loops, for weather. */
	RainLoop,
	WindLoop,
	Thunder,
	ZombieScream,
	ZombieSpit,
	/** A strained structure, per impact kind: wood creaks, metal groans, stone grinds and ticks, dirt trickles. */
	Creak, CreakWood, CreakDirt, CreakMetal, CreakFoliage,
	/** A firearm. Appended last so no existing sound's index or folder moves. */
	Gunshot,
	Num
};

namespace MadFall::Synth
{
	inline constexpr int32 SampleRate = 22050;

	/** The per-kind variant of a Hit, Break, Step or Creak sound. */
	MADFALLGAMEPLAY_API EMadSound ForKind(EMadSound Base, EMadImpactKind Kind);

	/** "stone", "wood", "dirt", "metal", "foliage"; anything else is stone. */
	MADFALLGAMEPLAY_API EMadImpactKind ParseImpactKind(FName Name);

	MADFALLGAMEPLAY_API const TCHAR* GetName(EMadSound Sound);

	/**
	 * Mono 16-bit PCM at SampleRate for one variation of a sound.
	 *
	 * No audio assets exist, and every effect is built here from noise, sine and
	 * sawtooth oscillators, one-pole filters and envelopes: a struck stone is a
	 * short bright noise burst over a low knock, wood a damped resonance, metal
	 * inharmonic partials that ring, a groan a vibrato sawtooth through a low
	 * pass. The seed picks the variation, so repeated hits are not identical.
	 * Output is peak-normalised to 80% of full scale.
	 */
	MADFALLGAMEPLAY_API void Generate(EMadSound Sound, uint32 Seed, TArray<int16>& OutSamples);
}

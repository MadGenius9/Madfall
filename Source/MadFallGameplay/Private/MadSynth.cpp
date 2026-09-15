// Copyright MadFall. All Rights Reserved.

#include "MadSynth.h"

namespace
{
	constexpr float Fs = static_cast<float>(MadFall::Synth::SampleRate);

	/** xorshift32 white noise in [-1, 1]. */
	struct FNoise
	{
		uint32 State;
		explicit FNoise(uint32 Seed) : State(Seed ? Seed : 0x9E3779B9u) {}
		float Next()
		{
			State ^= State << 13;
			State ^= State >> 17;
			State ^= State << 5;
			return static_cast<float>(State & 0xFFFFFF) / 8388607.5f - 1.0f;
		}
		float Range(float Min, float Max) { return Min + (Next() * 0.5f + 0.5f) * (Max - Min); }
	};

	/** One-pole low pass. */
	struct FLowPass
	{
		float A;
		float Y = 0.0f;
		explicit FLowPass(float Cutoff) : A(1.0f - FMath::Exp(-2.0f * UE_PI * Cutoff / Fs)) {}
		float Process(float X) { Y += A * (X - Y); return Y; }
	};

	float Decay(float T, float Tau) { return FMath::Exp(-T / FMath::Max(Tau, 1e-4f)); }
	float Attack(float T, float Seconds) { return Seconds <= 0.0f ? 1.0f : FMath::Min(1.0f, T / Seconds); }

	/** Adds a struck sound of one kind starting at Offset, scaled by Gain. */
	void AddImpact(TArray<float>& Out, int32 Offset, EMadImpactKind Kind, float Gain, FNoise& Rng, float Brightness = 1.0f)
	{
		float Duration = 0.12f;
		switch (Kind)
		{
		case EMadImpactKind::Wood:    Duration = 0.16f; break;
		case EMadImpactKind::Dirt:    Duration = 0.12f; break;
		case EMadImpactKind::Metal:   Duration = 0.45f; break;
		case EMadImpactKind::Foliage: Duration = 0.14f; break;
		default: break;
		}
		const int32 Count = static_cast<int32>(Duration * Fs);
		if (Out.Num() < Offset + Count)
		{
			Out.SetNumZeroed(Offset + Count);
		}

		FLowPass Low(Kind == EMadImpactKind::Dirt ? 450.0f * Brightness : (Kind == EMadImpactKind::Stone ? 2400.0f * Brightness : 1400.0f * Brightness));
		FLowPass High(Kind == EMadImpactKind::Foliage ? 1800.0f : 60.0f);
		const float Body = Kind == EMadImpactKind::Wood ? Rng.Range(170.0f, 260.0f) : Rng.Range(80.0f, 140.0f);
		const float Partials[4] = { Rng.Range(480.0f, 560.0f), Rng.Range(1250.0f, 1400.0f), Rng.Range(2050.0f, 2250.0f), Rng.Range(3100.0f, 3300.0f) };

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float T = static_cast<float>(Index) / Fs;
			const float White = Rng.Next();
			float Sample = 0.0f;
			switch (Kind)
			{
			case EMadImpactKind::Stone:
				Sample = Low.Process(White) * Decay(T, 0.022f) * 1.6f
					+ FMath::Sin(UE_TWO_PI * Body * T) * Decay(T, 0.03f) * 0.35f;
				break;
			case EMadImpactKind::Wood:
				Sample = (FMath::Sin(UE_TWO_PI * Body * T) + 0.45f * FMath::Sin(UE_TWO_PI * Body * 2.3f * T)) * Decay(T, 0.05f)
					+ Low.Process(White) * Decay(T, 0.015f) * 0.9f;
				break;
			case EMadImpactKind::Dirt:
				Sample = Low.Process(White) * Decay(T, 0.035f) * 2.2f
					+ FMath::Sin(UE_TWO_PI * Body * T) * Decay(T, 0.045f) * 0.4f;
				break;
			case EMadImpactKind::Metal:
				for (int32 P = 0; P < 4; ++P)
				{
					Sample += FMath::Sin(UE_TWO_PI * Partials[P] * T) * Decay(T, 0.22f / (1.0f + P * 0.6f)) * (0.5f / (1.0f + P));
				}
				Sample += Low.Process(White) * Decay(T, 0.006f) * 0.8f;
				break;
			case EMadImpactKind::Foliage:
			{
				// High-passed rustle with a crackly amplitude.
				const float Band = White - High.Process(White);
				Sample = Band * Decay(T, 0.05f) * (0.6f + 0.4f * FMath::Abs(Rng.Next()));
				break;
			}
			default:
				break;
			}
			Out[Offset + Index] += Sample * Gain;
		}
	}

	void Tone(TArray<float>& Out, int32 Offset, float Seconds, float StartHz, float EndHz, float Tau, float Gain)
	{
		const int32 Count = static_cast<int32>(Seconds * Fs);
		if (Out.Num() < Offset + Count)
		{
			Out.SetNumZeroed(Offset + Count);
		}
		float Phase = 0.0f;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float T = static_cast<float>(Index) / Fs;
			const float Hz = FMath::Lerp(StartHz, EndHz, T / Seconds);
			Phase += UE_TWO_PI * Hz / Fs;
			Out[Offset + Index] += FMath::Sin(Phase) * Decay(T, Tau) * Attack(T, 0.003f) * Gain;
		}
	}

	/** Two-pole resonator: rings at Hz, dying away over roughly Tau seconds, when struck. */
	struct FResonator
	{
		float B1 = 0.0f;
		float B2 = 0.0f;
		float Gain = 0.0f;
		float Y1 = 0.0f;
		float Y2 = 0.0f;
		FResonator(float Hz, float Tau)
		{
			const float R = FMath::Exp(-1.0f / (FMath::Max(Tau, 1e-4f) * Fs));
			B1 = 2.0f * R * FMath::Cos(UE_TWO_PI * Hz / Fs);
			B2 = -R * R;
			Gain = 1.0f - R;
		}
		float Process(float X)
		{
			const float Y = X * Gain + B1 * Y1 + B2 * Y2;
			Y2 = Y1;
			Y1 = Y;
			return Y;
		}
	};

	/**
	 * A strained joint. Wood and metal are stick-slip: the joint grips, strain
	 * builds, it slips with a tick that rings the member, faster as it gives -
	 * a pulse train of wandering rate through resonators (short and woody,
	 * long and low for a steel groan). Stone does not creak; it grinds, with
	 * sparse sharp ticks of cracking. Dirt trickles.
	 */
	void Creak(TArray<float>& Out, EMadImpactKind Kind, FNoise& Rng)
	{
		const bool bMetal = Kind == EMadImpactKind::Metal;
		const bool bStick = bMetal || Kind == EMadImpactKind::Wood || Kind == EMadImpactKind::Foliage;
		const float Seconds = bMetal ? Rng.Range(1.0f, 1.4f) : Rng.Range(0.65f, 1.0f);
		const int32 Count = static_cast<int32>(Seconds * Fs);
		Out.SetNumZeroed(Count);

		const float Body = bMetal ? Rng.Range(130.0f, 185.0f)
			: Kind == EMadImpactKind::Foliage ? Rng.Range(520.0f, 700.0f)
			: bStick ? Rng.Range(260.0f, 420.0f) : Rng.Range(900.0f, 1400.0f);
		FResonator R1(Body, bMetal ? 0.22f : (bStick ? 0.035f : 0.004f));
		FResonator R2(Body * (bMetal ? 2.76f : 2.3f), bMetal ? 0.1f : 0.015f);
		FResonator R3(Body * 5.4f, bMetal ? 0.04f : 0.006f);
		FLowPass Grind(bStick ? 900.0f : 220.0f);
		FLowPass Trickle(2400.0f);

		const float StartRate = bMetal ? Rng.Range(38.0f, 48.0f) : Rng.Range(14.0f, 22.0f);
		const float EndRate = bMetal ? Rng.Range(22.0f, 30.0f) : Rng.Range(45.0f, 70.0f);
		const float Wobble = Rng.Range(2.0f, 4.5f);
		const float WobblePhase = Rng.Range(0.0f, UE_TWO_PI);
		float Phase = 0.0f;
		float Brown = 0.0f;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float T = static_cast<float>(Index) / Fs;
			const float Progress = T / Seconds;
			const float Envelope = Attack(T, bMetal ? 0.15f : 0.06f) * FMath::Min(1.0f, (1.0f - Progress) * 3.0f);
			const float White = Rng.Next();
			float Excite = 0.0f;
			if (bStick)
			{
				const float Rate = FMath::Lerp(StartRate, EndRate, Progress) * (1.0f + 0.25f * FMath::Sin(UE_TWO_PI * Wobble * T + WobblePhase));
				Phase += Rate / Fs;
				if (Phase >= 1.0f)
				{
					Phase -= 1.0f;
					Excite = Rng.Range(0.5f, 1.0f);
				}
			}
			else
			{
				// Cracking ticks, thickening toward the middle of the sound.
				const float Density = Kind == EMadImpactKind::Dirt ? 0.004f : 0.0015f;
				Excite = Rng.Range(0.0f, 1.0f) < Density * (0.4f + FMath::Sin(UE_PI * Progress)) ? Rng.Range(0.4f, 1.0f) : 0.0f;
			}
			Brown = FMath::Clamp(Brown + White * 0.06f, -1.0f, 1.0f) * 0.997f;
			float Sample = R1.Process(Excite) * 1.0f + R2.Process(Excite) * 0.45f + R3.Process(Excite) * 0.2f;
			if (!bStick)
			{
				Sample += Grind.Process(Brown) * (Kind == EMadImpactKind::Dirt ? 0.35f : 0.6f);
				if (Kind == EMadImpactKind::Dirt)
				{
					Sample += (White - Trickle.Process(White)) * Excite * 0.8f;
				}
			}
			else
			{
				// The rub between slips: a little filtered friction noise.
				Sample += Grind.Process(White) * (bMetal ? 0.02f : 0.04f);
			}
			Out[Index] = Sample * Envelope;
		}
	}

	/** A voiced growl: vibrato sawtooth through a low pass, plus breath. */
	void Voice(TArray<float>& Out, float Seconds, float Hz, float EndHz, float Cutoff, float AttackSeconds, FNoise& Rng)
	{
		const int32 Count = static_cast<int32>(Seconds * Fs);
		Out.SetNumZeroed(Count);
		FLowPass Formant(Cutoff);
		FLowPass Formant2(Cutoff * 0.7f);
		FLowPass Breath(1100.0f);
		const float Vibrato = Rng.Range(4.0f, 6.5f);
		float Phase = 0.0f;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float T = static_cast<float>(Index) / Fs;
			const float Progress = T / Seconds;
			const float Pitch = FMath::Lerp(Hz, EndHz, Progress) * (1.0f + 0.045f * FMath::Sin(UE_TWO_PI * Vibrato * T));
			Phase = FMath::Fmod(Phase + Pitch / Fs, 1.0f);
			const float Saw = 2.0f * Phase - 1.0f;
			// Swell in, wobble, fade out over the last third.
			const float Envelope = Attack(T, AttackSeconds) * FMath::Min(1.0f, (1.0f - Progress) * 3.0f)
				* (0.75f + 0.25f * FMath::Sin(UE_TWO_PI * 2.3f * T));
			Out[Index] = (Formant2.Process(Formant.Process(Saw)) * 1.8f + Breath.Process(Rng.Next()) * 0.25f) * Envelope;
		}
	}
}

EMadSound MadFall::Synth::ForKind(EMadSound Base, EMadImpactKind Kind)
{
	const int32 Offset = FMath::Clamp(static_cast<int32>(Kind), 0, static_cast<int32>(EMadImpactKind::Num) - 1);
	switch (Base)
	{
	case EMadSound::Hit:   return static_cast<EMadSound>(static_cast<int32>(EMadSound::Hit) + Offset);
	case EMadSound::Break: return static_cast<EMadSound>(static_cast<int32>(EMadSound::Break) + Offset);
	case EMadSound::Step:  return static_cast<EMadSound>(static_cast<int32>(EMadSound::Step) + Offset);
	case EMadSound::Creak: return static_cast<EMadSound>(static_cast<int32>(EMadSound::Creak) + Offset);
	default:               return Base;
	}
}

EMadImpactKind MadFall::Synth::ParseImpactKind(FName Name)
{
	const FString Text = Name.ToString();
	if (Text.Equals(TEXT("wood"), ESearchCase::IgnoreCase))    { return EMadImpactKind::Wood; }
	if (Text.Equals(TEXT("dirt"), ESearchCase::IgnoreCase))    { return EMadImpactKind::Dirt; }
	if (Text.Equals(TEXT("metal"), ESearchCase::IgnoreCase))   { return EMadImpactKind::Metal; }
	if (Text.Equals(TEXT("foliage"), ESearchCase::IgnoreCase)) { return EMadImpactKind::Foliage; }
	return EMadImpactKind::Stone;
}

const TCHAR* MadFall::Synth::GetName(EMadSound Sound)
{
	static const TCHAR* Names[] = {
		TEXT("hit_stone"), TEXT("hit_wood"), TEXT("hit_dirt"), TEXT("hit_metal"), TEXT("hit_foliage"),
		TEXT("break_stone"), TEXT("break_wood"), TEXT("break_dirt"), TEXT("break_metal"), TEXT("break_foliage"),
		TEXT("step_stone"), TEXT("step_wood"), TEXT("step_dirt"), TEXT("step_metal"), TEXT("step_foliage"),
		TEXT("place"), TEXT("flesh_hit"), TEXT("zombie_groan"), TEXT("zombie_attack"), TEXT("player_hurt"),
		TEXT("collapse"), TEXT("pickup"), TEXT("craft_done"), TEXT("horde_horn"), TEXT("ui_click"), TEXT("bow_release"),
		TEXT("rain_loop"), TEXT("wind_loop"), TEXT("thunder"),
		TEXT("zombie_scream"), TEXT("zombie_spit"),
		TEXT("creak_stone"), TEXT("creak_wood"), TEXT("creak_dirt"), TEXT("creak_metal"), TEXT("creak_foliage")
	};
	static_assert(UE_ARRAY_COUNT(Names) == static_cast<int32>(EMadSound::Num), "one name per sound");
	const int32 Index = static_cast<int32>(Sound);
	return Index >= 0 && Index < UE_ARRAY_COUNT(Names) ? Names[Index] : TEXT("?");
}

void MadFall::Synth::Generate(EMadSound Sound, uint32 Seed, TArray<int16>& OutSamples)
{
	FNoise Rng(Seed * 2654435761u + static_cast<uint32>(Sound) * 40503u + 1u);
	TArray<float> Buffer;
	const int32 Index = static_cast<int32>(Sound);

	if (Index >= static_cast<int32>(EMadSound::Hit) && Index <= static_cast<int32>(EMadSound::HitFoliage))
	{
		AddImpact(Buffer, 0, static_cast<EMadImpactKind>(Index - static_cast<int32>(EMadSound::Hit)), 1.0f, Rng);
	}
	else if (Index >= static_cast<int32>(EMadSound::Break) && Index <= static_cast<int32>(EMadSound::BreakFoliage))
	{
		// A strike, then the block coming apart: smaller, duller pieces landing.
		const EMadImpactKind Kind = static_cast<EMadImpactKind>(Index - static_cast<int32>(EMadSound::Break));
		AddImpact(Buffer, 0, Kind, 1.0f, Rng, 1.2f);
		const int32 Pieces = 5 + static_cast<int32>(Rng.Range(0.0f, 4.0f));
		for (int32 Piece = 0; Piece < Pieces; ++Piece)
		{
			const float At = 0.05f + Rng.Range(0.0f, 0.35f);
			AddImpact(Buffer, static_cast<int32>(At * Fs), Kind, Rng.Range(0.15f, 0.45f) * (1.0f - At), Rng, 0.6f);
		}
	}
	else if (Index >= static_cast<int32>(EMadSound::Creak) && Index <= static_cast<int32>(EMadSound::CreakFoliage))
	{
		Creak(Buffer, static_cast<EMadImpactKind>(Index - static_cast<int32>(EMadSound::Creak)), Rng);
	}
	else if (Index >= static_cast<int32>(EMadSound::Step) && Index <= static_cast<int32>(EMadSound::StepFoliage))
	{
		AddImpact(Buffer, 0, static_cast<EMadImpactKind>(Index - static_cast<int32>(EMadSound::Step)), 0.5f, Rng, 0.35f);
		Buffer.SetNum(FMath::Min(Buffer.Num(), static_cast<int32>(0.09f * Fs)));
	}
	else
	{
		switch (Sound)
		{
		case EMadSound::Place:
			AddImpact(Buffer, 0, EMadImpactKind::Wood, 0.8f, Rng, 0.5f);
			AddImpact(Buffer, 0, EMadImpactKind::Dirt, 0.6f, Rng);
			break;
		case EMadSound::FleshHit:
			AddImpact(Buffer, 0, EMadImpactKind::Dirt, 1.0f, Rng, 0.7f);
			Tone(Buffer, 0, 0.08f, 120.0f, 70.0f, 0.03f, 0.6f);
			break;
		case EMadSound::ZombieGroan:
			Voice(Buffer, Rng.Range(0.9f, 1.4f), Rng.Range(82.0f, 118.0f), Rng.Range(70.0f, 95.0f), Rng.Range(420.0f, 620.0f), 0.18f, Rng);
			break;
		case EMadSound::ZombieAttack:
			Voice(Buffer, 0.4f, Rng.Range(140.0f, 170.0f), 95.0f, 900.0f, 0.02f, Rng);
			AddImpact(Buffer, static_cast<int32>(0.12f * Fs), EMadImpactKind::Dirt, 0.6f, Rng);
			break;
		case EMadSound::PlayerHurt:
			Voice(Buffer, 0.28f, Rng.Range(190.0f, 220.0f), 130.0f, 1100.0f, 0.01f, Rng);
			break;
		case EMadSound::Collapse:
		{
			// Brown noise rumble with cracks in its first second.
			const int32 Count = static_cast<int32>(2.2f * Fs);
			Buffer.SetNumZeroed(Count);
			FLowPass Rumble(140.0f);
			float Brown = 0.0f;
			for (int32 I = 0; I < Count; ++I)
			{
				const float T = static_cast<float>(I) / Fs;
				Brown = FMath::Clamp(Brown + Rng.Next() * 0.08f, -1.0f, 1.0f) * 0.998f;
				Buffer[I] = Rumble.Process(Brown) * 3.0f * Attack(T, 0.04f) * Decay(T, 0.75f);
			}
			for (int32 Crack = 0; Crack < 7; ++Crack)
			{
				AddImpact(Buffer, static_cast<int32>(Rng.Range(0.0f, 1.0f) * Fs), Crack % 2 ? EMadImpactKind::Stone : EMadImpactKind::Wood, Rng.Range(0.3f, 0.8f), Rng);
			}
			break;
		}
		case EMadSound::Pickup:
			Tone(Buffer, 0, 0.13f, 660.0f, 1320.0f, 0.05f, 0.8f);
			break;
		case EMadSound::CraftDone:
			Tone(Buffer, 0, 0.16f, 784.0f, 784.0f, 0.07f, 0.7f);
			Tone(Buffer, static_cast<int32>(0.11f * Fs), 0.3f, 1175.0f, 1175.0f, 0.11f, 0.7f);
			break;
		case EMadSound::HordeHorn:
		{
			// Two detuned sawtooth drones a fifth apart, swelling in.
			const int32 Count = static_cast<int32>(3.0f * Fs);
			Buffer.SetNumZeroed(Count);
			FLowPass Filter(380.0f);
			float P1 = 0.0f, P2 = 0.0f, P3 = 0.0f;
			for (int32 I = 0; I < Count; ++I)
			{
				const float T = static_cast<float>(I) / Fs;
				P1 = FMath::Fmod(P1 + 55.0f / Fs, 1.0f);
				P2 = FMath::Fmod(P2 + 55.4f / Fs, 1.0f);
				P3 = FMath::Fmod(P3 + 82.4f / Fs, 1.0f);
				const float Saw = (2.0f * P1 - 1.0f) + (2.0f * P2 - 1.0f) + 0.7f * (2.0f * P3 - 1.0f);
				const float Envelope = Attack(T, 0.9f) * FMath::Min(1.0f, (3.0f - T) / 0.8f);
				Buffer[I] = Filter.Process(Saw) * Envelope;
			}
			break;
		}
		case EMadSound::UIClick:
			Tone(Buffer, 0, 0.035f, 1900.0f, 1500.0f, 0.008f, 0.6f);
			break;
		case EMadSound::RainLoop:
		case EMadSound::WindLoop:
		{
			// Two seconds that tile: the tail is cross-faded into the head, and the
			// wind's gusting is one whole cycle long.
			const bool bRain = Sound == EMadSound::RainLoop;
			const int32 Loop = 2 * Fs;
			const int32 Fade = Fs / 4;
			TArray<float> Raw;
			Raw.SetNumZeroed(Loop + Fade);
			FLowPass Low(bRain ? 5200.0f : 520.0f);
			FLowPass Rumble(90.0f);
			for (int32 I = 0; I < Raw.Num(); ++I)
			{
				const float T = static_cast<float>(I) / Fs;
				const float White = Rng.Next();
				if (bRain)
				{
					// A hiss, with drops: sparse, sharp clicks of varying loudness.
					const float Hiss = (White - Rumble.Process(White)) * 0.35f;
					const float Drop = Rng.Range(0.0f, 1.0f) > 0.9985f ? Rng.Range(0.4f, 1.0f) : 0.0f;
					Raw[I] = Low.Process(Hiss + Drop);
				}
				else
				{
					const float Gust = 0.55f + 0.45f * FMath::Sin(UE_TWO_PI * 0.5f * T);
					Raw[I] = Low.Process(White) * Gust * 2.5f;
				}
			}
			Buffer.SetNumZeroed(Loop);
			for (int32 I = 0; I < Loop; ++I)
			{
				Buffer[I] = Raw[I];
			}
			for (int32 I = 0; I < Fade; ++I)
			{
				const float W = static_cast<float>(I) / Fade;
				Buffer[I] = Raw[I] * W + Raw[Loop + I] * (1.0f - W);
			}
			break;
		}
		case EMadSound::Thunder:
		{
			// A crack, then a long brown-noise rumble rolling away.
			const int32 Count = static_cast<int32>(3.0f * Fs);
			Buffer.SetNumZeroed(Count);
			FLowPass Roll(160.0f);
			float Brown = 0.0f;
			for (int32 I = 0; I < Count; ++I)
			{
				const float T = static_cast<float>(I) / Fs;
				Brown = FMath::Clamp(Brown + Rng.Next() * 0.08f, -1.0f, 1.0f) * 0.998f;
				const float Crack = T < 0.25f ? Rng.Next() * Decay(T, 0.06f) * 1.4f : 0.0f;
				const float Envelope = FMath::Min(1.0f, T / 0.08f) * Decay(FMath::Max(0.0f, T - 0.3f), 1.1f);
				Buffer[I] = Crack + Roll.Process(Brown) * Envelope * 3.0f;
			}
			break;
		}
		case EMadSound::ZombieScream:
			// A long rising shriek: a high voice with lots of breath in it.
			Voice(Buffer, Rng.Range(1.1f, 1.5f), Rng.Range(420.0f, 520.0f), Rng.Range(620.0f, 760.0f), 2600.0f, 0.6f, Rng);
			break;
		case EMadSound::ZombieSpit:
			Tone(Buffer, 0, 0.18f, 380.0f, 140.0f, 0.05f, 0.5f);
			AddImpact(Buffer, 0, EMadImpactKind::Foliage, 1.0f, Rng, 0.6f);
			break;
		case EMadSound::BowRelease:
			// The string's low twang, and the rustle of the arrow leaving.
			Tone(Buffer, 0, 0.22f, Rng.Range(150.0f, 175.0f), 120.0f, 0.06f, 0.8f);
			AddImpact(Buffer, 0, EMadImpactKind::Foliage, 0.7f, Rng);
			break;
		default:
			break;
		}
	}

	float Peak = 0.0f;
	for (float Sample : Buffer)
	{
		Peak = FMath::Max(Peak, FMath::Abs(Sample));
	}
	const float Scale = Peak > 1e-6f ? 0.8f * 32767.0f / Peak : 0.0f;

	OutSamples.SetNumUninitialized(Buffer.Num());
	for (int32 I = 0; I < Buffer.Num(); ++I)
	{
		OutSamples[I] = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Buffer[I] * Scale), -32767, 32767));
	}
}

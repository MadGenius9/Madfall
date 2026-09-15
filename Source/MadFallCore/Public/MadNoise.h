// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic noise for world generation.
 *
 * WHY NOT FMath::PerlinNoise3D:
 * The engine's Perlin uses a fixed permutation table with no seed parameter, so
 * every MadFall world would have the same terrain shape - the seed could only
 * offset the sample position, which produces the same landscape viewed from a
 * different place. Seeding the hash instead gives genuinely different worlds.
 *
 * WHY AN INTEGER HASH RATHER THAN A PERMUTATION TABLE:
 * A world must generate identically on every machine, forever. A table is fine
 * for that, but it is also 512 bytes of cache pressure in the innermost loop of
 * the generator, hit several times per voxel. The hash below is pure integer
 * arithmetic on uint32 with defined wrapping behaviour, which is exactly as
 * reproducible and touches no memory.
 *
 * Everything here is a pure function. No state, no allocation, thread-safe.
 */
namespace MadFall::Noise
{
	/**
	 * Integer avalanche hash (the finalizer from MurmurHash3).
	 *
	 * Chosen because it passes avalanche tests - flipping one input bit changes
	 * about half the output bits - which is what stops terrain from showing
	 * visible grid artifacts along the axes.
	 */
	FORCEINLINE constexpr uint32 HashInt(uint32 Value)
	{
		Value ^= Value >> 16;
		Value *= 0x7FEB352Du;
		Value ^= Value >> 15;
		Value *= 0x846CA68Bu;
		Value ^= Value >> 16;
		return Value;
	}

	/** Hashes a 3D lattice point with a world seed. */
	FORCEINLINE constexpr uint32 Hash3(int32 X, int32 Y, int32 Z, uint32 Seed)
	{
		// Large odd multipliers keep the three axes from aliasing onto each
		// other, which would make (1,2,3) and (3,2,1) hash alike and show up as
		// diagonal streaks in the terrain.
		uint32 Result = Seed;
		Result = HashInt(Result ^ (static_cast<uint32>(X) * 0x9E3779B1u));
		Result = HashInt(Result ^ (static_cast<uint32>(Y) * 0x85EBCA77u));
		Result = HashInt(Result ^ (static_cast<uint32>(Z) * 0xC2B2AE3Du));
		return Result;
	}

	FORCEINLINE constexpr uint32 Hash2(int32 X, int32 Y, uint32 Seed)
	{
		return Hash3(X, Y, 0, Seed);
	}

	/** Uniform float in [0, 1) from a hash. */
	FORCEINLINE constexpr float HashToUnitFloat(uint32 Hash)
	{
		// 24 bits is exactly the float mantissa, so every result is exactly
		// representable and the distribution has no gaps or clumps.
		return static_cast<float>(Hash >> 8) * (1.0f / 16777216.0f);
	}

	/** Quintic smoothstep. Perlin's improved fade: zero first AND second derivative at the ends. */
	FORCEINLINE constexpr float Fade(float T)
	{
		return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f);
	}

	FORCEINLINE constexpr float Lerp(float A, float B, float T)
	{
		return A + (B - A) * T;
	}

	/**
	 * Gradient (Perlin-style) noise in 3D. Returns roughly [-1, 1].
	 *
	 * Gradients come from the 12 edge-midpoint vectors of a cube, the set Perlin
	 * settled on in his 2002 revision: they are cheap to select (no
	 * normalisation, no table lookup) and avoid the directional clumping that a
	 * naive random unit vector produces at low octave counts.
	 */
	MADFALLCORE_API float Gradient3D(float X, float Y, float Z, uint32 Seed);

	/** 2D gradient noise. Used for heightmaps and climate fields, where 3D would be wasted work. */
	MADFALLCORE_API float Gradient2D(float X, float Y, uint32 Seed);

	/** Parameters for a fractal sum. */
	struct MADFALLCORE_API FFractalSettings
	{
		/** How many noise layers to sum. More octaves, more detail, linear cost. */
		int32 Octaves = 4;

		/** Frequency multiplier per octave. 2.0 is the classic choice. */
		float Lacunarity = 2.0f;

		/** Amplitude multiplier per octave. Below 0.5 gives smooth terrain, above gives rough. */
		float Gain = 0.5f;

		/** Base frequency, in cycles per voxel. */
		float Frequency = 0.01f;
	};

	/** Fractal Brownian motion. Result is normalised to roughly [-1, 1]. */
	MADFALLCORE_API float FBM2D(float X, float Y, uint32 Seed, const FFractalSettings& Settings);

	MADFALLCORE_API float FBM3D(float X, float Y, float Z, uint32 Seed, const FFractalSettings& Settings);

	/**
	 * Ridged multifractal: 1 - |noise|, summed.
	 *
	 * Produces creases rather than blobs, which is what makes mountain ridges
	 * and - in 3D - long connected cave tunnels instead of disconnected bubbles.
	 */
	MADFALLCORE_API float Ridged3D(float X, float Y, float Z, uint32 Seed, const FFractalSettings& Settings);

	MADFALLCORE_API float Ridged2D(float X, float Y, uint32 Seed, const FFractalSettings& Settings);

	/**
	 * A deterministic random stream for a single chunk.
	 *
	 * Seeded from the world seed and the chunk coordinate, so ore placement in
	 * one chunk never depends on which chunks were generated before it. A shared
	 * global RNG would make the world depend on the player's path through it.
	 */
	class MADFALLCORE_API FChunkRandom
	{
	public:
		FChunkRandom(uint32 WorldSeed, int32 ChunkX, int32 ChunkY, int32 ChunkZ, uint32 Salt = 0)
			: State(Hash3(ChunkX, ChunkY, ChunkZ, WorldSeed ^ HashInt(Salt)))
		{
			// A zero state would make the generator emit zeros forever.
			if (State == 0)
			{
				State = 0x9E3779B9u;
			}
		}

		/** xorshift32: small, fast, and adequate for scatter decisions. */
		FORCEINLINE uint32 NextUInt()
		{
			State ^= State << 13;
			State ^= State >> 17;
			State ^= State << 5;
			return State;
		}

		FORCEINLINE float NextFloat() { return HashToUnitFloat(NextUInt()); }

		/** Uniform in [Min, Max]. */
		FORCEINLINE int32 NextRange(int32 Min, int32 Max)
		{
			checkSlow(Max >= Min);
			return Min + static_cast<int32>(NextUInt() % static_cast<uint32>(Max - Min + 1));
		}

		FORCEINLINE bool Chance(float Probability) { return NextFloat() < Probability; }

	private:
		uint32 State;
	};
}

// Copyright MadFall. All Rights Reserved.

#include "MadNoise.h"

namespace MadFall::Noise
{
	namespace
	{
		/**
		 * Perlin's 12 gradient vectors: the midpoints of a cube's edges.
		 *
		 * A 13th duplicate entry is the usual trick for masking with 15 instead
		 * of taking a modulo by 12; we index with % 12 directly because the hash
		 * is already well distributed and the division is off the hot path once
		 * the compiler turns it into a multiply.
		 */
		constexpr float Gradients3D[12][3] =
		{
			{ 1,  1,  0}, {-1,  1,  0}, { 1, -1,  0}, {-1, -1,  0},
			{ 1,  0,  1}, {-1,  0,  1}, { 1,  0, -1}, {-1,  0, -1},
			{ 0,  1,  1}, { 0, -1,  1}, { 0,  1, -1}, { 0, -1, -1}
		};

		FORCEINLINE float DotGradient3D(uint32 Hash, float X, float Y, float Z)
		{
			const float* G = Gradients3D[Hash % 12];
			return G[0] * X + G[1] * Y + G[2] * Z;
		}

		FORCEINLINE float DotGradient2D(uint32 Hash, float X, float Y)
		{
			// Eight directions: the four axes and the four diagonals. Fewer than
			// 3D needs, and enough that 2D heightfields show no axis bias.
			switch (Hash & 7)
			{
			case 0: return  X + Y;
			case 1: return -X + Y;
			case 2: return  X - Y;
			case 3: return -X - Y;
			case 4: return  X;
			case 5: return -X;
			case 6: return  Y;
			default: return -Y;
			}
		}

		FORCEINLINE int32 FastFloor(float Value)
		{
			const int32 Truncated = static_cast<int32>(Value);
			return (Value < static_cast<float>(Truncated)) ? Truncated - 1 : Truncated;
		}
	}

	float Gradient3D(float X, float Y, float Z, uint32 Seed)
	{
		const int32 X0 = FastFloor(X);
		const int32 Y0 = FastFloor(Y);
		const int32 Z0 = FastFloor(Z);

		const float Fx = X - static_cast<float>(X0);
		const float Fy = Y - static_cast<float>(Y0);
		const float Fz = Z - static_cast<float>(Z0);

		const float Ux = Fade(Fx);
		const float Uy = Fade(Fy);
		const float Uz = Fade(Fz);

		const float N000 = DotGradient3D(Hash3(X0,     Y0,     Z0,     Seed), Fx,        Fy,        Fz);
		const float N100 = DotGradient3D(Hash3(X0 + 1, Y0,     Z0,     Seed), Fx - 1.0f, Fy,        Fz);
		const float N010 = DotGradient3D(Hash3(X0,     Y0 + 1, Z0,     Seed), Fx,        Fy - 1.0f, Fz);
		const float N110 = DotGradient3D(Hash3(X0 + 1, Y0 + 1, Z0,     Seed), Fx - 1.0f, Fy - 1.0f, Fz);
		const float N001 = DotGradient3D(Hash3(X0,     Y0,     Z0 + 1, Seed), Fx,        Fy,        Fz - 1.0f);
		const float N101 = DotGradient3D(Hash3(X0 + 1, Y0,     Z0 + 1, Seed), Fx - 1.0f, Fy,        Fz - 1.0f);
		const float N011 = DotGradient3D(Hash3(X0,     Y0 + 1, Z0 + 1, Seed), Fx,        Fy - 1.0f, Fz - 1.0f);
		const float N111 = DotGradient3D(Hash3(X0 + 1, Y0 + 1, Z0 + 1, Seed), Fx - 1.0f, Fy - 1.0f, Fz - 1.0f);

		const float X00 = Lerp(N000, N100, Ux);
		const float X10 = Lerp(N010, N110, Ux);
		const float X01 = Lerp(N001, N101, Ux);
		const float X11 = Lerp(N011, N111, Ux);

		const float Y0Lerp = Lerp(X00, X10, Uy);
		const float Y1Lerp = Lerp(X01, X11, Uy);

		// Perlin 3D peaks near 1/sqrt(3) of the gradient length; scaling by
		// ~1.15 puts the practical range close to [-1, 1] so callers can treat
		// it as normalised without clamping away detail.
		return Lerp(Y0Lerp, Y1Lerp, Uz) * 1.1547005f;
	}

	float Gradient2D(float X, float Y, uint32 Seed)
	{
		const int32 X0 = FastFloor(X);
		const int32 Y0 = FastFloor(Y);

		const float Fx = X - static_cast<float>(X0);
		const float Fy = Y - static_cast<float>(Y0);

		const float Ux = Fade(Fx);
		const float Uy = Fade(Fy);

		const float N00 = DotGradient2D(Hash2(X0,     Y0,     Seed), Fx,        Fy);
		const float N10 = DotGradient2D(Hash2(X0 + 1, Y0,     Seed), Fx - 1.0f, Fy);
		const float N01 = DotGradient2D(Hash2(X0,     Y0 + 1, Seed), Fx,        Fy - 1.0f);
		const float N11 = DotGradient2D(Hash2(X0 + 1, Y0 + 1, Seed), Fx - 1.0f, Fy - 1.0f);

		const float X0Lerp = Lerp(N00, N10, Ux);
		const float X1Lerp = Lerp(N01, N11, Ux);

		return Lerp(X0Lerp, X1Lerp, Uy) * 0.7071068f * 1.4142136f;
	}

	namespace
	{
		/**
		 * Every octave uses a DIFFERENT derived seed, not the same seed at a
		 * higher frequency.
		 *
		 * With one seed, all octaves share lattice alignment at the origin and
		 * the terrain develops a visible feature there - the classic "something
		 * weird at 0,0" bug.
		 */
		FORCEINLINE uint32 OctaveSeed(uint32 Seed, int32 Octave)
		{
			return HashInt(Seed + static_cast<uint32>(Octave) * 0x9E3779B9u);
		}
	}

	float FBM2D(float X, float Y, uint32 Seed, const FFractalSettings& Settings)
	{
		float Sum = 0.0f;
		float Amplitude = 1.0f;
		float Frequency = Settings.Frequency;
		float TotalAmplitude = 0.0f;

		for (int32 Octave = 0; Octave < Settings.Octaves; ++Octave)
		{
			Sum += Gradient2D(X * Frequency, Y * Frequency, OctaveSeed(Seed, Octave)) * Amplitude;
			TotalAmplitude += Amplitude;

			Amplitude *= Settings.Gain;
			Frequency *= Settings.Lacunarity;
		}

		return (TotalAmplitude > 0.0f) ? Sum / TotalAmplitude : 0.0f;
	}

	float FBM3D(float X, float Y, float Z, uint32 Seed, const FFractalSettings& Settings)
	{
		float Sum = 0.0f;
		float Amplitude = 1.0f;
		float Frequency = Settings.Frequency;
		float TotalAmplitude = 0.0f;

		for (int32 Octave = 0; Octave < Settings.Octaves; ++Octave)
		{
			Sum += Gradient3D(X * Frequency, Y * Frequency, Z * Frequency, OctaveSeed(Seed, Octave)) * Amplitude;
			TotalAmplitude += Amplitude;

			Amplitude *= Settings.Gain;
			Frequency *= Settings.Lacunarity;
		}

		return (TotalAmplitude > 0.0f) ? Sum / TotalAmplitude : 0.0f;
	}

	float Ridged3D(float X, float Y, float Z, uint32 Seed, const FFractalSettings& Settings)
	{
		float Sum = 0.0f;
		float Amplitude = 1.0f;
		float Frequency = Settings.Frequency;
		float TotalAmplitude = 0.0f;

		for (int32 Octave = 0; Octave < Settings.Octaves; ++Octave)
		{
			const float Value = Gradient3D(X * Frequency, Y * Frequency, Z * Frequency, OctaveSeed(Seed, Octave));
			const float Ridge = 1.0f - FMath::Abs(Value);

			// Squaring sharpens the crease. Without it the ridges are rounded
			// and 3D caves come out as wide caverns rather than tunnels.
			Sum += Ridge * Ridge * Amplitude;
			TotalAmplitude += Amplitude;

			Amplitude *= Settings.Gain;
			Frequency *= Settings.Lacunarity;
		}

		return (TotalAmplitude > 0.0f) ? Sum / TotalAmplitude : 0.0f;
	}

	float Ridged2D(float X, float Y, uint32 Seed, const FFractalSettings& Settings)
	{
		float Sum = 0.0f;
		float Amplitude = 1.0f;
		float Frequency = Settings.Frequency;
		float TotalAmplitude = 0.0f;

		for (int32 Octave = 0; Octave < Settings.Octaves; ++Octave)
		{
			const float Value = Gradient2D(X * Frequency, Y * Frequency, OctaveSeed(Seed, Octave));
			const float Ridge = 1.0f - FMath::Abs(Value);

			Sum += Ridge * Ridge * Amplitude;
			TotalAmplitude += Amplitude;

			Amplitude *= Settings.Gain;
			Frequency *= Settings.Lacunarity;
		}

		return (TotalAmplitude > 0.0f) ? Sum / TotalAmplitude : 0.0f;
	}
}

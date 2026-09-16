// Copyright MadFall. All Rights Reserved.

#include "MadSwim.h"

float MadFall::Swim::SubmergedFraction(float FeetZ, float HeadZ, float WaterTopZ)
{
	const float Height = HeadZ - FeetZ;
	if (Height <= KINDA_SMALL_NUMBER)
	{
		return WaterTopZ >= FeetZ ? 1.0f : 0.0f;
	}
	return FMath::Clamp((WaterTopZ - FeetZ) / Height, 0.0f, 1.0f);
}

float MadFall::Swim::Buoyancy(float EyeZ, float WaterTopZ, float DiveInput, const FMadSwimTuning& Tuning)
{
	// The target is the eyes a little above the water line. The further under
	// them the body is, the harder it rises, up to RiseSpeed - a spring rather
	// than a constant push, so a swimmer surfaces quickly from depth and settles
	// at the top instead of leaping out of it.
	const float Target = WaterTopZ + Tuning.FloatEyeOffset;
	const float Rise = FMath::Clamp((Target - EyeZ) * Tuning.BuoyancyStiffness, -Tuning.RiseSpeed, Tuning.RiseSpeed);

	// Swimming down beats the float; swimming up adds to it.
	const float Effort = FMath::Clamp(DiveInput, -1.0f, 1.0f) * Tuning.DiveSpeed;
	return Rise + Effort;
}

FMadSwimState MadFall::Swim::Evaluate(float Fraction, float EyeZ, float WaterTopZ, float DiveInput,
	bool bWasSwimming, bool bSprinting, const FMadSwimTuning& Tuning)
{
	FMadSwimState State;

	// Two thresholds, not one: at a single threshold a survivor standing where
	// the water is exactly that deep flips between walking and swimming every
	// frame, and the camera jitters with it.
	State.bSwimming = bWasSwimming ? Fraction > Tuning.WadeFraction : Fraction >= Tuning.SwimFraction;
	if (!State.bSwimming)
	{
		// Wading is walking, slowed by the water around the legs.
		State.SpeedMultiplier = FMath::Lerp(1.0f, Tuning.SpeedMultiplier, FMath::Clamp(Fraction / Tuning.SwimFraction, 0.0f, 1.0f));
		return State;
	}

	State.bHeadUnder = EyeZ < WaterTopZ;
	State.BuoyancyVelocity = Buoyancy(EyeZ, WaterTopZ, DiveInput, Tuning);
	// A hard stroke is faster than an easy one, and both are slower than walking.
	State.SpeedMultiplier = bSprinting ? Tuning.SprintMultiplier : Tuning.SpeedMultiplier;
	return State;
}

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Swimming: what water does to a body in it.
 *
 * WHY A MODULE OF ITS OWN: water was a block you stood in. It is flagged
 * climbable, so the ladder code caught it and the survivor hung motionless
 * inside a lake - no floating, no slowing, no drowning, and a fall into deep
 * water was as fatal as one onto rock. The rules here are the whole of what
 * water does to movement, and being pure arithmetic they are swept by a test
 * rather than judged from a screenshot of someone bobbing.
 *
 * Everything is in Unreal units (centimetres) and seconds.
 */
struct MADFALLGAMEPLAY_API FMadSwimTuning
{
	/** Submerged at least this much of the body: swimming rather than wading. */
	float SwimFraction = 0.55f;

	/** Back to walking below this, so a survivor in the shallows does not flicker between the two. */
	float WadeFraction = 0.35f;

	/** How fast a body rises when it is deep, and how hard it is pushed. */
	float RiseSpeed = 150.0f;
	float BuoyancyStiffness = 2.6f;

	/** How far the eyes float above the water line when still: a head out of the water. */
	float FloatEyeOffset = 12.0f;

	/** Swimming speed as a fraction of walking, and the sprint's crawl. */
	float SpeedMultiplier = 0.55f;
	float SprintMultiplier = 0.8f;

	/** Water drags: the speed a stroke gets to, and how quickly momentum is lost. */
	float Drag = 2.2f;

	/** How fast a swimmer can drive themselves down against the buoyancy. */
	float DiveSpeed = 220.0f;
};

/** What the water is doing to the survivor this frame. */
struct MADFALLGAMEPLAY_API FMadSwimState
{
	bool bSwimming = false;

	/** Eyes under the water: no air, and the world above is out of reach. */
	bool bHeadUnder = false;

	/** Vertical speed the body settles to, before the swimmer's own effort. */
	float BuoyancyVelocity = 0.0f;

	/** Multiplies the walking speed. */
	float SpeedMultiplier = 1.0f;
};

namespace MadFall::Swim
{
	/**
	 * How much of a capsule from FeetZ to HeadZ is under WaterTopZ, 0 to 1.
	 * A body with no water around it is 0 even if the number says otherwise.
	 */
	MADFALLGAMEPLAY_API float SubmergedFraction(float FeetZ, float HeadZ, float WaterTopZ);

	/**
	 * The vertical speed that floats a body: it rises while its eyes are under
	 * the surface, eases as they approach it and holds them a little above it,
	 * so a swimmer bobs at the top instead of shooting out of the water.
	 * A swimmer driving downward (DiveInput 1) overcomes it.
	 */
	MADFALLGAMEPLAY_API float Buoyancy(float EyeZ, float WaterTopZ, float DiveInput, const FMadSwimTuning& Tuning);

	/**
	 * The state for a body in water, given how submerged it is and whether it
	 * was swimming already (the two thresholds are a hysteresis band).
	 */
	MADFALLGAMEPLAY_API FMadSwimState Evaluate(float Fraction, float EyeZ, float WaterTopZ, float DiveInput,
		bool bWasSwimming, bool bSprinting, const FMadSwimTuning& Tuning);
}

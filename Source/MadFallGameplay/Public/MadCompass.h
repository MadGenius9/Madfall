// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AMadPlayerCharacter;

/** Something the compass points at. */
struct FMadCompassMarker
{
	FString Label;
	FVector Location = FVector::ZeroVector;
	FLinearColor Colour = FLinearColor::White;
};

/**
 * The compass strip at the top of the HUD: headings, and where the bed, the last
 * backpack and nearby POIs are.
 *
 * A strip rather than a map because the world is infinite and unexplored; what
 * a survivor needs is "which way is home" and "what is over that hill", both of
 * which are a bearing and a distance. North is +X, east +Y (Unreal yaw 0 and 90).
 */
namespace MadFall::Compass
{
	/** How far a POI may be to show, voxels. */
	inline constexpr float PoiRangeVoxels = 300.0f;

	/**
	 * Where a bearing lands on the strip: -1 at the left edge, +1 at the right,
	 * unset if it is outside the strip's half-width (HalfSpanDegrees) of the
	 * view yaw. Pure, for tests.
	 */
	MADFALLGAMEPLAY_API TOptional<float> ProjectBearing(float ViewYawDegrees, float BearingDegrees, float HalfSpanDegrees);

	/** Yaw in degrees from one point to another, in the compass's frame (0 north, 90 east). */
	MADFALLGAMEPLAY_API float BearingTo(const FVector& From, const FVector& To);

	/** "N", "NE", ... for a bearing. */
	MADFALLGAMEPLAY_API const TCHAR* GetHeadingName(float BearingDegrees);

	/** The bed, dropped backpacks, and POIs within PoiRangeVoxels. */
	MADFALLGAMEPLAY_API void GatherMarkers(const AMadPlayerCharacter& Player, TArray<FMadCompassMarker>& OutMarkers);
}

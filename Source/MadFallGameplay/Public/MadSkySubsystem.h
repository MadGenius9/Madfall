// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadSkySubsystem.generated.h"

class ADirectionalLight;
class APostProcessVolume;
class ASkyLight;
class AStaticMeshActor;
class UMaterialInstanceDynamic;

/** Where the sun and moon are and how bright, for one time of day. */
struct FMadSkyState
{
	/** Light rotations (the direction the light travels). Pitch below 0 is a light above the horizon. */
	FRotator SunRotation = FRotator::ZeroRotator;
	FRotator MoonRotation = FRotator::ZeroRotator;

	/** Height of the sun above the horizon, -1..1 (sine of its elevation). The moon's is the negative. */
	float SunHeight = 0.0f;

	/** Lux. */
	float SunIntensity = 0.0f;
	float MoonIntensity = 0.0f;
};

namespace MadFall::Sky
{
	/**
	 * Sun and moon for a time of day (hours). The sun rises in the east at 06:00,
	 * peaks 60 degrees up in the south at noon and sets in the west at
	 * 18:00; the moon is opposite it. Never straight overhead: a noon sun at the
	 * zenith left every wall and trunk black. Each fades
	 * out as it reaches the horizon rather than switching off, so dusk is a
	 * transition and a light below the horizon never lights terrain from under it.
	 */
	MADFALLGAMEPLAY_API FMadSkyState Compute(float TimeOfDay, float SunLux, float MoonLux);
}

/**
 * Sun, moon and exposure for game worlds.
 *
 * The test map (and any map without its own) has a movable sun, a real-time
 * sky light, atmosphere, fog and clouds, but no exposure control - the project
 * disables default auto exposure - so the image was a fixed exposure that
 * washed daylight out and could not make night dark. This owns, at runtime:
 *   - the sun's angle and intensity from UMadWorldClockSubsystem;
 *   - a moon, a second dim, cool directional light, so a night is dark but
 *     readable (and a horde can be seen coming);
 *   - an unbound post-process volume with histogram auto exposure clamped to a
 *     range that adapts from day to a darker night and into caves, without
 *     brightening a moonless cave into daylight;
 *   - a night sky: a dome around the camera with M_MadNightSky (deep blue and
 *     stars), faded in as the sun sets, and a cooler, less saturated picture
 *     at night, so night reads as night rather than a dim day under a black
 *     ceiling.
 *
 * A map that ships its own post-process volume or moon is left alone.
 * Tunables: mad.sky.SunLux, MoonLux, ExposureMinEV, ExposureMinEVNight, ExposureMaxEV, ExposureBias.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadSkySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	FString DescribeStatus() const;

	const FMadSkyState& GetState() const { return State; }

private:
	void EnsureActors();
	/** Daylight: 0 at night, 1 with the sun fully up. */
	/** Gloom 0..1 from the weather: a darker exposure bias and a greyer picture under cloud and rain. */
	void ApplyExposure(float Daylight, float Gloom);
	/** Night 0..1; cloud hides the stars. */
	void UpdateNightSky(float Night, float Cloudiness);

	UPROPERTY(Transient)
	TObjectPtr<ADirectionalLight> Sun;

	UPROPERTY(Transient)
	TObjectPtr<ADirectionalLight> Moon;

	UPROPERTY(Transient)
	TObjectPtr<APostProcessVolume> Exposure;

	UPROPERTY(Transient)
	TObjectPtr<ASkyLight> SkyLight;

	UPROPERTY(Transient)
	TObjectPtr<AStaticMeshActor> NightSky;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> NightSkyMaterial;

	float AppliedNightSky = -1.0f;

	bool bSearched = false;
	bool bOwnsExposure = false;
	uint32 AppliedExposureHash = 0;
	FMadSkyState State;
};

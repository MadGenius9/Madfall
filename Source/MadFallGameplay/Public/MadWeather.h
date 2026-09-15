// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadWeather.generated.h"

class AExponentialHeightFog;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;

UENUM()
enum class EMadWeather : uint8
{
	Clear,
	Cloudy,
	Rain,
	Storm,
	/** Rain or a storm below freezing. */
	Snow
};

/** The weather at one moment, blended across fronts. */
struct FMadWeatherState
{
	/** The front in charge (the nearer one, at a boundary). */
	EMadWeather Kind = EMadWeather::Clear;

	/** 0 clear sky to 1 overcast. */
	float Cloudiness = 0.0f;

	/** 0 dry to 1 downpour. Snow when cold. */
	float Precipitation = 0.0f;

	/** 0 still to 1 gale. */
	float Wind = 0.0f;

	bool bSnow = false;

	/** Degrees added to the air temperature. */
	float TemperatureOffset = 0.0f;
};

/**
 * What the weather has done to surfaces: how wet they are and how much snow
 * lies. Lags the weather - ground soaks in a couple of minutes of rain and
 * takes ten to dry, snow builds over a few minutes and melts slower - so a
 * shower that has passed still leaves the world wet.
 */
struct FMadSurfaceWeather
{
	float Wetness = 0.0f;
	float SnowCover = 0.0f;
};

namespace MadFall::Weather
{
	/**
	 * Advances surface weather by DeltaSeconds of real time. Pure, so tested:
	 * rain wets (heavier rain faster), snow builds instead of wetting, and both
	 * decay when the sky is dry - snow melting into wetness.
	 */
	MADFALLGAMEPLAY_API FMadSurfaceWeather StepSurface(const FMadSurfaceWeather& In, const FMadWeatherState& Weather, float DeltaSeconds);

	/**
	 * A lightning bolt as line segments from Top down to Ground: a jagged main
	 * channel that wanders sideways a little at each step, and a couple of
	 * shorter forks off it. Deterministic for a seed.
	 */
	MADFALLGAMEPLAY_API void MakeBolt(int32 Seed, const FVector& Top, const FVector& Ground, TArray<TPair<FVector, FVector>>& OutSegments);

	/** Seconds thunder takes to arrive from a strike this far away, in centimetres (sound at 343 m/s). */
	inline float ThunderDelay(float DistanceCm) { return FMath::Max(0.0f, DistanceCm) / 34300.0f; }

	/** A front lasts this many in-game hours; neighbours blend over the last BlendHours of each. */
	inline constexpr double FrontHours = 6.0;
	inline constexpr double BlendHours = 1.0;

	/** Which weather a front brings: a pure function of the world seed and the front's index. */
	MADFALLGAMEPLAY_API EMadWeather RollFront(uint64 Seed, int64 FrontIndex);

	/**
	 * The weather at a point in world time. Deterministic, so nothing about it
	 * needs saving: a loaded world has the weather it would have had. Below
	 * FreezingCelsius precipitation is snow.
	 */
	MADFALLGAMEPLAY_API FMadWeatherState Evaluate(uint64 Seed, double TotalHours, float AirCelsius);

	/** The steady state a kind of weather settles at. */
	MADFALLGAMEPLAY_API FMadWeatherState Settle(EMadWeather Kind);

	MADFALLGAMEPLAY_API const TCHAR* GetName(EMadWeather Kind);
	MADFALLGAMEPLAY_API bool ParseName(const FString& Name, EMadWeather& OutKind);

	inline constexpr float FreezingCelsius = 1.0f;
}

/**
 * Weather: clouds, rain, storms and snow, moving through in six-hour fronts.
 *
 * What it does to the world, from one blended state:
 *   light     the sun dims under cloud (UMadSkySubsystem reads GetSunScale)
 *   fog       the map's height fog thickens with cloud and rain
 *   cold      rain takes a few degrees off the air, a storm more
 *             (UMadSurvivalComponent adds GetTemperatureOffset)
 *   sound     a rain or wind loop at the weather's strength; thunder in storms,
 *             with a flash of sky light
 *   rain/snow streaks around the camera, hidden under a roof
 *
 * Precipitation is an instanced field of streaks fixed in a box around the
 * camera, scrolled down as one component and wrapped - one transform update
 * a frame whatever the density - rather than hundreds of moving particles.
 * No particle assets exist, and this costs nothing measurable.
 *
 *   `mad.weather.status`
 *   `mad.weather.set <clear|cloudy|rain|storm|snow|auto>`
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadWeatherSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	const FMadWeatherState& GetState() const { return State; }

	/** Multiplier on sunlight: 1 under a clear sky. */
	float GetSunScale() const { return 1.0f - 0.65f * State.Cloudiness; }

	float GetTemperatureOffset() const { return State.TemperatureOffset; }

	/** Multiplier on sky light: duller under cloud, a flash of lightning in a storm. */
	float GetSkyLightScale() const { return 1.0f - 0.35f * State.Cloudiness + FlashTimer * 25.0f; }

	const FMadSurfaceWeather& GetSurface() const { return Surface; }

	/** Sets surface wetness and snow directly (console, probes); weather carries on from there. */
	void SetSurface(const FMadSurfaceWeather& InSurface) { Surface = InSurface; }

	/** Forces a kind of weather until cleared (unset = follow the fronts). */
	void SetOverride(TOptional<EMadWeather> Kind);

	/** True while a roof or overhang is above the camera: no rain falls in view. */
	bool IsCameraSheltered() const { return bSheltered; }

	FString DescribeStatus() const;

	/** A lightning strike now, wherever the storm would put one (console, probes). */
	void StrikeNow() { FlashTimer = 0.18f; ++ThunderCount; Strike(LastCamera); }

	/** Lifetime count, for CI. */
	int32 GetThunderCount() const { return ThunderCount; }

private:
	void EnsurePrecipitation();
	void UpdatePrecipitation(float DeltaTime, const FVector& Camera);
	void UpdateFog();
	void UpdateSound(float DeltaTime);
	/** Draws a bolt at a random bearing in the distance and schedules its thunder. */
	void Strike(const FVector& Camera);
	void UpdateBolt(float DeltaTime);
	void UpdateShelter(const FVector& Camera);
	/** Steps surface weather and writes MPC_MadWeather for the world's materials. */
	void UpdateSurfaces(float DeltaTime);

	FMadWeatherState State;
	FMadSurfaceWeather Surface;
	bool bSurfaceSeeded = false;
	FMadSurfaceWeather AppliedSurface = { -1.0f, -1.0f };
	float AppliedWind = -1.0f;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialParameterCollection> WeatherCollection;

	TOptional<EMadWeather> Override;
	/** Blends an override in and out rather than snapping. */
	float OverrideBlend = 0.0f;
	FMadWeatherState Previous;

	UPROPERTY(Transient)
	TObjectPtr<AActor> PrecipitationActor;

	/** Three layers of streaks, switched on as the rain grows heavier. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Layers;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> StreakMaterial;

	TWeakObjectPtr<AExponentialHeightFog> Fog;
	float BaseFogDensity = -1.0f;

	float ScrollOffset = 0.0f;
	bool bSheltered = false;
	float ShelterTimer = 0.0f;
	float ThunderTimer = 8.0f;
	float FlashTimer = 0.0f;
	/** Thunder on its way from the last strike: seconds until it is heard. */
	float PendingThunder = -1.0f;
	float BoltTimer = 0.0f;
	FVector LastCamera = FVector::ZeroVector;

	UPROPERTY(Transient)
	TObjectPtr<AActor> BoltActor;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Bolt;

	/** Loaded with the world, not at the first strike: a synchronous load mid-storm measured an 8 ms frame. */
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMesh> BoltMesh;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> BoltMaterial;
	int32 ThunderCount = 0;
	bool bShowingSnow = false;
};

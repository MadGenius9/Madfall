// Copyright MadFall. All Rights Reserved.

#include "MadSkySubsystem.h"

#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/CollisionProfile.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MadWeather.h"
#include "Components/SkyLightComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadWorldClockSubsystem.h"

namespace
{
	TAutoConsoleVariable<float> CVarSunLux(
		TEXT("mad.sky.SunLux"), 10.0f,
		TEXT("Sun intensity at noon, lux. Relative to MoonLux and the exposure range, not physical daylight."));

	TAutoConsoleVariable<float> CVarMoonLux(
		TEXT("mad.sky.MoonLux"), 0.35f,
		TEXT("Moon intensity when high, lux."));

	TAutoConsoleVariable<float> CVarExposureMinEV(
		TEXT("mad.sky.ExposureMinEV"), 1.0f,
		TEXT("By day, the darkest scene auto exposure will brighten for (EV100). Lower lifts shade and caves; measured -1 washed a forest out, 2 left its shade black."));

	TAutoConsoleVariable<float> CVarExposureMinEVNight(
		TEXT("mad.sky.ExposureMinEVNight"), -2.0f,
		TEXT("The same floor at night, blended by sun height. Low enough that moonlight reads; 2 made midnight pitch black."));

	TAutoConsoleVariable<float> CVarExposureMaxEV(
		TEXT("mad.sky.ExposureMaxEV"), 6.0f,
		TEXT("Brightest scene auto exposure will darken for (EV100)."));

	TAutoConsoleVariable<float> CVarSkyLightIntensity(
		TEXT("mad.sky.SkyLightIntensity"), 3.0f,
		TEXT("Intensity of the map's sky light: the fill that lights faces the sun does not reach. Measured in a forest at noon: 1.5 left trunks in shade near black (luma 14 of 255), 6 washed the sunlit ground out; 3 keeps both. Night and interiors are set by auto exposure and barely change."));

	TAutoConsoleVariable<float> CVarExposureBiasNight(
		TEXT("mad.sky.ExposureBiasNight"), -1.0f,
		TEXT("Exposure compensation in stops added at full night, blended by daylight. Negative is darker."));

	TAutoConsoleVariable<bool> CVarNightSkyDome(
		TEXT("mad.sky.NightSkyDome"), true,
		TEXT("Spawn the night sky dome when the world starts."));

	TAutoConsoleVariable<float> CVarNightSkyBrightness(
		TEXT("mad.sky.NightSkyBrightness"), 1.0f,
		TEXT("Scale on the night sky dome's glow and stars. 0 hides the dome."));

	TAutoConsoleVariable<float> CVarExposureBias(
		TEXT("mad.sky.ExposureBias"), 0.0f,
		TEXT("Exposure compensation in stops, applied after auto exposure."));

	/** 0 at and below Low, 1 at and above High, smooth between. */
	float SmoothRamp(float Value, float Low, float High)
	{
		const float T = FMath::Clamp((Value - Low) / (High - Low), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}
}

FMadSkyState MadFall::Sky::Compute(float TimeOfDay, float SunLux, float MoonLux)
{
	FMadSkyState Out;
	const float Hours = FMath::Fmod(FMath::Max(TimeOfDay, 0.0f), 24.0f);

	// The sun on a circle tilted 30 degrees south of the zenith: angle 0 at
	// sunrise (east), half pi at noon, pi at sunset (west). World +Y is east,
	// +X north, +Z up.
	constexpr float Tilt = UE_PI / 6.0f;
	const float Angle = (Hours - 6.0f) / 12.0f * UE_PI;
	const FVector TowardSun(-FMath::Sin(Angle) * FMath::Sin(Tilt), FMath::Cos(Angle), FMath::Sin(Angle) * FMath::Cos(Tilt));
	const float Elevation = static_cast<float>(TowardSun.Z);
	Out.SunHeight = Elevation;
	Out.SunRotation = (-TowardSun).Rotation();
	Out.MoonRotation = TowardSun.Rotation();

	// Fade across a band just around the horizon: a light a few degrees below
	// it would otherwise light the underside of every overhang.
	Out.SunIntensity = SunLux * SmoothRamp(Elevation, -0.02f, 0.12f);
	Out.MoonIntensity = MoonLux * SmoothRamp(-Elevation, -0.02f, 0.12f);
	return Out;
}

bool UMadSkySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadSkySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadSkySubsystem, STATGROUP_Tickables);
}

void UMadSkySubsystem::EnsureActors()
{
	if (bSearched)
	{
		return;
	}
	bSearched = true;

	UWorld* World = GetWorld();
	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		const UDirectionalLightComponent* Component = Cast<UDirectionalLightComponent>(It->GetLightComponent());
		if (Component == nullptr)
		{
			continue;
		}
		// The map's second atmosphere light is its moon; the first is the sun.
		if (Component->GetAtmosphereSunLightIndex() == 1)
		{
			Moon = *It;
		}
		else if (Sun == nullptr)
		{
			Sun = *It;
		}
	}

	// The day cycle needs a movable sun. A stationary one (the default for a
	// placed DirectionalLight, and what the test map had) silently never moved:
	// shadows and daylight stayed at noon through the night. Nothing here uses
	// baked lighting (r.AllowStaticLighting=False), so promoting it costs nothing.
	for (ADirectionalLight* Light : { Sun.Get(), Moon.Get() })
	{
		if (Light != nullptr && Light->GetLightComponent() != nullptr && Light->GetLightComponent()->Mobility != EComponentMobility::Movable)
		{
			UE_LOG(LogMadFallGameplay, Log, TEXT("Sky: %s was not movable; making it movable for the day cycle."), *Light->GetName());
			Light->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		}
	}
	// The sun scatters in the volumetric fog (shafts through leaves) and blooms
	// a little where it is in view past an occluder.
	if (UDirectionalLightComponent* SunComponent = Sun ? Cast<UDirectionalLightComponent>(Sun->GetLightComponent()) : nullptr)
	{
		SunComponent->SetVolumetricScatteringIntensity(1.6f);
		SunComponent->bEnableLightShaftBloom = true;
		SunComponent->BloomScale = 0.12f;
		SunComponent->BloomThreshold = 4.0f;
		SunComponent->MarkRenderStateDirty();
	}

	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		SkyLight = *It;
		break;
	}

	bool bMapHasExposure = false;
	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		bMapHasExposure |= It->bUnbound && It->Settings.bOverride_AutoExposureMethod;
	}

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;

	if (Moon == nullptr)
	{
		Params.Name = MakeUniqueObjectName(World->PersistentLevel, ADirectionalLight::StaticClass(), TEXT("MadFallMoon"));
		Moon = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform::Identity, Params);
		if (UDirectionalLightComponent* Component = Moon ? Cast<UDirectionalLightComponent>(Moon->GetLightComponent()) : nullptr)
		{
			Component->SetMobility(EComponentMobility::Movable);
			// Not an atmosphere light: lit by a moon the sky scattered into a
			// daytime blue at midnight. The terrain is moonlit; the sky stays dark.
			Component->SetAtmosphereSunLight(false);
			// Moonlight is cool and weak; shadows kept so a lit horde casts them.
			Component->SetLightColor(FLinearColor(0.55f, 0.65f, 1.0f));
			Component->SetIntensity(0.0f);
		}
	}

	// The night sky dome. Transient and unlit; skipped without the asset or a renderer.
	UMaterialInterface* SkyMaterial = FApp::CanEverRender() && CVarNightSkyDome.GetValueOnGameThread() ? LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_MadNightSky.M_MadNightSky")) : nullptr;
	UStaticMesh* Sphere = SkyMaterial ? LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")) : nullptr;
	if (SkyMaterial != nullptr && Sphere != nullptr)
	{
		Params.Name = MakeUniqueObjectName(World->PersistentLevel, AStaticMeshActor::StaticClass(), TEXT("MadFallNightSky"));
		NightSky = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform::Identity, Params);
		if (NightSky != nullptr)
		{
			// No collision at all, on the actor and the component, before the mesh is
			// set. A 10 km sphere that blocks is a cage around the world: the first
			// version only set the component's collision after spawning, a survivor
			// teleported or restored inside it was pushed out to its top (Z 10 km),
			// froze, and saved there. Headless runs have no dome, so only rendered
			// probes saw it.
			NightSky->SetActorEnableCollision(false);
			UStaticMeshComponent* Mesh = NightSky->GetStaticMeshComponent();
			Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			Mesh->SetGenerateOverlapEvents(false);
			Mesh->SetCanEverAffectNavigation(false);
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetStaticMesh(Sphere);
			NightSkyMaterial = UMaterialInstanceDynamic::Create(SkyMaterial, this);
			Mesh->SetMaterial(0, NightSkyMaterial);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetCastShadow(false);
			Mesh->bAffectDistanceFieldLighting = false;
			// 10 km radius (the engine sphere is 50 uu): past the far terrain.
			Mesh->SetWorldScale3D(FVector(20000.0));
			NightSky->SetActorHiddenInGame(true);
		}
	}

	if (!bMapHasExposure)
	{
		Params.Name = MakeUniqueObjectName(World->PersistentLevel, APostProcessVolume::StaticClass(), TEXT("MadFallExposure"));
		Exposure = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, Params);
		if (Exposure != nullptr)
		{
			Exposure->bUnbound = true;
			Exposure->Priority = -1.0f;   // any authored volume the player walks into wins
			bOwnsExposure = true;
		}
	}

	UE_LOG(LogMadFallGameplay, Log, TEXT("Sky: sun %s, moon %s, exposure %s."),
		Sun ? *Sun->GetName() : TEXT("none"), Moon ? *Moon->GetName() : TEXT("none"),
		bOwnsExposure ? TEXT("runtime volume") : TEXT("the map's"));
}

void UMadSkySubsystem::ApplyExposure(float Daylight, float Gloom)
{
	if (!bOwnsExposure || Exposure == nullptr)
	{
		return;
	}

	// Quantised so the volume is only touched when the floor moves visibly, not
	// every frame of a sunset.
	const float MinEV = FMath::RoundToFloat(FMath::Lerp(CVarExposureMinEVNight.GetValueOnGameThread(), CVarExposureMinEV.GetValueOnGameThread(),
		FMath::Clamp(Daylight, 0.0f, 1.0f)) * 20.0f) / 20.0f;
	const float MaxEV = FMath::Max(MinEV, CVarExposureMaxEV.GetValueOnGameThread());
	// Quantised like the floor, so a slowly gathering storm touches the volume rarely.
	const float QuantisedGloom = FMath::RoundToFloat(FMath::Clamp(Gloom, 0.0f, 1.0f) * 20.0f) / 20.0f;
	const float QuantisedNight = FMath::RoundToFloat((1.0f - FMath::Clamp(Daylight, 0.0f, 1.0f)) * 20.0f) / 20.0f;
	// Auto exposure would lift an overcast scene straight back to noon: pull the
	// bias down with the gloom so a storm reads as a storm.
	// The same at night: auto exposure settles above the night floor on a moonlit
	// field and the starry dome, so the floor alone (measured: -2, -1 and 0 gave the
	// same picture) cannot make night darker. The bias can.
	const float Bias = CVarExposureBias.GetValueOnGameThread() - 0.8f * QuantisedGloom + CVarExposureBiasNight.GetValueOnGameThread() * QuantisedNight;
	const uint32 Hash = HashCombine(HashCombine(HashCombine(HashCombine(GetTypeHash(MinEV), GetTypeHash(MaxEV)), GetTypeHash(Bias)), GetTypeHash(QuantisedGloom)), GetTypeHash(QuantisedNight));
	if (Hash == AppliedExposureHash)
	{
		return;
	}
	AppliedExposureHash = Hash;

	FPostProcessSettings& S = Exposure->Settings;
	S.bOverride_AutoExposureMethod = true;
	S.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
	S.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	S.AutoExposureApplyPhysicalCameraExposure = false;
	S.bOverride_AutoExposureMinBrightness = true;
	S.AutoExposureMinBrightness = MinEV;
	S.bOverride_AutoExposureMaxBrightness = true;
	S.AutoExposureMaxBrightness = MaxEV;
	S.bOverride_AutoExposureBias = true;
	S.AutoExposureBias = Bias;
	// Eyes adjust quickly to light and slowly to dark: stepping out of a cave
	// is a moment's glare, walking into one takes a few seconds.
	S.bOverride_AutoExposureSpeedUp = true;
	S.AutoExposureSpeedUp = 3.0f;
	S.bOverride_AutoExposureSpeedDown = true;
	S.AutoExposureSpeedDown = 1.0f;
	// Colour drains out under heavy cloud; a clear day keeps its saturation.
	// Night is a little cooler and greyer too: a moonlit field at full saturation
	// read as a dim afternoon. Only a little: grading the whole picture also drains
	// torchlight, and at 0.55 saturation with a strong blue gain a torch-lit wall
	// went pink. The moon's own blue does the rest.
	S.bOverride_ColorSaturation = true;
	S.ColorSaturation = FVector4(1.0f, 1.0f, 1.0f, (1.0f - 0.55f * QuantisedGloom) * (1.0f - 0.25f * QuantisedNight));
	S.bOverride_ColorGain = true;
	S.ColorGain = FVector4(FMath::Lerp(1.0f, 0.93f, QuantisedNight), FMath::Lerp(1.0f, 0.97f, QuantisedNight), FMath::Lerp(1.0f, 1.05f, QuantisedNight), 1.0f);
	S.bOverride_ColorContrast = true;
	S.ColorContrast = FVector4(1.0f, 1.0f, 1.0f, 1.0f - 0.2f * QuantisedGloom);
	// A camera, not a debug view: a soft vignette, gentle bloom on bright sky and
	// fire, and a slight filmic lift in contrast. Tuned by eye against the forest,
	// plains and a torch-lit interior; strong enough to notice missing, not to see.
	S.bOverride_VignetteIntensity = true;
	S.VignetteIntensity = 0.32f;
	S.bOverride_BloomIntensity = true;
	S.BloomIntensity = 0.45f;
	S.bOverride_BloomThreshold = true;
	S.BloomThreshold = 1.2f;
	S.bOverride_ColorContrast = true;
	S.ColorContrast = FVector4(1.0f, 1.0f, 1.0f, (1.0f - 0.2f * QuantisedGloom) * 1.06f);
	S.bOverride_SceneFringeIntensity = true;
	S.SceneFringeIntensity = 0.0f;
}

void UMadSkySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	MAD_FRAME_SCOPE(Other);

	EnsureActors();

	const UMadWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<UMadWorldClockSubsystem>();
	if (Clock == nullptr)
	{
		return;
	}

	State = MadFall::Sky::Compute(Clock->GetTimeOfDay(), CVarSunLux.GetValueOnGameThread(), CVarMoonLux.GetValueOnGameThread());
	const float SunLux = CVarSunLux.GetValueOnGameThread();
	// The exposure floor follows the time of day, before cloud dims the sun.
	const float Daylight = SunLux > 0.0f ? State.SunIntensity / SunLux : 0.0f;
	float SkyScale = 1.0f;
	float Gloom = 0.0f;
	float Cloudiness = 0.0f;
	if (const UMadWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UMadWeatherSubsystem>())
	{
		Gloom = 0.6f * Weather->GetState().Cloudiness + 0.4f * Weather->GetState().Precipitation;
		Cloudiness = Weather->GetState().Cloudiness;
		// Cloud dims the sun and the sky, not the exposure floor: an overcast
		// noon should look duller, not be auto-exposed back to bright.
		State.SunIntensity *= Weather->GetSunScale();
		State.MoonIntensity *= Weather->GetSunScale();
		SkyScale = Weather->GetSkyLightScale();
	}
	ApplyExposure(Daylight, Gloom);
	// The sky darkens a little after the sun is down, not the moment it touches the horizon.
	UpdateNightSky(1.0f - SmoothRamp(State.SunHeight, -0.18f, 0.02f), Cloudiness);

	auto Drive = [](ADirectionalLight* Light, const FRotator& Rotation, float Intensity)
	{
		if (Light == nullptr || Light->GetRootComponent() == nullptr || Light->GetRootComponent()->Mobility != EComponentMobility::Movable)
		{
			// A static or stationary light cannot move at runtime; a hand-built
			// level that has one keeps its fixed sun.
			return;
		}
		Light->SetActorRotation(Rotation);
		ULightComponent* Component = Light->GetLightComponent();
		const bool bOn = Intensity > KINDA_SMALL_NUMBER;
		if (!FMath::IsNearlyEqual(Component->Intensity, Intensity, 0.001f))
		{
			Component->SetIntensity(Intensity);
		}
		// Off rather than zero: a zero-intensity directional light still renders
		// its virtual shadow maps.
		if (Component->IsVisible() != bOn)
		{
			Component->SetVisibility(bOn);
		}
	};
	if (SkyLight != nullptr && SkyLight->GetLightComponent() != nullptr)
	{
		const float SkyIntensity = CVarSkyLightIntensity.GetValueOnGameThread() * SkyScale;
		if (!FMath::IsNearlyEqual(SkyLight->GetLightComponent()->Intensity, SkyIntensity, 0.01f))
		{
			SkyLight->GetLightComponent()->SetIntensity(SkyIntensity);
		}
	}
	Drive(Sun, State.SunRotation, State.SunIntensity);
	Drive(Moon, State.MoonRotation, State.MoonIntensity);
}

void UMadSkySubsystem::UpdateNightSky(float Night, float Cloudiness)
{
	if (NightSky == nullptr || NightSkyMaterial == nullptr)
	{
		return;
	}
	const float Glow = Night * FMath::Max(0.0f, CVarNightSkyBrightness.GetValueOnGameThread());
	const bool bShow = Glow > 0.001f;
	if (NightSky->IsHidden() == bShow)
	{
		// Hidden by day: nothing to draw, and nothing between the atmosphere and the camera.
		NightSky->SetActorHiddenInGame(!bShow);
	}
	if (!bShow)
	{
		return;
	}
	// Follows the camera, so the dome is always around it and the stars never parallax.
	if (const APlayerController* Controller = GetWorld()->GetFirstPlayerController(); Controller && Controller->PlayerCameraManager)
	{
		NightSky->SetActorLocation(Controller->PlayerCameraManager->GetCameraLocation());
	}
	// Cloud hides stars and glow alike; quantised so the parameter changes rarely.
	const float Wanted = FMath::RoundToFloat(Glow * (1.0f - 0.85f * FMath::Clamp(Cloudiness, 0.0f, 1.0f)) * 50.0f) / 50.0f;
	if (!FMath::IsNearlyEqual(Wanted, AppliedNightSky))
	{
		AppliedNightSky = Wanted;
		NightSkyMaterial->SetScalarParameterValue(TEXT("Night"), Wanted);
	}
}

FString UMadSkySubsystem::DescribeStatus() const
{
	return FString::Printf(TEXT("Sky: sun %s pitch %.1f at %.2f lux; moon %s pitch %.1f at %.3f lux; exposure %s EV100 [%.1f, %.1f] bias %.1f"),
		Sun ? *Sun->GetName() : TEXT("none"), State.SunRotation.Pitch, State.SunIntensity,
		Moon ? *Moon->GetName() : TEXT("none"), State.MoonRotation.Pitch, State.MoonIntensity,
		bOwnsExposure ? TEXT("runtime") : TEXT("map"),
		CVarExposureMinEV.GetValueOnGameThread(), CVarExposureMaxEV.GetValueOnGameThread(), CVarExposureBias.GetValueOnGameThread());
}

static FAutoConsoleCommandWithWorld GMadSkyStatusCommand(
	TEXT("mad.sky.status"),
	TEXT("Sun, moon and exposure."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadSkySubsystem* Sky = World ? World->GetSubsystem<UMadSkySubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Sky->DescribeStatus());
		}
	}));

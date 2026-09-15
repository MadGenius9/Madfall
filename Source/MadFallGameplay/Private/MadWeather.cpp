// Copyright MadFall. All Rights Reserved.

#include "MadWeather.h"

#include "Materials/MaterialParameterCollection.h"
#include "Misc/App.h"
#include "Materials/MaterialParameterCollectionInstance.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadAudioSubsystem.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadVoxelRaycast.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldClockSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	TAutoConsoleVariable<bool> CVarVolumetricFog(
		TEXT("mad.weather.VolumetricFog"), true,
		TEXT("Turn the map's height fog volumetric when the world starts: light shafts and lit haze."));

	/** Streak box around the camera, cm. */
	constexpr float BoxHalfWidth = 1400.0f;
	constexpr float BoxHeight = 2400.0f;
	constexpr int32 StreaksPerLayer = 220;
	constexpr int32 NumLayers = 3;

	constexpr float RainSpeed = 1100.0f;
	constexpr float SnowSpeed = 140.0f;

	/** Seconds an override takes to blend in or out. */
	constexpr float OverrideSeconds = 6.0f;

	uint64 Mix(uint64 X)
	{
		X ^= X >> 33;
		X *= 0xff51afd7ed558ccdULL;
		X ^= X >> 33;
		X *= 0xc4ceb9fe1a85ec53ULL;
		X ^= X >> 33;
		return X;
	}

	FMadWeatherState Lerp(const FMadWeatherState& A, const FMadWeatherState& B, float T)
	{
		FMadWeatherState Out;
		Out.Kind = T < 0.5f ? A.Kind : B.Kind;
		Out.Cloudiness = FMath::Lerp(A.Cloudiness, B.Cloudiness, T);
		Out.Precipitation = FMath::Lerp(A.Precipitation, B.Precipitation, T);
		Out.Wind = FMath::Lerp(A.Wind, B.Wind, T);
		Out.TemperatureOffset = FMath::Lerp(A.TemperatureOffset, B.TemperatureOffset, T);
		return Out;
	}

	/** Precipitation turns to snow below freezing; the kind follows. */
	void ApplyCold(FMadWeatherState& State, float AirCelsius)
	{
		State.bSnow = State.Precipitation > 0.0f && AirCelsius + State.TemperatureOffset < MadFall::Weather::FreezingCelsius;
		if (State.bSnow && (State.Kind == EMadWeather::Rain || State.Kind == EMadWeather::Storm))
		{
			State.Kind = EMadWeather::Snow;
		}
	}
}

// --- model -----------------------------------------------------------------------

EMadWeather MadFall::Weather::RollFront(uint64 Seed, int64 FrontIndex)
{
	// The first front of a world is always clear: a new survivor's first
	// morning is for finding their feet, not for a thunderstorm.
	if (FrontIndex <= 1)
	{
		return EMadWeather::Clear;
	}
	const double Roll = static_cast<double>(Mix(Seed * 0x9E3779B97F4A7C15ULL + static_cast<uint64>(FrontIndex) + 0x5EA7) >> 11) / static_cast<double>(1ULL << 53);
	if (Roll < 0.42) { return EMadWeather::Clear; }
	if (Roll < 0.67) { return EMadWeather::Cloudy; }
	if (Roll < 0.88) { return EMadWeather::Rain; }
	return EMadWeather::Storm;
}

FMadWeatherState MadFall::Weather::Settle(EMadWeather Kind)
{
	FMadWeatherState State;
	State.Kind = Kind;
	switch (Kind)
	{
	case EMadWeather::Cloudy:
		State.Cloudiness = 0.6f;
		State.Wind = 0.3f;
		State.TemperatureOffset = -1.0f;
		break;
	case EMadWeather::Rain:
		State.Cloudiness = 0.85f;
		State.Precipitation = 0.55f;
		State.Wind = 0.4f;
		State.TemperatureOffset = -3.0f;
		break;
	case EMadWeather::Storm:
		State.Cloudiness = 1.0f;
		State.Precipitation = 1.0f;
		State.Wind = 0.9f;
		State.TemperatureOffset = -6.0f;
		break;
	case EMadWeather::Snow:
		State.Cloudiness = 0.9f;
		State.Precipitation = 0.7f;
		State.Wind = 0.5f;
		State.TemperatureOffset = -4.0f;
		State.bSnow = true;
		break;
	case EMadWeather::Clear:
	default:
		State.Wind = 0.1f;
		break;
	}
	return State;
}

FMadWeatherState MadFall::Weather::Evaluate(uint64 Seed, double TotalHours, float AirCelsius)
{
	const double Fronts = FMath::Max(0.0, TotalHours) / FrontHours;
	const int64 Index = FMath::FloorToInt64(Fronts);
	const double HoursInto = (Fronts - static_cast<double>(Index)) * FrontHours;

	const FMadWeatherState Current = Settle(RollFront(Seed, Index));
	const FMadWeatherState Next = Settle(RollFront(Seed, Index + 1));
	const double BlendStart = FrontHours - BlendHours;
	const float T = HoursInto > BlendStart ? FMath::SmoothStep(0.0f, 1.0f, static_cast<float>((HoursInto - BlendStart) / BlendHours)) : 0.0f;

	FMadWeatherState State = Lerp(Current, Next, T);
	ApplyCold(State, AirCelsius);
	return State;
}

const TCHAR* MadFall::Weather::GetName(EMadWeather Kind)
{
	switch (Kind)
	{
	case EMadWeather::Cloudy: return TEXT("cloudy");
	case EMadWeather::Rain:   return TEXT("rain");
	case EMadWeather::Storm:  return TEXT("storm");
	case EMadWeather::Snow:   return TEXT("snow");
	default:                  return TEXT("clear");
	}
}

bool MadFall::Weather::ParseName(const FString& Name, EMadWeather& OutKind)
{
	for (EMadWeather Kind : { EMadWeather::Clear, EMadWeather::Cloudy, EMadWeather::Rain, EMadWeather::Storm, EMadWeather::Snow })
	{
		if (Name.Equals(GetName(Kind), ESearchCase::IgnoreCase))
		{
			OutKind = Kind;
			return true;
		}
	}
	return false;
}

// --- subsystem ----------------------------------------------------------------------

bool UMadWeatherSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UMadWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Everything weather draws or writes is loaded while the world loads. The
	// frame budget gate forces a storm, and its first strike loading the bolt's
	// mesh and material in the middle of play was an 8 ms frame.
	WeatherCollection = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Materials/MPC_MadWeather.MPC_MadWeather"));
	if (FApp::CanEverRender())
	{
		BoltMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		BoltMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_MadLightning.M_MadLightning"));
	}
}

void UMadWeatherSubsystem::Deinitialize()
{
	if (UMadAudioSubsystem* Audio = GetWorld() ? GetWorld()->GetSubsystem<UMadAudioSubsystem>() : nullptr)
	{
		Audio->SetLoop(0, EMadSound::RainLoop, 0.0f);
		Audio->SetLoop(1, EMadSound::WindLoop, 0.0f);
	}
	Super::Deinitialize();
}

TStatId UMadWeatherSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadWeatherSubsystem, STATGROUP_Tickables);
}

void UMadWeatherSubsystem::SetOverride(TOptional<EMadWeather> Kind)
{
	Previous = State;
	Override = Kind;
	OverrideBlend = 0.0f;
	if (Kind.IsSet() && Kind.GetValue() == EMadWeather::Storm)
	{
		// A forced storm announces itself promptly.
		ThunderTimer = FMath::Min(ThunderTimer, 3.0f);
	}
}

void UMadWeatherSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	MAD_FRAME_SCOPE(Other);

	UWorld* World = GetWorld();
	const UMadWorldClockSubsystem* Clock = World->GetSubsystem<UMadWorldClockSubsystem>();
	const UMadVoxelWorldSubsystem* VoxelWorld = World->GetSubsystem<UMadVoxelWorldSubsystem>();
	if (Clock == nullptr || VoxelWorld == nullptr)
	{
		return;
	}

	const APlayerController* Controller = World->GetFirstPlayerController();
	const FVector Camera = Controller && Controller->PlayerCameraManager ? Controller->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;

	// Air temperature without the weather's own offset, for the snow line.
	float Air = 10.0f;
	if (const FMadWorldGenerator* Generator = VoxelWorld->GetWorldGenerator())
	{
		const FVector At = Camera / MadFall::VoxelSizeUU;
		Air = FMath::Lerp(-20.0f, 42.0f, Generator->GetTemperature(At.X, At.Y, At.Z)) + Clock->GetTemperatureOffset();
	}

	const FMadWeatherState Natural = MadFall::Weather::Evaluate(static_cast<uint64>(VoxelWorld->GetSeed()), Clock->GetTotalHours(), Air);
	OverrideBlend = FMath::Min(1.0f, OverrideBlend + DeltaTime / OverrideSeconds);
	if (Override.IsSet())
	{
		FMadWeatherState Forced = MadFall::Weather::Settle(Override.GetValue());
		if (Override.GetValue() == EMadWeather::Snow)
		{
			Forced.bSnow = true;
		}
		State = Lerp(Previous, Forced, OverrideBlend);
		State.Kind = Override.GetValue();
		State.bSnow = Forced.bSnow || (State.Precipitation > 0.0f && Air + State.TemperatureOffset < MadFall::Weather::FreezingCelsius);
	}
	else if (OverrideBlend < 1.0f)
	{
		State = Lerp(Previous, Natural, OverrideBlend);
		State.bSnow = Natural.bSnow;
	}
	else
	{
		State = Natural;
	}

	LastCamera = Camera;
	UpdateShelter(Camera);
	UpdateSurfaces(DeltaTime);
	UpdateBolt(DeltaTime);
	UpdatePrecipitation(DeltaTime, Camera);
	UpdateFog();
	UpdateSound(DeltaTime);
}

FMadSurfaceWeather MadFall::Weather::StepSurface(const FMadSurfaceWeather& In, const FMadWeatherState& Weather, float DeltaSeconds)
{
	FMadSurfaceWeather Out = In;
	const float Dt = FMath::Max(0.0f, DeltaSeconds);
	const float Falling = FMath::Clamp(Weather.Precipitation, 0.0f, 1.0f);
	if (Falling > 0.02f && Weather.bSnow)
	{
		// About three minutes of heavy snow to cover everything; it does not wet.
		Out.SnowCover = FMath::Min(1.0f, Out.SnowCover + Dt * Falling / 180.0f);
		Out.Wetness = FMath::Max(0.0f, Out.Wetness - Dt / 900.0f);
	}
	else if (Falling > 0.02f)
	{
		// Two minutes of downpour to soak; a drizzle only ever dampens.
		const float Target = FMath::Min(1.0f, 0.3f + Falling);
		if (Out.Wetness < Target)
		{
			Out.Wetness = FMath::Min(Target, Out.Wetness + Dt * Falling / 120.0f);
		}
		// Rain washes snow away, and the melt is wet.
		const float Melt = FMath::Min(Out.SnowCover, Dt / 120.0f);
		Out.SnowCover -= Melt;
		Out.Wetness = FMath::Min(1.0f, Out.Wetness + Melt * 0.5f);
	}
	else
	{
		// Ten minutes to dry from soaked; snow lingers twice that, and while it
		// melts the ground under it stays damp.
		const float Melt = FMath::Min(Out.SnowCover, Dt / 1200.0f);
		Out.SnowCover -= Melt;
		const float MeltDamp = Out.SnowCover > 0.02f ? 0.4f : 0.0f;
		Out.Wetness = FMath::Clamp(FMath::Max(Out.Wetness - Dt / 600.0f, MeltDamp), 0.0f, 1.0f);
	}
	return Out;
}

void MadFall::Weather::MakeBolt(int32 Seed, const FVector& Top, const FVector& Ground, TArray<TPair<FVector, FVector>>& OutSegments)
{
	OutSegments.Reset();
	FRandomStream Random(Seed);
	const FVector Down = Ground - Top;
	const float Length = Down.Size();
	if (Length < 1.0f)
	{
		return;
	}
	const int32 Steps = FMath::Clamp(FMath::RoundToInt32(Length / 3500.0f), 8, 40);
	const float Jag = Length / Steps * 0.45f;

	TArray<FVector> Channel;
	Channel.Add(Top);
	for (int32 Step = 1; Step < Steps; ++Step)
	{
		const FVector Along = Top + Down * (static_cast<float>(Step) / Steps);
		Channel.Add(Along + FVector(Random.FRandRange(-Jag, Jag), Random.FRandRange(-Jag, Jag), Random.FRandRange(-Jag, Jag) * 0.3f));
	}
	Channel.Add(Ground);
	for (int32 Index = 0; Index + 1 < Channel.Num(); ++Index)
	{
		OutSegments.Emplace(Channel[Index], Channel[Index + 1]);
	}

	// Two forks, from the upper half, a third of the channel long, spreading outward.
	for (int32 Fork = 0; Fork < 2; ++Fork)
	{
		const int32 From = Random.RandRange(1, FMath::Max(1, Steps / 2));
		FVector At = Channel[From];
		const FVector Spread = FVector(Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f), 0.0f).GetSafeNormal() * Jag * 1.5f;
		const int32 ForkSteps = FMath::Max(2, Steps / 3);
		for (int32 Step = 0; Step < ForkSteps; ++Step)
		{
			const FVector Next = At + Down / Steps + Spread + FVector(Random.FRandRange(-Jag, Jag), Random.FRandRange(-Jag, Jag), 0.0f) * 0.5f;
			OutSegments.Emplace(At, Next);
			At = Next;
		}
	}
}

void UMadWeatherSubsystem::Strike(const FVector& Camera)
{
	// The flash and the sky light happen at once; the sound follows at the speed of sound.
	const float Bearing = FMath::FRandRange(0.0f, UE_TWO_PI);
	const float Distance = FMath::FRandRange(25000.0f, 90000.0f);
	const FVector Ground = Camera + FVector(FMath::Cos(Bearing), FMath::Sin(Bearing), 0.0f) * Distance - FVector(0.0, 0.0, 1500.0);
	const FVector Top = Ground + FVector(FMath::FRandRange(-8000.0f, 8000.0f), FMath::FRandRange(-8000.0f, 8000.0f), 60000.0f);
	PendingThunder = MadFall::Weather::ThunderDelay(Distance);

	if (!FApp::CanEverRender())
	{
		return;
	}
	if (Bolt == nullptr)
	{
		UStaticMesh* Cube = BoltMesh;
		UMaterialInterface* Material = BoltMaterial;
		if (Cube == nullptr || Material == nullptr)
		{
			return;
		}
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		BoltActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		if (BoltActor == nullptr)
		{
			return;
		}
		BoltActor->SetActorEnableCollision(false);
		USceneComponent* Root = NewObject<USceneComponent>(BoltActor, TEXT("BoltRoot"));
		Root->SetMobility(EComponentMobility::Movable);
		Root->RegisterComponent();
		BoltActor->SetRootComponent(Root);
		Bolt = NewObject<UInstancedStaticMeshComponent>(BoltActor, TEXT("Bolt"));
		Bolt->SetStaticMesh(Cube);
		Bolt->SetMaterial(0, Material);
		Bolt->SetMobility(EComponentMobility::Movable);
		Bolt->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Bolt->SetCastShadow(false);
		Bolt->bAffectDistanceFieldLighting = false;
		Bolt->SetupAttachment(Root);
		Bolt->RegisterComponent();
	}

	TArray<TPair<FVector, FVector>> Segments;
	MadFall::Weather::MakeBolt(FMath::Rand(), Top, Ground, Segments);
	TArray<FTransform> Transforms;
	Transforms.Reserve(Segments.Num());
	for (const TPair<FVector, FVector>& Segment : Segments)
	{
		const FVector Along = Segment.Value - Segment.Key;
		const float Length = Along.Size();
		// A cube stretched along the segment: 1.2 m thick, so it reads at a kilometre.
		Transforms.Emplace(FRotationMatrix::MakeFromX(Along).ToQuat(), (Segment.Key + Segment.Value) * 0.5, FVector(Length / 100.0, 1.2, 1.2));
	}
	Bolt->ClearInstances();
	Bolt->AddInstances(Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
	Bolt->SetVisibility(true);
	BoltTimer = 0.32f;
}

void UMadWeatherSubsystem::UpdateBolt(float DeltaTime)
{
	if (PendingThunder >= 0.0f)
	{
		PendingThunder -= DeltaTime;
		if (PendingThunder < 0.0f)
		{
			if (UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>())
			{
				Audio->Play2D(EMadSound::Thunder, 0.9f);
			}
		}
	}
	if (Bolt == nullptr || BoltTimer <= 0.0f)
	{
		return;
	}
	BoltTimer -= DeltaTime;
	// A strike flickers: on, off, on again, then gone.
	const bool bShow = BoltTimer > 0.0f && (BoltTimer > 0.2f || (BoltTimer > 0.06f && BoltTimer < 0.14f));
	if (Bolt->IsVisible() != bShow)
	{
		Bolt->SetVisibility(bShow);
	}
}

void UMadWeatherSubsystem::UpdateSurfaces(float DeltaTime)
{
	if (!bSurfaceSeeded)
	{
		// A world loaded mid-downpour is already wet, not drying out from a dry start.
		bSurfaceSeeded = true;
		Surface.Wetness = State.bSnow ? 0.0f : FMath::Clamp(State.Precipitation * 1.3f, 0.0f, 1.0f);
		Surface.SnowCover = State.bSnow ? FMath::Clamp(State.Precipitation * 1.3f, 0.0f, 1.0f) : 0.0f;
	}
	Surface = MadFall::Weather::StepSurface(Surface, State, DeltaTime);

	if (WeatherCollection == nullptr)
	{
		WeatherCollection = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Materials/MPC_MadWeather.MPC_MadWeather"));
		if (WeatherCollection == nullptr)
		{
			return;
		}
	}
	UMaterialParameterCollectionInstance* Instance = GetWorld()->GetParameterCollectionInstance(WeatherCollection);
	if (Instance == nullptr)
	{
		return;
	}
	// Written only when a value moves visibly: each write re-uploads the collection.
	if (FMath::Abs(Surface.Wetness - AppliedSurface.Wetness) > 0.01f || FMath::Abs(Surface.SnowCover - AppliedSurface.SnowCover) > 0.01f)
	{
		AppliedSurface = Surface;
		Instance->SetScalarParameterValue(TEXT("Wetness"), Surface.Wetness);
		Instance->SetScalarParameterValue(TEXT("SnowCover"), Surface.SnowCover);
	}
	if (FMath::Abs(State.Wind - AppliedWind) > 0.02f)
	{
		AppliedWind = State.Wind;
		Instance->SetScalarParameterValue(TEXT("Wind"), State.Wind);
	}
}

void UMadWeatherSubsystem::UpdateShelter(const FVector& Camera)
{
	ShelterTimer -= GetWorld()->GetDeltaSeconds();
	if (ShelterTimer > 0.0f)
	{
		return;
	}
	ShelterTimer = 0.25f;
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	FMadVoxelHit Hit;
	bSheltered = VoxelWorld != nullptr && MadFall::VoxelRaycast(Camera / MadFall::VoxelSizeUU, FVector::UpVector, 24.0f,
		[VoxelWorld](const FIntVector& V) { return VoxelWorld->GetVoxel(V.X, V.Y, V.Z).IsSolid(); }, Hit);
}

void UMadWeatherSubsystem::EnsurePrecipitation()
{
	if (PrecipitationActor != nullptr)
	{
		return;
	}
	UWorld* World = GetWorld();
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	PrecipitationActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (PrecipitationActor == nullptr)
	{
		return;
	}
	USceneComponent* Root = NewObject<USceneComponent>(PrecipitationActor, TEXT("PrecipitationRoot"));
	PrecipitationActor->SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);
	Root->RegisterComponent();

	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		StreakMaterial = UMaterialInstanceDynamic::Create(Base, this);
	}

	for (int32 Layer = 0; Layer < NumLayers; ++Layer)
	{
		UInstancedStaticMeshComponent* Streaks = NewObject<UInstancedStaticMeshComponent>(PrecipitationActor);
		Streaks->SetMobility(EComponentMobility::Movable);
		Streaks->SetupAttachment(Root);
		Streaks->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Streaks->SetCastShadow(false);
		Streaks->SetCanEverAffectNavigation(false);
		if (StreakMaterial != nullptr)
		{
			Streaks->SetMaterial(0, StreakMaterial);
		}
		Streaks->RegisterComponent();
		Streaks->SetVisibility(false);
		Layers.Add(Streaks);
	}
	bShowingSnow = !State.bSnow;   // forces the first fill below
}

void UMadWeatherSubsystem::UpdatePrecipitation(float DeltaTime, const FVector& Camera)
{
	const bool bWant = State.Precipitation > 0.02f;
	if (!bWant && PrecipitationActor == nullptr)
	{
		return;
	}
	EnsurePrecipitation();
	if (PrecipitationActor == nullptr)
	{
		return;
	}

	// Refill the streaks when rain turns to snow or back: a different shape and speed.
	if (bShowingSnow != State.bSnow)
	{
		bShowingSnow = State.bSnow;
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, bShowingSnow ? TEXT("/Engine/BasicShapes/Cube.Cube") : TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		const FVector Scale = bShowingSnow ? FVector(0.03) : FVector(0.008, 0.008, 0.45);
		if (StreakMaterial != nullptr)
		{
			StreakMaterial->SetVectorParameterValue(TEXT("Color"), bShowingSnow ? FLinearColor(0.9f, 0.92f, 0.95f) : FLinearColor(0.72f, 0.76f, 0.82f));
		}
		FRandomStream Random(0x5A1DAu);
		for (UInstancedStaticMeshComponent* Streaks : Layers)
		{
			Streaks->ClearInstances();
			Streaks->SetStaticMesh(Mesh);
			TArray<FTransform> Instances;
			Instances.Reserve(StreaksPerLayer * 2);
			for (int32 Index = 0; Index < StreaksPerLayer; ++Index)
			{
				const FVector At(Random.FRandRange(-BoxHalfWidth, BoxHalfWidth), Random.FRandRange(-BoxHalfWidth, BoxHalfWidth), Random.FRandRange(0.0f, BoxHeight));
				// Each streak twice, one box height apart, so the field tiles vertically and scrolls seamlessly.
				Instances.Add(FTransform(FRotator::ZeroRotator, At, Scale));
				Instances.Add(FTransform(FRotator::ZeroRotator, At + FVector(0.0, 0.0, BoxHeight), Scale));
			}
			Streaks->AddInstances(Instances, false);
		}
	}

	const float Speed = bShowingSnow ? SnowSpeed : RainSpeed;
	ScrollOffset = FMath::Fmod(ScrollOffset + Speed * DeltaTime, BoxHeight);
	// Snow drifts sideways with the wind; rain leans into it.
	const float Lean = State.Wind * (bShowingSnow ? 25.0f : 12.0f);
	PrecipitationActor->SetActorLocationAndRotation(Camera - FVector(0.0, 0.0, BoxHeight * 0.5f + ScrollOffset), FRotator(0.0, 0.0, Lean));

	for (int32 Layer = 0; Layer < Layers.Num(); ++Layer)
	{
		const bool bVisible = bWant && !bSheltered && State.Precipitation > static_cast<float>(Layer) / NumLayers * 0.9f;
		if (Layers[Layer]->IsVisible() != bVisible)
		{
			Layers[Layer]->SetVisibility(bVisible);
		}
	}
}

void UMadWeatherSubsystem::UpdateFog()
{
	if (!Fog.IsValid())
	{
		for (TActorIterator<AExponentialHeightFog> It(GetWorld()); It; ++It)
		{
			Fog = *It;
			BaseFogDensity = It->GetComponent()->FogDensity;
			// Volumetric fog: light scatters in the air, so the sun comes through
			// the canopy in shafts, torches glow in a haze at night, and a storm's
			// thickening fog is lit rather than a grey wash. Forward-scattering (a
			// glow toward the sun), and kept thin: the height fog already does the
			// distance. Cost: a froxel volume pass, High quality and up only
			// (scalability turns it off below).
			UExponentialHeightFogComponent* Component = It->GetComponent();
			if (CVarVolumetricFog.GetValueOnGameThread())
			{
				Component->SetVolumetricFog(true);
				Component->SetVolumetricFogScatteringDistribution(0.7f);
				Component->SetVolumetricFogExtinctionScale(0.6f);
				Component->SetVolumetricFogDistance(12000.0f);
			}
			break;
		}
	}
	if (!Fog.IsValid() || BaseFogDensity < 0.0f)
	{
		return;
	}
	// Rain closes the distance in: a downpour cuts visibility to a few hundred metres.
	const float Wanted = BaseFogDensity * (1.0f + 3.0f * State.Cloudiness + 14.0f * State.Precipitation);
	UExponentialHeightFogComponent* Component = Fog->GetComponent();
	if (!FMath::IsNearlyEqual(Component->FogDensity, Wanted, BaseFogDensity * 0.02f + KINDA_SMALL_NUMBER))
	{
		Component->SetFogDensity(Wanted);
	}
}

void UMadWeatherSubsystem::UpdateSound(float DeltaTime)
{
	UMadAudioSubsystem* Audio = GetWorld()->GetSubsystem<UMadAudioSubsystem>();
	if (Audio == nullptr)
	{
		return;
	}
	// Muffled indoors, not silent: rain on the roof.
	const float Shelter = bSheltered ? 0.45f : 1.0f;
	Audio->SetLoop(0, EMadSound::RainLoop, State.bSnow ? 0.0f : State.Precipitation * 0.6f * Shelter);
	Audio->SetLoop(1, EMadSound::WindLoop, FMath::Max(0.0f, State.Wind - 0.25f) * 0.7f * Shelter);

	FlashTimer = FMath::Max(0.0f, FlashTimer - DeltaTime);
	if (State.Kind == EMadWeather::Storm && State.Precipitation > 0.7f && !State.bSnow)
	{
		ThunderTimer -= DeltaTime;
		if (ThunderTimer <= 0.0f)
		{
			ThunderTimer = FMath::FRandRange(12.0f, 40.0f);
			FlashTimer = 0.18f;
			++ThunderCount;
			Strike(LastCamera);
		}
	}
}

FString UMadWeatherSubsystem::DescribeStatus() const
{
	return FString::Printf(TEXT("Weather: %s%s, cloud %.2f, precipitation %.2f%s, wind %.2f, %+.1f C, sun x%.2f%s, thunder %d; ground wet %.2f, snow %.2f"),
		MadFall::Weather::GetName(State.Kind), Override.IsSet() ? TEXT(" (forced)") : TEXT(""), State.Cloudiness, State.Precipitation,
		State.bSnow ? TEXT(" (snow)") : TEXT(""), State.Wind, State.TemperatureOffset, GetSunScale(),
		bSheltered ? TEXT(", sheltered") : TEXT(""), ThunderCount, Surface.Wetness, Surface.SnowCover);
}

static FAutoConsoleCommandWithWorld GMadWeatherStatusCommand(
	TEXT("mad.weather.status"),
	TEXT("The current weather and its effects."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadWeatherSubsystem* Weather = World ? World->GetSubsystem<UMadWeatherSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Weather->DescribeStatus());
		}
	}));

static FAutoConsoleCommandWithWorld GMadWeatherStrikeCommand(
	TEXT("mad.weather.strike"),
	TEXT("A lightning strike in the distance now, thunder following."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadWeatherSubsystem* Weather = World ? World->GetSubsystem<UMadWeatherSubsystem>() : nullptr)
		{
			Weather->StrikeNow();
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadWeatherSurfaceCommand(
	TEXT("mad.weather.surface"),
	TEXT("mad.weather.surface <wetness 0-1> <snow 0-1>: sets how wet the ground is and how much snow lies; the weather carries on from there."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadWeatherSubsystem* Weather = World ? World->GetSubsystem<UMadWeatherSubsystem>() : nullptr;
		if (Weather == nullptr || Args.Num() < 2)
		{
			return;
		}
		FMadSurfaceWeather Surface;
		Surface.Wetness = FMath::Clamp(FCString::Atof(*Args[0]), 0.0f, 1.0f);
		Surface.SnowCover = FMath::Clamp(FCString::Atof(*Args[1]), 0.0f, 1.0f);
		Weather->SetSurface(Surface);
		UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Weather->DescribeStatus());
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadWeatherSetCommand(
	TEXT("mad.weather.set"),
	TEXT("mad.weather.set <clear|cloudy|rain|storm|snow|auto>: forces the weather, or returns it to the fronts."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadWeatherSubsystem* Weather = World ? World->GetSubsystem<UMadWeatherSubsystem>() : nullptr;
		if (Weather == nullptr || Args.Num() == 0)
		{
			UE_LOG(LogMadFallGameplay, Warning, TEXT("usage: mad.weather.set <clear|cloudy|rain|storm|snow|auto>"));
			return;
		}
		EMadWeather Kind;
		if (Args[0].Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			Weather->SetOverride(TOptional<EMadWeather>());
		}
		else if (MadFall::Weather::ParseName(Args[0], Kind))
		{
			Weather->SetOverride(Kind);
		}
		else
		{
			UE_LOG(LogMadFallGameplay, Warning, TEXT("Unknown weather '%s'."), *Args[0]);
			return;
		}
		UE_LOG(LogMadFallGameplay, Display, TEXT("Weather set to %s."), *Args[0]);
	}));

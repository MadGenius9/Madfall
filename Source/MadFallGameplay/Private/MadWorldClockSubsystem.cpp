// Copyright MadFall. All Rights Reserved.

#include "MadWorldClockSubsystem.h"

#include "MadFrameBudget.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "MadFallGameplay.h"
#include "Misc/DefaultValueHelper.h"

namespace
{
	/** 7 Days to Die's default is an hour; 40 minutes keeps a horde week to under five hours of play. */
	TAutoConsoleVariable<float> CVarDayMinutes(
		TEXT("mad.clock.DayMinutes"),
		40.0f,
		TEXT("Real-time minutes in one 24-hour game day."),
		ECVF_Default);
}

bool UMadWorldClockSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMadWorldClockSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadWorldClockSubsystem, STATGROUP_Tickables);
}

void UMadWorldClockSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MAD_FRAME_SCOPE(Other);

	if (!bPaused)
	{
		const float HoursPerSecond = 24.0f / (FMath::Max(0.1f, CVarDayMinutes.GetValueOnGameThread()) * 60.0f);
		const bool bWasNight = IsNight();

		TimeOfDay += DeltaTime * HoursPerSecond;
		if (TimeOfDay >= 24.0f)
		{
			TimeOfDay -= 24.0f;
			++Day;
		}

		const bool bNowNight = IsNight();
		if (!bWasNight && bNowNight)
		{
			UE_LOG(LogMadFallGameplay, Log, TEXT("Dusk on day %d%s."), Day, IsHordeNight() ? TEXT(" - HORDE NIGHT") : TEXT(""));
			DuskDelegate.Broadcast(Day);
		}
		else if (bWasNight && !bNowNight)
		{
			UE_LOG(LogMadFallGameplay, Log, TEXT("Dawn on day %d."), Day);
			DawnDelegate.Broadcast(Day);
		}
	}

}

bool UMadWorldClockSubsystem::IsHordeNight() const
{
	return IsNight() && GetNightDay() > 0 && (GetNightDay() % HordeEveryDays) == 0;
}

int32 UMadWorldClockSubsystem::DaysUntilHorde() const
{
	if (IsHordeNight())
	{
		return 0;
	}
	const int32 Remainder = Day % HordeEveryDays;
	return Remainder == 0 ? 0 : HordeEveryDays - Remainder;
}

float UMadWorldClockSubsystem::GetTemperatureOffset() const
{
	// A cosine with its minimum at 05:00 and maximum at 17:00, +-6 degrees.
	const float Phase = (TimeOfDay - 17.0f) / 24.0f * UE_TWO_PI;
	return 6.0f * FMath::Cos(Phase);
}

void UMadWorldClockSubsystem::SetTime(float Hours, int32 InDay)
{
	TimeOfDay = FMath::Fmod(FMath::Max(0.0f, Hours), 24.0f);
	Day = FMath::Max(1, InDay);
}

FString UMadWorldClockSubsystem::DescribeStatus() const
{
	const int32 Hours = FMath::FloorToInt32(TimeOfDay);
	const int32 Minutes = FMath::FloorToInt32((TimeOfDay - Hours) * 60.0f);
	return FString::Printf(TEXT("Day %d, %02d:%02d%s%s; next horde in %d day(s); %.1f min days"),
		Day, Hours, Minutes, IsNight() ? TEXT(" (night)") : TEXT(""), IsHordeNight() ? TEXT(" HORDE NIGHT") : TEXT(""),
		DaysUntilHorde(), CVarDayMinutes.GetValueOnGameThread());
}

static FAutoConsoleCommandWithWorldAndArgs GMadClockSetCommand(
	TEXT("mad.clock.set"),
	TEXT("mad.clock.set <hours> [day] - sets the time of day (and optionally the day number)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		UMadWorldClockSubsystem* Clock = World ? World->GetSubsystem<UMadWorldClockSubsystem>() : nullptr;
		float Hours = 0.0f;
		if (Clock == nullptr || Args.Num() < 1 || !FDefaultValueHelper::ParseFloat(Args[0], Hours))
		{
			UE_LOG(LogMadFallGameplay, Error, TEXT("Usage: mad.clock.set <hours> [day] (game worlds only)"));
			return;
		}
		int32 NewDay = Clock->GetDay();
		if (Args.Num() > 1)
		{
			FDefaultValueHelper::ParseInt(Args[1], NewDay);
		}
		Clock->SetTime(Hours, NewDay);
		UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Clock->DescribeStatus());
	}));

static FAutoConsoleCommandWithWorld GMadClockStatusCommand(
	TEXT("mad.clock.status"),
	TEXT("Time of day, day count and horde schedule."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadWorldClockSubsystem* Clock = World ? World->GetSubsystem<UMadWorldClockSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Clock->DescribeStatus());
		}
	}));

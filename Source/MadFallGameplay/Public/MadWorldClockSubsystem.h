// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MadWorldClockSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FMadOnDayPhase, int32 /*Day*/);

/**
 * Time of day, day count, and which nights are horde nights.
 *
 * Game worlds only. Everything time-dependent asks this one clock - survival
 * ambient temperature, the horde director, game stage for loot - so a console
 * `mad.clock.set 21.9` puts the entire game one tick before dusk.
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadWorldClockSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Hours, 0-24. */
	float GetTimeOfDay() const { return TimeOfDay; }

	/** 1 on the first day. */
	int32 GetDay() const { return Day; }

	/** Hours since midnight before day 1: one continuous timeline for things that take days, like crops. */
	double GetTotalHours() const { return static_cast<double>(Day - 1) * 24.0 + TimeOfDay; }

	/** Night is 22:00 to 06:00. */
	bool IsNight() const { return TimeOfDay >= DuskHour || TimeOfDay < DawnHour; }

	/**
	 * Every HordeEveryDays-th day's night. A horde night belongs to the day it
	 * started on, so the hours after midnight on day 8 are still day 7's horde.
	 */
	bool IsHordeNight() const;

	/** The day number a horde night counts as (see IsHordeNight). */
	int32 GetNightDay() const { return TimeOfDay < DawnHour ? Day - 1 : Day; }

	/** Days until the next horde night starts (0 = tonight). */
	int32 DaysUntilHorde() const;

	/** Degrees added to ambient temperature by time of day: coldest just before dawn, warmest mid-afternoon. */
	float GetTemperatureOffset() const;

	void SetTime(float Hours, int32 InDay);

	/** Paused clocks keep time of day fixed. For screenshots and tests. */
	void SetPaused(bool bInPaused) { bPaused = bInPaused; }

	FMadOnDayPhase& OnDusk() { return DuskDelegate; }
	FMadOnDayPhase& OnDawn() { return DawnDelegate; }

	FString DescribeStatus() const;

	static constexpr float DuskHour = 22.0f;
	static constexpr float DawnHour = 6.0f;
	static constexpr int32 HordeEveryDays = 7;

private:
	float TimeOfDay = 8.0f;
	int32 Day = 1;
	bool bPaused = false;

	FMadOnDayPhase DuskDelegate;
	FMadOnDayPhase DawnDelegate;
};

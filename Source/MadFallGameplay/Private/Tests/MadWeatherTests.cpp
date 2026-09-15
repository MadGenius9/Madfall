// Copyright MadFall. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "MadWeather.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadSurfaceWeatherTest,
	"MadFall.World.SurfaceWeather",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadSurfaceWeatherTest::RunTest(const FString& Parameters)
{
	using MadFall::Weather::StepSurface;
	auto Run = [](FMadSurfaceWeather Surface, const FMadWeatherState& Weather, float Seconds)
	{
		for (float T = 0.0f; T < Seconds; T += 0.5f)
		{
			Surface = StepSurface(Surface, Weather, 0.5f);
		}
		return Surface;
	};
	FMadWeatherState Clear;
	FMadWeatherState Downpour = MadFall::Weather::Settle(EMadWeather::Storm);
	Downpour.bSnow = false;
	FMadWeatherState Drizzle = Downpour;
	Drizzle.Precipitation = 0.15f;
	FMadWeatherState Blizzard = Downpour;
	Blizzard.bSnow = true;

	FMadSurfaceWeather Soaked = Run(FMadSurfaceWeather(), Downpour, 180.0f);
	TestTrue(FString::Printf(TEXT("three minutes of storm soaks the ground (%.2f)"), Soaked.Wetness), Soaked.Wetness > 0.9f);
	TestTrue(TEXT("a storm does not lay snow"), Soaked.SnowCover == 0.0f);
	TestTrue(TEXT("a drizzle only dampens"), Run(FMadSurfaceWeather(), Drizzle, 900.0f).Wetness < 0.5f);

	const FMadSurfaceWeather Drying = Run(Soaked, Clear, 300.0f);
	TestTrue(FString::Printf(TEXT("five dry minutes leave it damp, not dry (%.2f)"), Drying.Wetness), Drying.Wetness > 0.3f && Drying.Wetness < Soaked.Wetness);
	TestTrue(TEXT("fifteen dry minutes dry it out"), Run(Soaked, Clear, 900.0f).Wetness == 0.0f);

	const FMadSurfaceWeather Snowed = Run(FMadSurfaceWeather(), Blizzard, 240.0f);
	TestTrue(FString::Printf(TEXT("a blizzard covers the ground (%.2f)"), Snowed.SnowCover), Snowed.SnowCover > 0.9f);
	TestTrue(TEXT("snow does not wet"), Snowed.Wetness == 0.0f);
	const FMadSurfaceWeather Melting = Run(Snowed, Clear, 300.0f);
	TestTrue(TEXT("snow lingers and melts into wet ground"), Melting.SnowCover > 0.5f && Melting.SnowCover < Snowed.SnowCover && Melting.Wetness > 0.0f);
	TestTrue(TEXT("rain washes snow away"), Run(Snowed, Downpour, 150.0f).SnowCover == 0.0f);
	TestTrue(TEXT("a negative step changes nothing"), StepSurface(Soaked, Clear, -5.0f).Wetness == Soaked.Wetness);

	// Lightning: a connected jagged channel from the cloud to the ground, plus forks.
	TArray<TPair<FVector, FVector>> Bolt;
	const FVector Top(0.0, 0.0, 60000.0);
	const FVector Ground(0.0, 0.0, 0.0);
	MadFall::Weather::MakeBolt(7, Top, Ground, Bolt);
	TestTrue(FString::Printf(TEXT("a bolt has a channel and forks (%d segments)"), Bolt.Num()), Bolt.Num() > 10);
	if (Bolt.Num() > 0)
	{
		TestTrue(TEXT("it starts at the cloud"), Bolt[0].Key.Equals(Top));
		bool bConnected = true;
		int32 ChannelEnd = 0;
		for (int32 Index = 0; Index + 1 < Bolt.Num(); ++Index)
		{
			if (!Bolt[Index].Value.Equals(Bolt[Index + 1].Key))
			{
				ChannelEnd = Index;
				break;
			}
		}
		for (int32 Index = 0; Index < ChannelEnd; ++Index)
		{
			bConnected &= Bolt[Index].Value.Equals(Bolt[Index + 1].Key);
		}
		TestTrue(TEXT("the channel is connected"), bConnected);
		TestTrue(TEXT("and reaches the ground"), ChannelEnd > 0 && Bolt[ChannelEnd].Value.Equals(Ground));
		bool bJagged = false;
		for (const TPair<FVector, FVector>& Segment : Bolt)
		{
			bJagged |= FMath::Abs(Segment.Key.X) > 100.0;
		}
		TestTrue(TEXT("it is jagged, not a straight line"), bJagged);
	}
	TArray<TPair<FVector, FVector>> Again;
	MadFall::Weather::MakeBolt(7, Top, Ground, Again);
	TestTrue(TEXT("the same seed makes the same bolt"), Again.Num() == Bolt.Num() && (Bolt.Num() == 0 || Again.Last().Value.Equals(Bolt.Last().Value)));
	TestTrue(TEXT("thunder from a kilometre arrives about three seconds later"), FMath::IsNearlyEqual(MadFall::Weather::ThunderDelay(100000.0f), 2.915f, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMadWeatherTest,
	"MadFall.World.Weather",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMadWeatherTest::RunTest(const FString& Parameters)
{
	using namespace MadFall::Weather;
	const uint64 Seed = 20260913u;

	// Deterministic: the same seed and hour give the same sky, so weather needs no save.
	const FMadWeatherState A = Evaluate(Seed, 123.4, 15.0f);
	const FMadWeatherState B = Evaluate(Seed, 123.4, 15.0f);
	TestTrue(TEXT("same seed and hour, same weather"), A.Kind == B.Kind && A.Cloudiness == B.Cloudiness && A.Precipitation == B.Precipitation);

	// A new world's first hours are clear.
	for (double Hours = 0.0; Hours < 11.0; Hours += 0.5)
	{
		if (Evaluate(Seed, Hours, 15.0f).Precipitation > 0.0f)
		{
			AddError(FString::Printf(TEXT("it rains at hour %.1f of a new world"), Hours));
			break;
		}
	}

	// Over many fronts every kind turns up, roughly in its share.
	int32 Counts[5] = {};
	for (int64 Front = 2; Front < 2002; ++Front)
	{
		++Counts[static_cast<int32>(RollFront(Seed, Front))];
	}
	TestTrue(*FString::Printf(TEXT("clear is the most common (%d)"), Counts[0]), Counts[0] > 700 && Counts[0] < 980);
	TestTrue(*FString::Printf(TEXT("storms are rare but happen (%d)"), Counts[3]), Counts[3] > 150 && Counts[3] < 330);
	TestEqual(TEXT("fronts never roll snow directly"), Counts[4], 0);
	TestNotEqual(TEXT("a different seed, a different sequence"), RollFront(Seed, 50) == RollFront(Seed + 1, 50) && RollFront(Seed, 51) == RollFront(Seed + 1, 51)
		&& RollFront(Seed, 52) == RollFront(Seed + 1, 52) && RollFront(Seed, 53) == RollFront(Seed + 1, 53), true);

	// Fronts blend: no jump larger than a sliver between samples three minutes apart.
	float Worst = 0.0f;
	FMadWeatherState Last = Evaluate(Seed, 0.0, 15.0f);
	for (double Hours = 0.05; Hours < 24.0 * 14.0; Hours += 0.05)
	{
		const FMadWeatherState Now = Evaluate(Seed, Hours, 15.0f);
		Worst = FMath::Max(Worst, FMath::Abs(Now.Cloudiness - Last.Cloudiness));
		Worst = FMath::Max(Worst, FMath::Abs(Now.Precipitation - Last.Precipitation));
		Last = Now;
	}
	TestTrue(*FString::Printf(TEXT("weather changes gradually (largest step %.3f)"), Worst), Worst < 0.1f);

	// Cold turns rain to snow; warmth does not.
	const FMadWeatherState Storm = Settle(EMadWeather::Storm);
	TestTrue(TEXT("a storm is wet, windy and cold"), Storm.Precipitation > 0.9f && Storm.Wind > 0.5f && Storm.TemperatureOffset < -4.0f);
	TestEqual(TEXT("clear skies change nothing"), Settle(EMadWeather::Clear).TemperatureOffset, 0.0f);
	double RainyHour = -1.0;
	for (double Hours = 12.0; Hours < 24.0 * 60.0 && RainyHour < 0.0; Hours += 1.0)
	{
		if (Evaluate(Seed, Hours, 15.0f).Precipitation > 0.3f)
		{
			RainyHour = Hours;
		}
	}
	if (TestTrue(TEXT("found a rainy hour"), RainyHour >= 0.0))
	{
		TestFalse(TEXT("warm rain is rain"), Evaluate(Seed, RainyHour, 15.0f).bSnow);
		const FMadWeatherState Cold = Evaluate(Seed, RainyHour, -8.0f);
		TestTrue(TEXT("cold rain is snow"), Cold.bSnow && Cold.Kind == EMadWeather::Snow);
	}

	EMadWeather Parsed = EMadWeather::Clear;
	TestTrue(TEXT("names parse"), ParseName(TEXT("Storm"), Parsed) && Parsed == EMadWeather::Storm);
	TestFalse(TEXT("nonsense does not"), ParseName(TEXT("hail"), Parsed));
	return true;
}

#endif

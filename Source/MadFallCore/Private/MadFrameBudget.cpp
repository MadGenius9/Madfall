// Copyright MadFall. All Rights Reserved.

#include "MadFrameBudget.h"

#include "HAL/IConsoleManager.h"
#include "MadFallCore.h"
#include "Misc/CoreDelegates.h"
#include "Misc/StringBuilder.h"

namespace
{
	constexpr int32 NumSystems = static_cast<int32>(EMadFrameSystem::Num);

	/** This frame's seconds per system. Game thread only. */
	double GCurrent[NumSystems] = {};
	bool GCurrentHasWork = false;

	MadFall::FrameBudget::FReport GReport;
	FDelegateHandle GEndFrameHandle;

	FAutoConsoleCommand CmdPerf(
		TEXT("mad.perf"),
		TEXT("MadFall game-thread time per frame by system, against the 2 ms budget."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			TArray<FString> Lines;
			MadFall::FrameBudget::Describe().ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				UE_LOG(LogMadFallVoxel, Display, TEXT("%s"), *Line);
			}
		}));

	FAutoConsoleCommand CmdPerfReset(
		TEXT("mad.perf.reset"),
		TEXT("Starts a new MadFall frame-budget measurement."),
		FConsoleCommandDelegate::CreateStatic([]() { MadFall::FrameBudget::Reset(); }));
}

namespace MadFall::FrameBudget
{
	const TCHAR* GetSystemName(EMadFrameSystem System)
	{
		switch (System)
		{
		case EMadFrameSystem::WorldLoads: return TEXT("world loads");
		case EMadFrameSystem::Streaming:  return TEXT("streaming");
		case EMadFrameSystem::Meshing:    return TEXT("meshing");
		case EMadFrameSystem::Structural: return TEXT("structural");
		case EMadFrameSystem::Debris:     return TEXT("debris");
		case EMadFrameSystem::Zombies:    return TEXT("zombies");
		case EMadFrameSystem::Horde:      return TEXT("horde");
		case EMadFrameSystem::Player:     return TEXT("player");
		case EMadFrameSystem::Saving:     return TEXT("saving");
		case EMadFrameSystem::FarTerrain: return TEXT("far terrain");
		case EMadFrameSystem::Scripts: return TEXT("scripts");
		case EMadFrameSystem::Other:      return TEXT("other");
		default:                          return TEXT("?");
		}
	}

	void Add(EMadFrameSystem System, double Seconds)
	{
		// Scopes can run in commandlets and tests without a frame loop; the
		// accounting is only meaningful on the game thread.
		if (!IsInGameThread() || System >= EMadFrameSystem::Num)
		{
			return;
		}
		GCurrent[static_cast<int32>(System)] += Seconds;
		GCurrentHasWork = true;
	}

	double GetSpentThisFrameMs()
	{
		double Seconds = 0.0;
		for (double Value : GCurrent)
		{
			Seconds += Value;
		}
		return Seconds * 1000.0;
	}

	double GetRemainingMs(double ShareMs, double MinimumMs)
	{
		return FMath::Max(MinimumMs, FMath::Min(ShareMs, BudgetMs * 0.9 - GetSpentThisFrameMs()));
	}

	void EndFrame()
	{
		++GReport.Frames;
		if (!GCurrentHasWork)
		{
			return;
		}

		double FrameMs = 0.0;
		int32 Largest = 0;
		double LargestMs = -1.0;
		for (int32 Index = 0; Index < NumSystems; ++Index)
		{
			const double Ms = GCurrent[Index] * 1000.0;
			FrameMs += Ms;
			GReport.SumMs[Index] += Ms;
			GReport.WorstMs[Index] = FMath::Max(GReport.WorstMs[Index], Ms);
			if (Ms > LargestMs)
			{
				Largest = Index;
				LargestMs = Ms;
			}
			GCurrent[Index] = 0.0;
		}
		GCurrentHasWork = false;

		++GReport.FramesWithWork;
		GReport.TotalMs += FrameMs;
		GReport.WorstFrameMs = FMath::Max(GReport.WorstFrameMs, FrameMs);
		if (FrameMs > BudgetMs)
		{
			++GReport.FramesOverBudget;
			++GReport.CulpritCount[Largest];
		}
	}

	void Reset()
	{
		GReport = FReport();
		for (double& Seconds : GCurrent)
		{
			Seconds = 0.0;
		}
		GCurrentHasWork = false;
	}

	FReport GetReport()
	{
		return GReport;
	}

	FString Describe()
	{
		const FReport& R = GReport;
		TStringBuilder<2048> Out;
		Out.Appendf(TEXT("MadFall frame budget: %lld frames, %lld with MadFall work, %lld over %.1f ms; worst frame %.3f ms, mean working frame %.3f ms\n"),
			R.Frames, R.FramesWithWork, R.FramesOverBudget, BudgetMs, R.WorstFrameMs,
			R.FramesWithWork > 0 ? R.TotalMs / R.FramesWithWork : 0.0);
		Out.Append(TEXT("  system        worst ms   mean ms/frame   largest in over-budget frames\n"));
		for (int32 Index = 0; Index < NumSystems; ++Index)
		{
			if (R.SumMs[Index] <= 0.0)
			{
				continue;
			}
			Out.Appendf(TEXT("  %-12s %9.3f %14.4f %10lld\n"), GetSystemName(static_cast<EMadFrameSystem>(Index)), R.WorstMs[Index],
				R.Frames > 0 ? R.SumMs[Index] / R.Frames : 0.0, R.CulpritCount[Index]);
		}
		return FString(Out.ToString());
	}

	void Startup()
	{
		if (!GEndFrameHandle.IsValid())
		{
			GEndFrameHandle = FCoreDelegates::OnEndFrame.AddStatic(&EndFrame);
		}
	}

	void Shutdown()
	{
		FCoreDelegates::OnEndFrame.Remove(GEndFrameHandle);
		GEndFrameHandle.Reset();
	}
}

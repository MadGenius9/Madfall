// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Game-thread time MadFall spends per frame, by system, against the 2 ms rule.
 *
 * Each system's measured stats (mad.mesh.stats, mad.si.status) say how long
 * *that* system takes; the rule is about the frame. Two systems at 1.2 ms each
 * in the same frame break it while both report "inside budget". This adds up
 * every scope that ran in a frame at FCoreDelegates::OnEndFrame and records:
 * the worst frame, how many frames went over, and which system was largest in
 * each frame that did - so an over-budget frame names its culprit.
 *
 * Scopes are for the game thread only (checked). They cost two clock reads;
 * nested scopes of different systems double-count, so wrap each system's tick
 * entry points, not helpers they share.
 *
 *   `mad.perf`        report
 *   `mad.perf.reset`  start a new measurement
 */
enum class EMadFrameSystem : uint8
{
	WorldLoads,
	Streaming,
	Meshing,
	Structural,
	Debris,
	Zombies,
	Horde,
	Player,
	Saving,
	FarTerrain,
	Scripts,
	Other,
	Num
};

namespace MadFall::FrameBudget
{
	/** Budget the report measures against, milliseconds. */
	inline constexpr double BudgetMs = 2.0;

	MADFALLCORE_API const TCHAR* GetSystemName(EMadFrameSystem System);

	/** Adds elapsed game-thread seconds to a system's bucket for the current frame. */
	MADFALLCORE_API void Add(EMadFrameSystem System, double Seconds);

	/** Milliseconds already charged to finished scopes this frame. */
	MADFALLCORE_API double GetSpentThisFrameMs();

	/**
	 * How long a system that would like ShareMs may run now: its share, cut to
	 * what is left of 90% of the frame budget once the systems that ticked
	 * earlier this frame are counted, but never below MinimumMs so it still
	 * makes progress. Per-system budgets alone let a solver, a mesher and a
	 * loader each stay inside their own share while their sum broke the frame.
	 */
	MADFALLCORE_API double GetRemainingMs(double ShareMs, double MinimumMs);

	/** Closes the current frame. Bound to FCoreDelegates::OnEndFrame; tests call it directly. */
	MADFALLCORE_API void EndFrame();

	MADFALLCORE_API void Reset();

	struct FReport
	{
		int64 Frames = 0;
		int64 FramesWithWork = 0;
		int64 FramesOverBudget = 0;
		double WorstFrameMs = 0.0;
		double TotalMs = 0.0;
		/** Wall clock since the last Reset, so the report can say how hard the work was packed. */
		double SessionSeconds = 0.0;
		double WorstMs[static_cast<int32>(EMadFrameSystem::Num)] = {};
		double SumMs[static_cast<int32>(EMadFrameSystem::Num)] = {};
		/** In frames over budget, how often each system was the largest cost. */
		int64 CulpritCount[static_cast<int32>(EMadFrameSystem::Num)] = {};
	};

	MADFALLCORE_API FReport GetReport();
	MADFALLCORE_API FString Describe();

	/** Hooks EndFrame to the engine's end of frame. Called by the Core module. */
	MADFALLCORE_API void Startup();
	MADFALLCORE_API void Shutdown();

	struct FScope
	{
		explicit FScope(EMadFrameSystem InSystem)
			: System(InSystem), Start(FPlatformTime::Seconds())
		{
		}
		~FScope()
		{
			Add(System, FPlatformTime::Seconds() - Start);
		}
		FScope(const FScope&) = delete;
		FScope& operator=(const FScope&) = delete;

	private:
		EMadFrameSystem System;
		double Start;
	};
}

/** Charges the enclosing block's game-thread time to a MadFall system. */
#define MAD_FRAME_SCOPE(SystemName) ::MadFall::FrameBudget::FScope UE_JOIN(MadFrameScope_, __LINE__)(EMadFrameSystem::SystemName)

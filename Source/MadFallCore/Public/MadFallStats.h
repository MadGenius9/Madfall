// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

/**
 * Stat groups for MadFall.
 *
 * Declared centrally and up front so that every subsystem lands in a readable
 * Unreal Insights trace from the first commit. The perf budgets in the brief
 * (60 FPS at 1440p, render thread under 8 ms, no game-thread stall over 2 ms)
 * are only enforceable if the trace actually names who spent the time, and
 * retrofitting scopes after a system is slow is how profiling gets skipped.
 *
 * Named CPU scopes use these groups via SCOPE_CYCLE_COUNTER / TRACE_CPUPROFILER_EVENT_SCOPE.
 */

/** Chunk allocation, palette repacking, voxel edits, region I/O. */
DECLARE_STATS_GROUP(TEXT("MadFall Voxel"), STATGROUP_MadFallVoxel, STATCAT_Advanced);

/** Dual Contouring, greedy meshing, LOD stitching, collision cooking. */
DECLARE_STATS_GROUP(TEXT("MadFall Mesher"), STATGROUP_MadFallMesher, STATCAT_Advanced);

/** Structural integrity flood-fill, collapse, debris. */
DECLARE_STATS_GROUP(TEXT("MadFall Structural"), STATGROUP_MadFallStructural, STATCAT_Advanced);

/** Mod discovery, manifest parsing, definition merge, pak mounting. */
DECLARE_STATS_GROUP(TEXT("MadFall ModLoad"), STATGROUP_MadFallModLoad, STATCAT_Advanced);

/** Worldgen: noise, carvers, scatter, POI stamping. */
DECLARE_STATS_GROUP(TEXT("MadFall WorldGen"), STATGROUP_MadFallWorldGen, STATCAT_Advanced);

/** Replication: voxel deltas, interest management, chunk streaming. */
DECLARE_STATS_GROUP(TEXT("MadFall Net"), STATGROUP_MadFallNet, STATCAT_Advanced);

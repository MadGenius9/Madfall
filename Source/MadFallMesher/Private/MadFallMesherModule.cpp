// Copyright MadFall. All Rights Reserved.

#include "MadFallMesher.h"

DEFINE_LOG_CATEGORY(LogMadFallMesher);

void FMadFallMesherModule::StartupModule()
{
	UE_LOG(LogMadFallMesher, Log, TEXT("MadFallMesher starting."));

	// PHASE 2 attaches here: Dual Contouring / Surface Nets job system, greedy
	// cubic mesher, LOD seam stitching, async Chaos collision cooking.
}

void FMadFallMesherModule::ShutdownModule()
{
	UE_LOG(LogMadFallMesher, Log, TEXT("MadFallMesher shutting down."));
}

IMPLEMENT_MODULE(FMadFallMesherModule, MadFallMesher)

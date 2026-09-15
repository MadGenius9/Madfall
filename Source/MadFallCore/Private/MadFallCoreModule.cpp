// Copyright MadFall. All Rights Reserved.

#include "MadFallCore.h"

#include "MadFallModAPI.h"
#include "MadFrameBudget.h"
#include "MadFallVoxelTypes.h"

DEFINE_LOG_CATEGORY(LogMadFallVoxel);
DEFINE_LOG_CATEGORY(LogMadFallRegistry);

void FMadFallCoreModule::StartupModule()
{
	UE_LOG(LogMadFallVoxel, Log,
		TEXT("MadFallCore starting (PreDefault). Chunk %dx%dx%d, voxel %d bytes, world Z %d..%d (%d chunk layers)."),
		MadFall::ChunkSize, MadFall::ChunkSize, MadFall::ChunkSize,
		static_cast<int32>(sizeof(FMadVoxel)),
		MadFall::WorldMinZ, MadFall::WorldMaxZ, MadFall::WorldChunkLayers);

	MadFall::FrameBudget::Startup();
}

void FMadFallCoreModule::ShutdownModule()
{
	MadFall::FrameBudget::Shutdown();
	UE_LOG(LogMadFallVoxel, Log, TEXT("MadFallCore shutting down."));
}

IMPLEMENT_MODULE(FMadFallCoreModule, MadFallCore)

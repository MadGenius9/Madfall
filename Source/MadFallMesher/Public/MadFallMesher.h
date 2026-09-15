// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

/** Meshing jobs, LOD, seam stitching, collision cooking. */
MADFALLMESHER_API DECLARE_LOG_CATEGORY_EXTERN(LogMadFallMesher, Log, All);

/**
 * MadFallMesher module.
 *
 * Reads voxel data, never writes it. All meshing runs on worker threads; the
 * game thread only swaps in finished results.
 */
class MADFALLMESHER_API FMadFallMesherModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	virtual bool IsGameModule() const override { return true; }
	//~ End IModuleInterface
};

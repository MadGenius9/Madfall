// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

/** Voxel volume, chunk storage, palette, region serialization. */
MADFALLCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogMadFallVoxel, Log, All);

/** Block registry: definition load, namespaced ID resolution, override warnings. */
MADFALLCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogMadFallRegistry, Log, All);

/**
 * MadFallCore module.
 *
 * Loads at PreDefault so the block registry exists before any Default-phase
 * module tries to resolve a block ID.
 */
class MADFALLCORE_API FMadFallCoreModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	virtual bool IsGameModule() const override { return true; }
	//~ End IModuleInterface

	static FMadFallCoreModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FMadFallCoreModule>(TEXT("MadFallCore"));
	}
};

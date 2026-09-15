// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

/** Umbrella log category. Subsystems declare their own narrower categories. */
MADFALLMODAPI_API DECLARE_LOG_CATEGORY_EXTERN(LogMadFall, Log, All);

/** Mod discovery, manifest parsing, load-order resolution, pak mounting. */
MADFALLMODAPI_API DECLARE_LOG_CATEGORY_EXTERN(LogMadFallMods, Log, All);

/**
 * MadFallModAPI module.
 *
 * Loads at PostConfigInit - earlier than Engine - because Tier-2 content mods
 * must have their .pak files mounted before the asset registry scans, or their
 * assets are invisible to every soft-path reference in a mod definition.
 *
 * That timing constraint is why this module cannot depend on Engine, and is the
 * reason the whole mod surface sits at the bottom of the dependency graph
 * rather than the top. See MadFallModAPI.Build.cs.
 */
class MADFALLMODAPI_API FMadFallModAPIModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	virtual bool IsGameModule() const override { return true; }
	//~ End IModuleInterface

	/** Convenience accessor. Asserts if the module is not loaded. */
	static FMadFallModAPIModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FMadFallModAPIModule>(TEXT("MadFallModAPI"));
	}
};

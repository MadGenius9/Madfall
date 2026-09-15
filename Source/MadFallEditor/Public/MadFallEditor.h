// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

/** Editor-only tooling: POI/prefab authoring, definition linter, region inspector. */
MADFALLEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogMadFallEditor, Log, All);

/**
 * MadFallEditor module.
 *
 * Nothing depends on this module and it is excluded from MadFallServer.Target.cs,
 * so a stray editor-only include in runtime code becomes a dedicated-server
 * compile error in CI on the day it is written.
 */
class MADFALLEDITOR_API FMadFallEditorModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface
};

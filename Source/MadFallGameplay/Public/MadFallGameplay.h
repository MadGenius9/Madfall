// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

/** Survival stats, combat, crafting, AI, horde director. */
MADFALLGAMEPLAY_API DECLARE_LOG_CATEGORY_EXTERN(LogMadFallGameplay, Log, All);

/** Structural integrity solver: support propagation, collapse, debris. */
MADFALLGAMEPLAY_API DECLARE_LOG_CATEGORY_EXTERN(LogMadFallStructural, Log, All);

/**
 * MadFallGameplay module.
 *
 * This is the primary game module for the MadFall target - see the
 * IMPLEMENT_PRIMARY_GAME_MODULE call in MadFallGameplayModule.cpp.
 */
class MADFALLGAMEPLAY_API FMadFallGameplayModule : public FDefaultGameModuleImpl
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface
};

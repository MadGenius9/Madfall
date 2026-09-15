// Copyright MadFall. All Rights Reserved.

#include "MadFallGameplay.h"

DEFINE_LOG_CATEGORY(LogMadFallGameplay);
DEFINE_LOG_CATEGORY(LogMadFallStructural);

void FMadFallGameplayModule::StartupModule()
{
	UE_LOG(LogMadFallGameplay, Log, TEXT("MadFallGameplay starting (primary game module)."));

	// PHASE 4 attaches here: GAS attribute sets, structural integrity solver,
	// crafting, loot, progression, zombie AI, horde director.
}

void FMadFallGameplayModule::ShutdownModule()
{
	UE_LOG(LogMadFallGameplay, Log, TEXT("MadFallGameplay shutting down."));
}

// Exactly one primary game module per game target. MadFallGameplay owns it
// because it is the module a packaged build cannot run without.
IMPLEMENT_PRIMARY_GAME_MODULE(FMadFallGameplayModule, MadFallGameplay, "MadFall");

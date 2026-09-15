// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * A tiny deferred console script.
 *
 *   -ExecCmds="mad.onspawn mad.player.give madfall:rock 5; wait 1; mad.player.status; quit"
 *
 * `-ExecCmds` runs at startup, long before the world around the player has
 * streamed in, so gameplay commands passed that way would act on a survivor
 * hanging in the void. `mad.onspawn` queues them instead; the player character
 * drains the queue once it is standing on the ground, one command per frame,
 * with `wait <seconds>` for pauses. This is how CI plays the survival loop
 * headless, and how a tester reproduces a bug from a one-line command.
 */
namespace MadFall::ConsoleScript
{
	MADFALLGAMEPLAY_API void Enqueue(const FString& Commands);

	/** Runs at most one queued command (or advances a wait). */
	MADFALLGAMEPLAY_API void Tick(UWorld* World, float DeltaSeconds);

	MADFALLGAMEPLAY_API bool IsEmpty();
}

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadModResolver.h"
#include "Templates/Function.h"

/**
 * Finds, resolves and mounts mods. Runs from MadFallModAPI's StartupModule at
 * PostConfigInit, before the asset registry scans - which is the only reason
 * Tier-2 paks mounted here are visible to every soft path afterwards.
 *
 * LAYOUT
 *   Mods/<folder>/mod.json                 manifest (madfall.mod/1)
 *   Mods/<folder>/definitions/<kind>/      Tier 1: blocks, biomes, prefabs, items, recipes, loot, zombies, animals
 *   Mods/<folder>/definitions/patches/     Tier 1: edits to other definitions
 *   Mods/<folder>/<paks listed>            Tier 2: cooked content
 *   Mods/<folder>/<scripts listed>         Tier 3: sandboxed scripts
 *
 * The folder name is not the id; `mod.json` is. Players rename folders.
 *
 * The player's disabled set lives in Saved/Config/MadFallMods.json, not in the
 * mods themselves, so a mod update never re-enables what the player turned off.
 */
class MADFALLMODAPI_API FMadModManager
{
public:
	/** Scans ModsRoot, reads the disabled list, resolves. Safe to call again (rescan). */
	void Discover(const FString& ModsRoot, const FString& DisabledListPath);

	const FMadModResolution& GetResolution() const { return Resolution; }

	/**
	 * First-party content, then every enabled mod in load order: Visit(root
	 * directory, owning namespace). Every definition loader iterates this, so
	 * load order is decided in exactly one place.
	 */
	void ForEachContentRoot(const FString& FirstPartyRoot, TFunctionRef<void(const FString& Directory, FName ModId)> Visit) const;

	/** Mounts Tier-2 paks of enabled mods. Returns how many mounted. */
	int32 MountPaks();

	/** Persists a player's enable/disable choice. Takes effect on the next launch. */
	bool SetModEnabled(FName ModId, bool bEnabled);

	FString Describe() const;

	bool HasDiscovered() const { return bDiscovered; }

private:
	FMadModResolution Resolution;
	TSet<FName> Disabled;
	FString DisabledPath;
	TArray<FString> MountedPaks;
	bool bDiscovered = false;
};

namespace MadFall
{
	/** The process-wide mod manager. Discovery runs at module startup; call Discover again to rescan. */
	MADFALLMODAPI_API FMadModManager& GetModManager();
}

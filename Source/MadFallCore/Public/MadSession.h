// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

/** What a world folder's world.json says about it. */
struct MADFALLCORE_API FMadWorldInfo
{
	/** Folder name: letters, digits, - and _. */
	FString Name;

	/** What the player typed; may contain spaces. Falls back to Name. */
	FString DisplayName;

	int64 Seed = 0;
	FDateTime Created;
	FDateTime LastPlayed;

	/** Day count at the last save, for the world list. */
	int32 Day = 1;

	/** easy, normal or hard (MadFall::Difficulty). */
	FName Difficulty = FName(TEXT("normal"));

	/**
	 * A creative world: flight, no harm and no needs, free building, and every
	 * item on a tab of the inventory. Chosen when the world is made and kept
	 * with it, so a survival world never picks it up by accident.
	 */
	bool bCreative = false;

	/** Absolute directory. Filled in by ReadWorldInfo / ListWorlds. */
	FString Directory;
};

/**
 * Which world this session plays, and whether it is at the title screen.
 *
 * Process-wide rather than on a UObject because the choice has to outlive the
 * level load it causes: the menu picks a world, reopens the map, and the voxel
 * world subsystem of the NEW world reads the choice in its Initialize - before
 * any game mode or game instance code of that world runs.
 *
 * WHY world.json: the seed used to live nowhere, so every world was seed 0
 * and a "new world" was the same world in a different folder. Each world folder
 * now records its seed when it is created. A folder with regions but no
 * world.json predates this and keeps seed 0, which is what it was made with.
 */
namespace MadFall::Session
{
	/** The background world the title screen streams. Never saved. */
	inline const TCHAR* TitleWorldName = TEXT("_Title");
	inline constexpr int64 TitleWorldSeed = 20260913;

	/**
	 * Where worlds are saved. A packaged game keeps them in the player's own
	 * folder (%LOCALAPPDATA%\MadFall\Worlds), not beside the executable: a
	 * player's first world was lost when the game folder it was played from
	 * was rebuilt, because the saves lived inside it. The editor and tests keep
	 * using the project's Saved folder. -MadWorldsDir= overrides both, which is
	 * how CI runs a packaged build without touching anyone's real worlds.
	 */
	MADFALLCORE_API FString GetWorldsRoot();

	/** The rule GetWorldsRoot applies, as a pure function so it can be tested. */
	MADFALLCORE_API FString ChooseWorldsRoot(bool bPackaged, const FString& Override, const FString& ProjectSavedDir, const FString& UserDir);

	/** Letters, digits, - and _, 1-64 characters, not starting with '_' (reserved). */
	MADFALLCORE_API bool IsValidWorldName(const FString& Name);

	/** A folder name for what a player typed: spaces to '_', other characters dropped. Empty if nothing usable is left. */
	MADFALLCORE_API FString MakeWorldName(const FString& DisplayName);

	/**
	 * A seed from what a player typed: a number is used as is, anything else is
	 * hashed (so "zombies" is a shareable seed), empty picks a random one.
	 */
	MADFALLCORE_API int64 ParseSeed(const FString& Text);

	MADFALLCORE_API bool ReadWorldInfo(const FString& Directory, FMadWorldInfo& Out);
	MADFALLCORE_API bool WriteWorldInfo(const FMadWorldInfo& Info, FString& OutError);

	/**
	 * A world folder's info: world.json, or for a folder from before world.json
	 * (regions but no world.json) its folder name, seed 0 and the time the
	 * regions were last written. False if Root/Name is not a world.
	 */
	MADFALLCORE_API bool FindWorld(const FString& Name, const FString& Root, FMadWorldInfo& Out);

	/** Every world under Root (see FindWorld), most recently played first. */
	MADFALLCORE_API void ListWorlds(TArray<FMadWorldInfo>& Out, const FString& Root);

	/** Deletes a world folder under Root. Refuses names that are not valid world names. */
	MADFALLCORE_API bool DeleteWorld(const FString& Name, const FString& Root, FString& OutError);

	/** The world the next level load plays, and the seed to create it with if it does not exist yet. */
	MADFALLCORE_API void SelectWorld(const FString& Name, TOptional<int64> SeedIfNew);
	MADFALLCORE_API bool GetSelectedWorld(FString& OutName, TOptional<int64>& OutSeedIfNew);

	/**
	 * True while the session is at the title screen. Decided once from the
	 * command line (a plain game launch starts at the title; -unattended,
	 * -MadWorld= and -MadNoTitle start straight in a world, -MadTitle forces the
	 * title), then changed only by SelectWorld and ReturnToTitle.
	 */
	MADFALLCORE_API bool IsTitleScreen();
	MADFALLCORE_API void ReturnToTitle();
}

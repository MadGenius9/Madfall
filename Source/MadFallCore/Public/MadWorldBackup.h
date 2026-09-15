// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

/** One backup of a world folder. */
struct MADFALLCORE_API FMadWorldBackup
{
	/** Folder name under the world's backups/ folder, a UTC timestamp: 2026-09-14_08-30-05. */
	FString Name;
	FDateTime Time;
	int64 Bytes = 0;
	FString Directory;
};

/**
 * Automatic world backups.
 *
 * WHEN: as a world is opened for play, before a single region file is opened.
 * That is the one moment the folder is guaranteed quiescent - nothing streams
 * or saves yet - so a plain file copy is a consistent snapshot without a
 * journal. The cost is a copy on the loading path, proportional to the world's
 * size on disk (region files are LZ4-compressed; a well-explored world is tens
 * of megabytes, well under a second). A background copy would race the first
 * saves; a copy on quit would miss a crash, which is the case backups are for.
 *
 * WHAT: world.json, gameplay.json and regions/ into
 * `<world>/backups/<timestamp>/`. Nothing is copied when no file changed since
 * the newest backup (a manifest of sizes, time stamps and, for the JSON files,
 * contents), so relaunching an untouched world costs a directory scan.
 * The newest MaxBackups are kept.
 *
 * RESTORE: the world's current state is backed up first, so a restore can be
 * undone by restoring that.
 */
namespace MadFall::WorldBackup
{
	inline constexpr int32 MaxBackups = 3;

	/** The newest backups first. */
	MADFALLCORE_API void List(const FString& WorldDirectory, TArray<FMadWorldBackup>& Out);

	/**
	 * Backs a world up if anything changed since its newest backup, then prunes to
	 * MaxBackups. OutCreated is empty when nothing needed copying. False only on a
	 * failed copy (a partial backup is removed).
	 */
	MADFALLCORE_API bool BackUp(const FString& WorldDirectory, FString& OutCreated, FString& OutError);

	/** Replaces the world's files with a backup's, after backing the current state up. The world must not be open. */
	MADFALLCORE_API bool Restore(const FString& WorldDirectory, const FString& BackupName, FString& OutError);
}

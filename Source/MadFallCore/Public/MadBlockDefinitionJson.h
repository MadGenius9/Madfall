// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBlockDefinition.h"

/**
 * One validation failure, located precisely enough for a modder to fix it
 * without reading engine source.
 *
 * Every rejected field produces one of these. There is no silent coercion and
 * no silent default for a malformed value: a mod author who writes
 * "mass_kg": "heavy" gets told the file, the JSON pointer, the expected type
 * and what they actually wrote.
 */
struct MADFALLCORE_API FMadDefinitionError
{
	/** Absolute path of the file the error came from. */
	FString SourcePath;

	/** RFC 6901 JSON pointer, e.g. "/material/mass_kg". */
	FString Pointer;

	FString Message;

	FString ToString() const
	{
		return FString::Printf(TEXT("%s%s: %s"), *SourcePath, *Pointer, *Message);
	}
};

/** The schema string every block definition file must declare. */
namespace MadFall
{
	inline const TCHAR* BlockSchemaV1 = TEXT("madfall.block/1");
}

/**
 * Parses block definition JSON into the same struct the data asset carries.
 *
 * Intentionally free functions over a class: parsing has no state worth
 * keeping, and a static surface is easier for the editor linter commandlet to
 * call on a file it never intends to register.
 */
namespace MadFall::BlockDefinitionJson
{
	/**
	 * Parses one definition object.
	 *
	 * Returns false if the definition is unusable (bad schema, missing or
	 * malformed id). Individual bad fields are reported in OutErrors and the
	 * definition still loads with those fields left at their defaults, because
	 * one mistyped sound path should not delete a block from the world.
	 */
	MADFALLCORE_API bool ParseObject(
		const TSharedRef<FJsonObject>& Object,
		const FString& SourcePath,
		FName ModId,
		FMadBlockDefinitionData& OutData,
		TArray<FMadDefinitionError>& OutErrors);

	/**
	 * Parses a file that contains either a single definition object or an array
	 * of them. Appends every definition it could read to OutDefinitions.
	 */
	MADFALLCORE_API bool ParseText(
		const FString& JsonText,
		const FString& SourcePath,
		FName ModId,
		TArray<FMadBlockDefinitionData>& OutDefinitions,
		TArray<FMadDefinitionError>& OutErrors);

	/** Loads and parses a file from disk. */
	MADFALLCORE_API bool ParseFile(
		const FString& FilePath,
		FName ModId,
		TArray<FMadBlockDefinitionData>& OutDefinitions,
		TArray<FMadDefinitionError>& OutErrors);

	/**
	 * "<namespace>:<name>", both halves non-empty and limited to [a-z0-9_].
	 *
	 * Lowercase-only is a deliberate restriction: block ids end up in file
	 * paths and in save files on case-insensitive and case-sensitive
	 * filesystems alike, and "MyMod:Stone" versus "mymod:stone" resolving
	 * differently per platform is not a bug anyone enjoys finding.
	 */
	MADFALLCORE_API bool IsValidBlockId(FName Id, FString& OutReason);

	/** Namespace half of a valid id, or NAME_None. */
	MADFALLCORE_API FName GetNamespace(FName Id);
}

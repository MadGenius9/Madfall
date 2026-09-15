// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadBlockDefinitionJson.h"

class FJsonObject;
class FJsonValue;

namespace MadFall
{
	inline const TCHAR* PatchSchemaV1 = TEXT("madfall.patch/1");
}

/** One edit, addressed by RFC 6901 JSON pointer relative to the definition object. */
struct MADFALLCORE_API FMadPatchOp
{
	enum class EType : uint8
	{
		/** Write a value, creating intermediate objects. "-" as the last array token appends. */
		Set,
		/** Append a value to an array, creating the array if absent. */
		Append,
		/** Delete a field or array element. */
		Remove
	};

	EType Type = EType::Set;
	FString Path;
	TSharedPtr<FJsonValue> Value;
};

/**
 * A patch: edits to another definition, from any mod.
 *
 *   definitions/patches/weaker_wood.json
 *   {
 *     "schema": "madfall.patch/1",
 *     "kind": "block",
 *     "target": "madfall:wood_frame",
 *     "ops": [
 *       { "op": "set",    "path": "/material/hardness", "value": 60 },
 *       { "op": "append", "path": "/tags",              "value": "block.fragile" },
 *       { "op": "remove", "path": "/drops" }
 *     ]
 *   }
 *
 * WHY PATCHES AND NOT OVERRIDES
 *   Redefining madfall:wood_frame replaces it wholesale, so two mods that each
 *   tweak one field of it conflict and the later one silently erases the
 *   other's change. Patches edit paths: two mods patching different fields of
 *   the same definition both apply, and two patching the same field resolve by
 *   load order with a warning naming both.
 *
 * Patches apply to the definition's JSON before inheritance is resolved, so
 * patching a parent reaches every child that does not override the field, and
 * the patched result goes through exactly the same validation as a hand-written
 * definition.
 */
struct MADFALLCORE_API FMadDefinitionPatch
{
	/** "block", "biome", "item", "recipe", "loot", "zombie". */
	FName Kind;
	FName Target;
	TArray<FMadPatchOp> Ops;

	FName ModId;
	FString SourcePath;
};

namespace MadFall::JsonPatch
{
	/** Applies one op in place. False with a message when the path does not fit the document. */
	MADFALLCORE_API bool Apply(const TSharedRef<FJsonObject>& Root, const FMadPatchOp& Op, FString& OutError);

	/** Parses a patch object (not the whole file). */
	MADFALLCORE_API bool Parse(const TSharedRef<FJsonObject>& Object, const FString& SourcePath, FName ModId,
		FMadDefinitionPatch& OutPatch, TArray<FMadDefinitionError>& OutErrors);
}

/**
 * Every patch from first-party content and enabled mods, in load order.
 */
class MADFALLCORE_API FMadPatchSet
{
public:
	/** Reads definitions/patches/ from every content root in load order. */
	void LoadFromSources(TArray<FMadDefinitionError>& OutErrors);

	void Add(FMadDefinitionPatch&& Patch);

	/**
	 * Applies every patch for a definition to its JSON, in load order. Two
	 * patches writing the same path warn, naming both mods; the later wins.
	 * Returns the number of ops applied.
	 */
	int32 ApplyTo(FName Kind, FName Target, const TSharedRef<FJsonObject>& Object, TArray<FMadDefinitionError>& OutErrors) const;

	/**
	 * Reports every patch of a kind whose target is not among the known ids -
	 * almost always a typo, or a patch for a mod that is not installed.
	 */
	void ReportUnmatched(FName Kind, const TSet<FName>& KnownIds, TArray<FMadDefinitionError>& OutErrors) const;

	int32 Num() const { return Patches.Num(); }

private:
	TArray<FMadDefinitionPatch> Patches;
};

namespace MadFall
{
	/** The process-wide patch set, loaded on first use. */
	MADFALLCORE_API const FMadPatchSet& GetPatchSet();
}

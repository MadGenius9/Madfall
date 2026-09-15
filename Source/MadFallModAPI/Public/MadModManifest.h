// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace MadFall
{
	inline const TCHAR* ModManifestSchemaV1 = TEXT("madfall.mod/1");

	/** The namespace first-party content uses. No mod may take this id. */
	inline const TCHAR* CoreModId = TEXT("madfall");
}

/** major.minor.patch. Pre-release and build suffixes are accepted and ignored for ordering. */
struct MADFALLMODAPI_API FMadSemanticVersion
{
	int32 Major = 0;
	int32 Minor = 0;
	int32 Patch = 0;

	static bool Parse(const FString& Text, FMadSemanticVersion& Out);

	FString ToString() const { return FString::Printf(TEXT("%d.%d.%d"), Major, Minor, Patch); }

	int32 Compare(const FMadSemanticVersion& Other) const
	{
		if (Major != Other.Major) { return Major < Other.Major ? -1 : 1; }
		if (Minor != Other.Minor) { return Minor < Other.Minor ? -1 : 1; }
		if (Patch != Other.Patch) { return Patch < Other.Patch ? -1 : 1; }
		return 0;
	}
};

/**
 * A version requirement: comma-separated clauses that must all hold.
 *
 *   "*"                any version
 *   "1.4.2"            exactly 1.4.2
 *   ">=1.2", "<2.0.0"  comparisons (missing components are 0)
 *   "^1.2.0"           >=1.2.0 and <2.0.0   (for 0.x: >=0.2.0 and <0.3.0)
 *   "~1.2.0"           >=1.2.0 and <1.3.0
 *   ">=1.2, <1.5"      both
 */
struct MADFALLMODAPI_API FMadVersionConstraint
{
	static bool Parse(const FString& Text, FMadVersionConstraint& Out, FString& OutError);

	bool IsSatisfiedBy(const FMadSemanticVersion& Version) const;

	FString Source;

private:
	enum class EOp : uint8 { Equal, Greater, GreaterEqual, Less, LessEqual };
	struct FClause
	{
		EOp Op = EOp::Equal;
		FMadSemanticVersion Version;
	};
	TArray<FClause> Clauses;
};

struct MADFALLMODAPI_API FMadModDependency
{
	FName ModId;
	FMadVersionConstraint Version;
	bool bOptional = false;
};

/**
 * A mod's `mod.json`.
 *
 * The tier is not declared, it is what the mod contains: definitions make it
 * Tier 1, paks Tier 2, scripts Tier 3. A mod can be all three.
 */
struct MADFALLMODAPI_API FMadModManifest
{
	FName Id;
	FString Name;
	FMadSemanticVersion Version;
	FString Description;
	TArray<FString> Authors;

	/** The mod API version the mod was written against, "major.minor". */
	int32 ApiMajor = 0;
	int32 ApiMinor = 0;

	TArray<FMadModDependency> Dependencies;

	/** Soft ordering: load after these if they are present; no error if absent. */
	TArray<FName> LoadAfter;
	TArray<FName> LoadBefore;

	/** Mods this one refuses to run alongside. */
	TArray<FName> Incompatible;

	/** Relative to the mod directory. */
	TArray<FString> Paks;
	TArray<FString> Scripts;

	/** Absolute directory the manifest was read from. */
	FString Directory;

	/**
	 * Parses and validates a manifest. Returns false when the mod cannot be
	 * loaded at all; OutErrors says why, one entry per problem.
	 */
	static bool ParseText(const FString& JsonText, const FString& Directory, FMadModManifest& Out, TArray<FString>& OutErrors);

	/** Mod id rules: lowercase a-z, 0-9 and _, 2-64 characters, not starting with a digit, not "madfall". */
	static bool IsValidModId(const FString& Id, FString& OutReason);
};

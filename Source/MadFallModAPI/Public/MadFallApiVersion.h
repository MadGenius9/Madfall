// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Version of the public mod-facing surface.
 *
 * This is NOT the game version. A mod declares a compatible range of this
 * version in its mod.json; the loader refuses a mod whose range excludes the
 * running API version, with a message naming both.
 *
 * Bump rules (semver, enforced by review, not by tooling):
 *   MAJOR - a stable public symbol changed shape or was removed. Requires a
 *           documented migration path in docs/MODDING.md before merge.
 *   MINOR - new stable symbols added; existing ones untouched.
 *   PATCH - behaviour fix behind an unchanged surface.
 */
namespace MadFall::ModApi
{
	inline constexpr int32 VersionMajor = 0;
	inline constexpr int32 VersionMinor = 1;
	inline constexpr int32 VersionPatch = 0;

	/** "0.1.0" */
	MADFALLMODAPI_API FString GetVersionString();

	/** True if RequiredMajor.RequiredMinor is satisfied by the running API. */
	MADFALLMODAPI_API bool IsCompatible(int32 RequiredMajor, int32 RequiredMinor);
}

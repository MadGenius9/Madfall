// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadModManifest.h"

enum class EMadModStatus : uint8
{
	/** In the load order. */
	Enabled,

	/** Turned off by the player. */
	DisabledByUser,

	/** Could not load: bad manifest, missing dependency, API mismatch, cycle, conflict. */
	Failed
};

struct MADFALLMODAPI_API FMadModIssue
{
	FName ModId;

	/** True when the issue disabled the mod; false for warnings. */
	bool bFatal = true;

	FString Message;

	FString ToString() const
	{
		return FString::Printf(TEXT("%s%s: %s"), bFatal ? TEXT("") : TEXT("warning: "), *ModId.ToString(), *Message);
	}
};

struct MADFALLMODAPI_API FMadModResolution
{
	/** Enabled mods, in load order. First-party content always loads before all of them. */
	TArray<FMadModManifest> LoadOrder;

	TMap<FName, EMadModStatus> Status;
	TArray<FMadModIssue> Issues;

	bool IsEnabled(FName ModId) const
	{
		const EMadModStatus* Found = Status.Find(ModId);
		return Found && *Found == EMadModStatus::Enabled;
	}

	/** A short fingerprint of the exact load order and versions, for stamping into world saves. */
	FString Fingerprint() const;
};

/**
 * Decides which mods load and in what order.
 *
 * RULES, applied until nothing changes:
 *   - A duplicated id disables every copy after the first (by directory order),
 *     never both: the player should not lose a mod because they unpacked it twice.
 *   - A mod written for an incompatible API version fails.
 *   - A required dependency that is absent, disabled, failed, or outside the
 *     version range fails the dependent mod - and that can cascade.
 *   - Two enabled mods where either declares the other `incompatible` fail the
 *     one that declared it. The declaring author knows why.
 *
 * ORDER is a topological sort over: dependency before dependent, `load_after`
 * target before the mod, the mod before a `load_before` target. Soft edges to
 * mods that are not enabled are ignored. Among mods with no constraint between
 * them, order is alphabetical by id, so the result never depends on filesystem
 * enumeration order or on which machine resolves it. Mods caught in a cycle
 * fail with the cycle spelled out.
 *
 * Pure: no filesystem, no globals. The mod manager feeds it discovered
 * manifests; tests feed it fabricated ones.
 */
namespace MadFall::ModResolver
{
	MADFALLMODAPI_API FMadModResolution Resolve(const TArray<FMadModManifest>& Discovered, const TSet<FName>& DisabledByUser);
}

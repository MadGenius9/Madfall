// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "IMadBlockRegistry.h"
#include "MadBlockDefinition.h"
#include "MadBlockDefinitionJson.h"

class FJsonObject;

/** One registered block. */
struct FMadBlockEntry
{
	FMadBlockDefinitionData Definition;
	FMadBlockDefView View;
	uint16 RuntimeId = 0;

	/** True for a placeholder standing in for a block whose defining mod is absent. */
	bool bUnresolved = false;
};

/** A definition that has been read but not yet had `extends` applied. */
struct FMadPendingDefinition
{
	FName Id;
	FName Extends;
	FName ModId;
	FString SourcePath;

	/** Retained so that inheritance can re-apply the child's fields over the resolved parent. */
	TSharedPtr<FJsonObject> Json;

	/** Set instead of Json for definitions that came from a UMadBlockDefinition asset. */
	TOptional<FMadBlockDefinitionData> AssetData;
};

/**
 * The block registry.
 *
 * Owns the mapping between the two ID spaces described in IMadBlockRegistry,
 * and is the single place a namespaced id becomes a number.
 *
 * LIFECYCLE
 *   BeginLoad()                          discard everything, install reserved ids
 *   AddFromDirectory() / AddFromAsset()  stage definitions, no ids assigned yet
 *   FinishLoad()                         resolve `extends`, assign runtime ids, publish
 *
 * Ids are assigned in FinishLoad in a deterministic order, so two runs with the
 * same mod set produce the same numbering. They are still never written to disk.
 *
 * THREAD SAFETY
 *   Everything registered is immutable after FinishLoad, so ResolveRuntimeId,
 *   GetStringId and GetBlockView are lock-free from any thread. The one
 *   exception is the unresolved-block table, which grows while chunks load on
 *   worker threads and is guarded by its own lock. That split matters: the
 *   registry is read on the mesher's path, and a mutex there would be felt.
 */
class MADFALLCORE_API FMadBlockRegistry final : public IMadBlockRegistry
{
public:
	FMadBlockRegistry();
	virtual ~FMadBlockRegistry() override;

	// --- IMadBlockRegistry -------------------------------------------------

	virtual uint16 ResolveRuntimeId(FName BlockId) const override;
	virtual FName GetStringId(uint16 RuntimeId) const override;
	virtual FMadBlockDefView GetBlockView(uint16 RuntimeId) const override;
	virtual FMadBlockDefView GetBlockViewById(FName BlockId) const override;
	virtual bool IsRegistered(FName BlockId) const override;
	virtual int32 Num() const override;
	virtual void GetAllBlockIds(TArray<FName>& OutIds) const override;

	// --- load ---------------------------------------------------------------

	/** Clears everything and installs the reserved ids. */
	void BeginLoad();

	/**
	 * Reads every *.json under Directory (recursively) as block definitions
	 * owned by ModId. Returns the number of definitions staged.
	 */
	int32 AddFromDirectory(const FString& Directory, FName ModId, TArray<FMadDefinitionError>& OutErrors);

	/** Stages a definition that came from a UMadBlockDefinition data asset. */
	bool AddFromAsset(const FMadBlockDefinitionData& Data, TArray<FMadDefinitionError>& OutErrors);

	/** Scans the asset registry for UMadBlockDefinition assets and stages them all. */
	int32 AddFromAssetRegistry(TArray<FMadDefinitionError>& OutErrors);

	/** Resolves inheritance, assigns runtime ids, and makes the registry queryable. */
	void FinishLoad(TArray<FMadDefinitionError>& OutErrors);

	/** Applies mod patches to every staged JSON definition. Call between staging and FinishLoad. */
	void ApplyPatches(const class FMadPatchSet& Patches, TArray<FMadDefinitionError>& OutErrors);

	bool IsLoaded() const { return bLoaded; }

	// --- unresolved blocks --------------------------------------------------

	/**
	 * Returns a stable runtime id for a block id no installed definition
	 * provides, allocating one on first use.
	 *
	 * Called by the chunk deserializer. The returned id keeps the ORIGINAL
	 * string, so the block renders as an inert placeholder, survives a save
	 * cycle byte-for-byte, and comes back intact when the mod is reinstalled.
	 * Mapping unknown ids to air here instead is the single most common way a
	 * voxel game silently deletes a player's base.
	 *
	 * Thread-safe; may be called from worker threads.
	 */
	uint16 GetOrCreateUnresolvedId(FName BlockId);

	/** How many distinct absent block ids this session has encountered. */
	int32 NumUnresolved() const;

	/** True if this runtime id is an unresolved placeholder. */
	bool IsUnresolvedId(uint16 RuntimeId) const;

	// --- diagnostics --------------------------------------------------------

	/** Human-readable summary for the `mad.blocks` console command. */
	FString DescribeContents() const;

	/** Every definition, in runtime-id order. Registered blocks only. */
	const TArray<FMadBlockEntry>& GetEntries() const { return Entries; }

	/** Full definition for a runtime id, or nullptr. */
	const FMadBlockDefinitionData* FindDefinition(uint16 RuntimeId) const;

private:
	void InstallReservedIds();

	/** Depth-first `extends` resolution with cycle detection. */
	bool ResolveDefinition(FName Id, TSet<FName>& Visiting, TMap<FName, FMadBlockDefinitionData>& Resolved,
		TArray<FMadDefinitionError>& OutErrors);

	uint16 AssignRuntimeId(const FMadBlockDefinitionData& Data);

	/** Registered definitions, in runtime-id order. Immutable after FinishLoad. */
	TArray<FMadBlockEntry> Entries;

	/** Registered id -> runtime id. Immutable after FinishLoad. */
	TMap<FName, uint16> IdToRuntimeId;

	/** Runtime id -> index into Entries. Covers registered ids only. */
	TMap<uint16, int32> RuntimeIdToEntryIndex;

	/** Staged during load, cleared by FinishLoad. */
	TArray<FMadPendingDefinition> Pending;
	TMap<FName, int32> PendingByIndex;

	/**
	 * Unresolved placeholders, allocated downward from BlockTypeUnresolved - 1
	 * so they can never collide with the sequentially assigned registered ids.
	 */
	mutable FRWLock UnresolvedLock;
	TMap<FName, uint16> UnresolvedIdToRuntimeId;
	TMap<uint16, FMadBlockEntry> UnresolvedEntries;
	uint16 NextUnresolvedId = MadFall::BlockTypeUnresolved - 1;

	bool bLoaded = false;
};

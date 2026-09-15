// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallBlockTypes.h"
#include "MadFallVoxelTypes.h"

/**
 * The block registry, as mods and scripts see it.
 *
 * Two ID spaces exist and confusing them is the most expensive mistake
 * available in this codebase:
 *
 *   FName    "madfall:oak_log"   stable, namespaced, SAVED TO DISK, survives
 *                                mod removal and reordering.
 *   uint16   42                  session-local, assigned at load, packed into
 *                                FMadVoxel, NEVER written to a save file.
 *
 * Runtime IDs are assigned in load order and will differ between two runs with
 * different mod sets. Anything that persists a block reference persists the
 * FName. Anything in the hot path uses the uint16.
 *
 * Implemented by FMadBlockRegistry in MadFallCore. Declared here so the mod
 * surface does not depend on Core.
 */
class MADFALLMODAPI_API IMadBlockRegistry
{
public:
	virtual ~IMadBlockRegistry() = default;

	/**
	 * Namespaced id -> runtime id.
	 *
	 * Returns MadFall::BlockTypeAir for NAME_None. Returns
	 * MadFall::BlockTypeUnresolved for an id no installed mod defines - callers
	 * must not treat that as "not found and therefore air", because the block
	 * still exists and still round-trips to disk.
	 */
	virtual uint16 ResolveRuntimeId(FName BlockId) const = 0;

	/**
	 * Runtime id -> namespaced id. This is what the serializer writes.
	 *
	 * For an unresolved block this returns the ORIGINAL id recorded when the
	 * chunk was loaded, not a generic placeholder name. That is what makes
	 * uninstall/reinstall of a mod lossless.
	 */
	virtual FName GetStringId(uint16 RuntimeId) const = 0;

	/** Read-only view of a definition. Invalid view (Id == NAME_None) if unknown. */
	virtual FMadBlockDefView GetBlockView(uint16 RuntimeId) const = 0;

	/** Convenience: view by namespaced id. */
	virtual FMadBlockDefView GetBlockViewById(FName BlockId) const = 0;

	/** True if an installed definition provides this id. False for unresolved placeholders. */
	virtual bool IsRegistered(FName BlockId) const = 0;

	/** Total registered definitions, including the reserved air block. */
	virtual int32 Num() const = 0;

	/** Every registered namespaced id, in runtime-id order. For tooling and the mod manager UI. */
	virtual void GetAllBlockIds(TArray<FName>& OutIds) const = 0;
};

namespace MadFall
{
	/**
	 * The process-wide block registry, or nullptr before MadFallCore has built it.
	 *
	 * A free function rather than a singleton class so that MadFallModAPI owns
	 * the accessor while MadFallCore owns the implementation and its lifetime.
	 * Scripts and mods never see the concrete type.
	 */
	MADFALLMODAPI_API IMadBlockRegistry* GetBlockRegistry();

	/** Called by MadFallCore during startup and shutdown. Not part of the mod surface. */
	MADFALLMODAPI_API void SetBlockRegistry(IMadBlockRegistry* Registry);
}

// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallVoxelTypes.h"

class FMadBlockRegistry;
struct FMadBlockDefinitionData;

/** What one hit did to one voxel. */
struct MADFALLGAMEPLAY_API FMadBlockDamageResult
{
	/** The voxel to write back. Unchanged from the input if nothing happened. */
	FMadVoxel NewVoxel;

	/** Hit points actually removed, after resistance. */
	float EffectiveDamage = 0.0f;

	/** The block is gone (air). */
	bool bDestroyed = false;

	/** The block became a different block through a damage stage's downgrade_to. */
	bool bDowngraded = false;

	/** The damage stage changed. Drives crack decals, sounds, and a structural re-solve. */
	bool bStageChanged = false;
};

/**
 * Block damage rules.
 *
 * HIT POINTS AND THE DAMAGE BYTE
 *   A block's `hardness` is its hit points. FMadVoxel::Damage stores the
 *   fraction lost, 0 intact to 255 gone, so hit points never need their own
 *   storage and a damaged wall costs nothing extra on disk until it is
 *   damaged (the damage side array is sparse).
 *
 *   Eight bits means 1/255 of hardness granularity. A steel beam has 1400 HP,
 *   so the smallest recordable hit is ~5.5 HP. Damage is rounded UP so a
 *   landed hit is never silently discarded; the cost is that many tiny hits
 *   over-count slightly, which reads as "chipping", not as a bug.
 *
 * RESISTANCE
 *   Damage type id -> multiplier from the block definition, 1.0 when absent.
 *   0 is immune (bedrock), above 1 is weak (wood vs fire).
 *
 * DOWNGRADES
 *   When the damage byte crosses a stage with `downgrade_to`, the voxel becomes
 *   that block at zero damage and the overflow carries into it. Rebar concrete
 *   shot past its last stage is a concrete frame with the leftover damage
 *   already applied, not a fresh one - otherwise a big enough explosion would
 *   be less effective against reinforced walls than two small ones.
 */
namespace MadFall::BlockDamage
{
	/** Multiplier for a damage type on a definition. */
	MADFALLGAMEPLAY_API float GetResistance(const FMadBlockDefinitionData& Definition, FName DamageType);

	/** Index of the damage stage a damage byte falls in. 0 with no stages. */
	MADFALLGAMEPLAY_API int32 GetStageIndex(const FMadBlockDefinitionData& Definition, uint8 Damage);

	/**
	 * Computes the result of Amount hit points of DamageType against a voxel.
	 * Pure: writes nothing. Air and unknown blocks take no damage.
	 */
	MADFALLGAMEPLAY_API FMadBlockDamageResult Compute(
		const FMadVoxel& Voxel, float Amount, FName DamageType, const FMadBlockRegistry& Registry);
}

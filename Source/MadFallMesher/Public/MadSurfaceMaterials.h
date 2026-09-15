// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;

namespace MadFall::SurfaceMaterials
{
	/** The photo-textured world material every textured surface instance derives from. */
	inline constexpr const TCHAR* WorldMaterialPath = TEXT("/Game/Materials/M_MadVoxelPBR.M_MadVoxelPBR");

	/** The same textures in an object's own space, for held blocks and model blocks. */
	inline constexpr const TCHAR* HeldMaterialPath = TEXT("/Game/Materials/M_MadVoxelPBRHeld.M_MadVoxelPBRHeld");

	/** Every layered set in one material, the layer in vertex alpha: see FMadSurfaceDefinition::TextureLayer. */
	inline constexpr const TCHAR* ArrayMaterialPath = TEXT("/Game/Materials/M_MadVoxelPBRArray.M_MadVoxelPBRArray");

	/** The layered material in an object's own space, the layer a parameter. */
	inline constexpr const TCHAR* ArrayHeldMaterialPath = TEXT("/Game/Materials/M_MadVoxelPBRArrayHeld.M_MadVoxelPBRArrayHeld");

	/** True when Material is M_MadVoxelPBR or an instance of it. */
	MADFALLMESHER_API bool IsTexturedWorldMaterial(const UMaterialInterface* Material);

	/**
	 * A held-space material wearing a textured surface's textures and settings,
	 * or null when the surface is not photo-textured (it keeps the procedural
	 * held material).
	 *
	 * WHY BUILT AT RUNTIME: a world instance projects its textures from world
	 * position, which on a block in the hand or a barrel slides the texture
	 * through the object as it moves. A second instance asset per texture set
	 * would have to be kept in step with the first by hand, and every mod that
	 * adds a textured surface would need both. Copying the parameters from the
	 * surface's own instance means any M_MadVoxelPBR instance, shipped or
	 * modded, has a held look for free. Weather 0 keeps rain off the block in
	 * the survivor's hand; 1 lets it fall on a barrel outside.
	 */
	MADFALLMESHER_API UMaterialInstanceDynamic* MakeHeld(UObject* Outer, FName MaterialClass, float Weather);
}

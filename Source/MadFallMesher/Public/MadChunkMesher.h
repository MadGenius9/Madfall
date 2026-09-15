// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadChunkSampleGrid.h"
#include "MadMeshBuffers.h"

class IMadBlockRegistry;

/**
 * Turns a chunk sample grid into renderable geometry.
 *
 * HYBRID GEOMETRY, THE IMPLEMENTATION:
 * Natural terrain and player construction live in the same voxel volume and are
 * separated by one bit - EMadVoxelFlags::Cubic. Voxels with that bit go through
 * the greedy cubic mesher and come out as axis-aligned quads on the build grid;
 * voxels without it go through Surface Nets and come out smooth. Both write
 * into the same FMadChunkMesh, so a cliff face and a concrete wall that meet
 * end up in one component with one transform.
 *
 * The two paths deliberately do NOT stitch to each other at the seam. A cubic
 * block placed into terrain simply overlaps the isosurface where the terrain
 * would have been solid, which reads correctly because both are opaque. Welding
 * them would mean re-triangulating the cubic block against an arbitrary
 * isosurface every time a neighbouring voxel changed.
 *
 * WHY SURFACE NETS AND NOT DUAL CONTOURING:
 * Dual Contouring's advantage is sharp-feature preservation, which it gets by
 * solving a QEF over *hermite data* - the exact surface normal at each edge
 * crossing. We store an 8-bit density per voxel and nothing else, so there is
 * no hermite data to solve against; a QEF fed reconstructed normals degenerates
 * to roughly what Surface Nets produces anyway, at several times the cost.
 * If Phase 3's generator later emits edge normals alongside density, the vertex
 * placement step here is the only thing that has to change.
 */
namespace MadFall::ChunkMesher
{
	/** Tuning that the console can change without a rebuild. */
	struct MADFALLMESHER_API FMeshSettings
	{
		/**
		 * 0 = full resolution. Each level doubles the terrain's sample stride; it
		 * takes effect only with a matching FMadLodSampleGrid. Placed blocks are
		 * always meshed at full resolution: a distant building is a few hundred
		 * merged faces, and a coarse one loses walls a voxel thick.
		 */
		int32 LodLevel = 0;

		/** Emit the smooth terrain surface. */
		bool bIsosurface = true;

		/** Emit player-placed cubic construction. */
		bool bCubic = true;

		/**
		 * Generate UVs by triplanar projection in the material instead of here.
		 * When false the mesher writes world-space planar UVs, which is enough
		 * for a placeholder material and costs nothing.
		 */
		bool bTriplanarUVs = true;
	};

	/**
	 * Builds the mesh for one chunk.
	 *
	 * Pure function of its inputs, no engine state touched, safe to call from
	 * any thread. Registry lookups are read-only and lock-free after the
	 * registry has finished loading.
	 */
	MADFALLMESHER_API void BuildChunkMesh(
		const FMadChunkSampleGrid& Grid,
		const IMadBlockRegistry& Registry,
		const FMeshSettings& Settings,
		FMadChunkMesh& OutMesh);

	/**
	 * The same, with the terrain surface taken from LodGrid when Settings.LodLevel
	 * asks for its stride (Grid still supplies placed blocks and the empty-chunk
	 * test). LodGrid may be null.
	 */
	MADFALLMESHER_API void BuildChunkMesh(
		const FMadChunkSampleGrid& Grid,
		const struct FMadLodSampleGrid* LodGrid,
		const IMadBlockRegistry& Registry,
		const FMeshSettings& Settings,
		FMadChunkMesh& OutMesh);

	/**
	 * The detail level for a chunk Distance chunks (horizontally, the larger of
	 * the X and Y offsets) from the viewer: 0 within Lod1Distance, 1 within
	 * Lod2Distance, 2 beyond. CurrentLod is the level its mesh has now (-1 for
	 * none).
	 *
	 * Finer detail is taken at once, coarser only one chunk past the boundary: a
	 * survivor pacing back and forth across a boundary would otherwise remesh the
	 * whole ring of chunks on it every time.
	 */
	MADFALLMESHER_API int32 ChooseLod(int32 Distance, int32 CurrentLod, int32 Lod1Distance, int32 Lod2Distance);
}

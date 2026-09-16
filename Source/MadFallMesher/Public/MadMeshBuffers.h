// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallCoordinates.h"

/**
 * One draw-call's worth of generated geometry, all of it sharing a material.
 *
 * Deliberately engine-agnostic types (FVector3f, uint32 indices) rather than
 * the mesh component's own structs. The component is an implementation
 * choice - ProceduralMeshComponent today, possibly something else once runtime
 * geometry gets a better home in the engine - and the mesher should not have to
 * change when that does.
 */
struct MADFALLMESHER_API FMadMeshSection
{
	/**
	 * Material class id from the block definition, e.g. "madfall:concrete".
	 * Sections are keyed by this rather than by block id so that twenty
	 * concrete block variants share one draw call.
	 */
	FName MaterialClass;

	/** Chunk-local, in Unreal units. The component places the chunk. */
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> Tangents;
	TArray<FVector2f> UVs;

	/** Surface colour in RGB, the surface's pattern index in A (MadFall::Surfaces::PatternToAlpha). */
	TArray<FColor> Colors;

	/**
	 * Corner ambient occlusion per vertex, 0 (open) to 3 (in a corner), from the
	 * blocks around the face, in the low two bits; CubicFaceFlag marks a face of a
	 * placed block, which the material bevels. Reaches the material as
	 * UV1 = (level / 3, cubic ? 1 : 0).
	 */
	TArray<uint8> Occlusion;

	/**
	 * How broken this vertex's block is, 0 (whole) to 255, from the voxel's
	 * damage. Reaches the material as UV2.x, which draws cracks that widen with
	 * it, so a wall a horde is working on shows where it is going.
	 */
	TArray<uint8> Damage;

	static constexpr uint8 CubicFaceFlag = 0x10;

	/**
	 * False for a section a body passes through: water, and anything else a
	 * block flags as liquid. Water used to collide like rock, so a survivor
	 * walked on lakes and a fall into one killed them; it is drawn, it is
	 * mined, it fills a bottle, and nothing stands on it.
	 */
	bool bCollides = true;

	TArray<uint32> Indices;

	int32 NumVertices() const { return Positions.Num(); }
	int32 NumTriangles() const { return Indices.Num() / 3; }
	bool IsEmpty() const { return Indices.Num() == 0; }

	void Reserve(int32 VertexCount, int32 IndexCount)
	{
		Positions.Reserve(VertexCount);
		Normals.Reserve(VertexCount);
		Tangents.Reserve(VertexCount);
		UVs.Reserve(VertexCount);
		Colors.Reserve(VertexCount);
		Occlusion.Reserve(VertexCount);
		Damage.Reserve(VertexCount);
		Indices.Reserve(IndexCount);
	}

	int32 AddVertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV, const FColor& Color, uint8 InOcclusion = 0, uint8 InDamage = 0);

	/** Emits two triangles for a quad given in counter-clockwise order. */
	void AddQuad(int32 V0, int32 V1, int32 V2, int32 V3)
	{
		Indices.Add(V0); Indices.Add(V1); Indices.Add(V2);
		Indices.Add(V0); Indices.Add(V2); Indices.Add(V3);
	}

	int64 GetAllocatedSize() const;
};

/** Which mesher produced a section. Kept for diagnostics and for LOD policy. */
enum class EMadMeshSource : uint8
{
	/** Smooth terrain from the isosurface path. */
	Isosurface,

	/** Player-placed construction from the greedy cubic path. */
	Cubic
};

/**
 * Everything generated for one chunk at one level of detail.
 *
 * Produced entirely on a worker thread and handed to the game thread as a
 * finished object; the game thread's only job is to push it at a component.
 */
struct MADFALLMESHER_API FMadChunkMesh
{
	FMadChunkCoord Coord;

	/** 0 = full resolution. Each level doubles the sampling stride. */
	int32 LodLevel = 0;

	TArray<FMadMeshSection> Sections;

	/**
	 * The sections already in the render component's own vertex format, built on
	 * the meshing worker (UMadChunkMeshComponent::Prepare) so the game thread only
	 * copies them. Opaque here to keep this file free of component types; null
	 * when a mesh was not prepared, and the apply then converts it itself.
	 */
	TSharedPtr<struct FMadPreparedChunkMesh, ESPMode::ThreadSafe> Prepared;

	// --- diagnostics, surfaced by `mad.mesh.stats` ---
	double BuildMilliseconds = 0.0;
	double IsosurfaceMilliseconds = 0.0;
	double CubicMilliseconds = 0.0;

	bool IsEmpty() const;
	int32 TotalVertices() const;
	int32 TotalTriangles() const;
	int64 GetAllocatedSize() const;

	/** Finds or creates the section for a material class. */
	FMadMeshSection& FindOrAddSection(FName MaterialClass);
};

using FMadChunkMeshPtr = TSharedPtr<FMadChunkMesh, ESPMode::ThreadSafe>;

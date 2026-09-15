// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MadFallBlockTypes.generated.h"

/**
 * How a block turns into geometry.
 *
 * This is the terrain/construction seam from the design pillars, expressed as
 * data rather than as a hardcoded branch: a mod can ship an isosurface ore or a
 * cubic girder without the engine knowing which is which ahead of time.
 */
UENUM(BlueprintType)
enum class EMadBlockShapeKind : uint8
{
	/** Snapped cubic grid, greedy-meshed, uses a modular mesh per damage state. */
	Cubic       UMETA(DisplayName = "Cubic"),

	/** Smooth isosurface, Dual Contouring, uses a triplanar material rather than a mesh. */
	Isosurface  UMETA(DisplayName = "Isosurface"),

	/** A single authored static mesh placed at the voxel, no meshing at all (props, doors). */
	Model       UMETA(DisplayName = "Model")
};

/**
 * How much a block can be rotated when placed.
 *
 * Constrains the orientation index stored in FMadVoxel::Rotation bits 0-4.
 */
UENUM(BlueprintType)
enum class EMadBlockRotationMode : uint8
{
	/** Always orientation 0. Terrain, and anything radially symmetric. */
	None     UMETA(DisplayName = "None"),

	/** 3 states: the block's up axis snaps to X, Y or Z. Logs, pillars. */
	Axis     UMETA(DisplayName = "Axis"),

	/** 4 states: yaw only. Furniture, workstations. */
	Facing4  UMETA(DisplayName = "Facing (4)"),

	/** All 24 proper rotations of a cube. Ramps, corners, structural members. */
	Full24   UMETA(DisplayName = "Full (24)")
};

/**
 * Read-only view of a registered block definition.
 *
 * This is the shape mods and scripts see. It is deliberately a flat POD with no
 * pointers into engine types: MadFallModAPI cannot depend on Engine, and a
 * stable surface must not hand out references to objects whose layout is free
 * to change. Anything needing the full definition (meshes, sounds, loot tables)
 * goes through the registry by ID instead.
 */
USTRUCT(BlueprintType)
struct MADFALLMODAPI_API FMadBlockDefView
{
	GENERATED_BODY()

	/** Namespaced id, e.g. "madfall:oak_log". NAME_None if this view is invalid. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	FName Id;

	/** Session-local runtime id. Never persist this - persist Id. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	int32 RuntimeId = 0;

	/** Material class id, e.g. "madfall:concrete". Drives sounds, particles, decals. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	FName MaterialClass;

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	EMadBlockShapeKind ShapeKind = EMadBlockShapeKind::Cubic;

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	EMadBlockRotationMode RotationMode = EMadBlockRotationMode::None;

	/** Kilograms for one 1 m^3 voxel. Feeds debris physics and structural load. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	float MassKg = 0.0f;

	/** Hit points at full health, before damage-type resistance. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	float Hardness = 0.0f;

	/** Kilograms this block can carry through itself. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	float SupportStrength = 0.0f;

	/** Unsupported cantilever in voxels before structural failure. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	int32 MaxHorizontalSpan = 0;

	/** Infinite support source. Terminates structural flood-fill. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	bool bIsAnchor = false;

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	bool bTransparent = false;

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	bool bLiquid = false;

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	bool bClimbable = false;

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	bool bFlammable = false;

	/** Hides neighbouring faces during greedy meshing. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	bool bOccludesNeighbors = true;

	/**
	 * True when this definition is a placeholder standing in for a block whose
	 * defining mod is not installed. Its Id is the ORIGINAL id from the save
	 * file, so the block round-trips intact.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	bool bUnresolved = false;

	/** Number of damage stages, including the intact stage. Always at least 1. */
	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Block")
	int32 NumDamageStages = 1;

	bool IsValid() const { return !Id.IsNone(); }
};

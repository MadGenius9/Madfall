// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MadFallBlockTypes.h"
#include "UObject/SoftObjectPath.h"
#include "MadBlockDefinition.generated.h"

/** How a block collides. */
UENUM(BlueprintType)
enum class EMadBlockCollisionKind : uint8
{
	None  UMETA(DisplayName = "None"),
	Box   UMETA(DisplayName = "Box"),
	Mesh  UMETA(DisplayName = "Mesh")
};

/**
 * One stage in a block's damage downgrade chain.
 *
 * Stages are ordered ascending by `At`, which is compared against
 * FMadVoxel::Damage. The first stage must be At == 0. A stage with a non-None
 * DowngradeTo replaces the block with a different definition rather than
 * swapping a mesh - that is how a concrete block becomes a concrete frame
 * rather than vanishing.
 */
USTRUCT(BlueprintType)
struct MADFALLCORE_API FMadBlockDamageStage
{
	GENERATED_BODY()

	/** Damage threshold, 0-255. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Block", meta = (ClampMin = "0", ClampMax = "255"))
	int32 At = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Block")
	FSoftObjectPath Mesh;

	/** Multiplies the block's support strength while in this stage. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Block", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SupportMultiplier = 1.0f;

	/** Mesh decal set applied on top, e.g. "madfall:cracks_light". */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Block")
	FName DecalSet;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Block")
	FName Sound;

	/** Namespaced id to become when this stage is reached. NAME_None to stay. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Block")
	FName DowngradeTo;
};

/**
 * A block definition, in the shape both the data asset and the JSON loader
 * produce.
 *
 * Kept as a plain USTRUCT rather than living directly on the UDataAsset so that
 * a JSON-defined block and an editor-authored block are literally the same
 * type by the time the registry sees them. If the shipped content could do
 * something a mod cannot, the mod API would be a lie - so first-party blocks go
 * through the JSON path too (see Definitions/blocks/).
 */
USTRUCT(BlueprintType)
struct MADFALLCORE_API FMadBlockDefinitionData
{
	GENERATED_BODY()

	// --- identity ----------------------------------------------------------

	/** Namespaced, e.g. "madfall:rebar_concrete". Required. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Identity")
	FName Id;

	/** Another definition to inherit unset fields from. Resolved before any mod patches. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Identity")
	FName Extends;

	/** "@key" for a localization lookup, or a literal string. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Identity")
	FString DisplayName;

	/** GameplayTag-style tags mods can query, e.g. "block.building". */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Identity")
	TArray<FName> Tags;

	// --- shape -------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Shape")
	EMadBlockShapeKind ShapeKind = EMadBlockShapeKind::Cubic;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Shape")
	EMadBlockRotationMode RotationMode = EMadBlockRotationMode::None;

	/** Up to 8 shape variants, indexed by FMadVoxel::Rotation bits 5-7. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Shape")
	TArray<FName> Variants;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Shape")
	bool bOccludesNeighbors = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Shape")
	EMadBlockCollisionKind Collision = EMadBlockCollisionKind::Box;

	// --- material ----------------------------------------------------------

	/** Material class id, e.g. "madfall:concrete". Supplies default sounds, particles, decals. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Material")
	FName MaterialClass;

	/** Kilograms for one 1 m^3 voxel. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Material", meta = (ClampMin = "0.0"))
	float MassKg = 100.0f;

	/** Hit points at full health. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Material", meta = (ClampMin = "0.0"))
	float Hardness = 100.0f;

	/** Damage-type id -> multiplier. A missing key means 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Material")
	TMap<FName, float> Resistances;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Material")
	TArray<FName> HarvestToolTags;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Material", meta = (ClampMin = "0"))
	int32 HarvestTier = 0;

	// --- structure ---------------------------------------------------------

	/** Kilograms this block can carry through itself. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Structure", meta = (ClampMin = "0.0"))
	float SupportStrength = 0.0f;

	/** Unsupported cantilever in voxels before failure. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Structure", meta = (ClampMin = "0"))
	int32 MaxHorizontalSpan = 0;

	/** Infinite support source. Bedrock, terrain below a depth, claim foundations. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Structure")
	bool bIsAnchor = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Structure")
	FName DebrisOnCollapse;

	// --- damage ------------------------------------------------------------

	/** Ordered ascending by At. Empty means a single intact stage. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Damage")
	TArray<FMadBlockDamageStage> DamageStages;

	// --- render ------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render")
	FSoftObjectPath Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render")
	FSoftObjectPath Material;

	/** Model blocks: mesh offset from the voxel centre, in voxels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render")
	FVector MeshOffset = FVector::ZeroVector;

	/** Model blocks: mesh scale, where 1 fits a 100 uu mesh to one voxel. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render")
	FVector MeshScale = FVector::OneVector;

	/** Honoured for Model blocks, ignored for Isosurface - see the Nanite boundary in ARCHITECTURE.md. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render")
	bool bNanite = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render")
	bool bCastShadow = true;

	/** Light the block gives off (torches, lamps). Linear colour; authored as sRGB in render.light.color. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render")
	FLinearColor LightColor = FLinearColor(1.0f, 0.5f, 0.2f);

	/** Luminous power, lumens. 0 means the block gives off no light. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render", meta = (ClampMin = "0.0"))
	float LightLumens = 0.0f;

	/** How far the light reaches, voxels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render", meta = (ClampMin = "1.0"))
	float LightRadius = 8.0f;

	/** Light position from the voxel centre, voxels (rotated with the block). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render")
	FVector LightOffset = FVector::ZeroVector;

	/**
	 * Size of the flame drawn centred on the light: 1 is 20 cm tall and 10 cm
	 * across. 0 draws none: a lamp is not a fire.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Render", meta = (ClampMin = "0.0"))
	float LightFlame = 1.0f;

	bool HasLight() const { return LightLumens > 0.0f; }

	// --- interaction -------------------------------------------------------

	/** Interacting (E) swaps this block for that one, with the same orientation: a door and its open state. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Interaction")
	FName ToggleTo;

	/** Interacting sets the survivor's respawn point here: a bed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Interaction")
	bool bSpawnPoint = false;

	// --- growth / placement ------------------------------------------------

	/** After GrowHours in-game hours this block becomes that one: a crop's next stage. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Growth")
	FName GrowInto;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Growth", meta = (ClampMin = "0.0"))
	float GrowHours = 0.0f;

	// --- trap ----------------------------------------------------------------

	/** Hit points dealt to a creature standing in the block, every TrapSeconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Trap", meta = (ClampMin = "0.0"))
	float TrapDamage = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Trap", meta = (ClampMin = "0.1"))
	float TrapSeconds = 1.0f;

	/** Damage the trap takes itself per hit: spikes blunt and break. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Trap", meta = (ClampMin = "0.0"))
	float TrapWear = 0.0f;

	/** Movement speed multiplier while inside. 1 = no slowing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Trap", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float TrapSlow = 1.0f;

	bool HasTrap() const { return TrapDamage > 0.0f || TrapSlow < 1.0f; }

	/** Placeable only on top of a block with this tag (seeds on farmland). NAME_None = anywhere. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Placement")
	FName PlaceOnTag;

	// --- sounds / drops ----------------------------------------------------

	/** Event name ("place", "hit", "destroy", "step") -> asset. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Audio")
	TMap<FName, FSoftObjectPath> Sounds;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Loot")
	FName DropTable;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Loot")
	FName DropTableOnCollapse;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Loot")
	TArray<FName> RequiresToolTags;

	// --- flags -------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Flags")
	bool bTransparent = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Flags")
	bool bLiquid = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Flags")
	bool bClimbable = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Flags")
	bool bFlammable = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall|Flags")
	bool bConductive = false;

	// --- provenance --------------------------------------------------------
	// Not authored. Filled in by the loader so that collision warnings and the
	// mod manager UI can name the file a definition actually came from.

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Provenance")
	FName SourceModId;

	UPROPERTY(BlueprintReadOnly, Category = "MadFall|Provenance")
	FString SourcePath;

	/** Builds the read-only view mods and scripts see. */
	FMadBlockDefView MakeView(int32 RuntimeId) const;
};

/**
 * Editor-authored block definition.
 *
 * Exactly a wrapper around FMadBlockDefinitionData. The registry consumes the
 * inner struct, so a data asset and a JSON file are interchangeable by the time
 * anything gameplay-facing sees them.
 */
UCLASS(BlueprintType)
class MADFALLCORE_API UMadBlockDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MadFall", meta = (ShowOnlyInnerProperties))
	FMadBlockDefinitionData Data;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("MadBlockDefinition"), GetFName());
	}
};

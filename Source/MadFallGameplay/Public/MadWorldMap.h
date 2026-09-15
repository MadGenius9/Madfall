// Copyright MadFall. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tasks/Task.h"
#include "MadWorldMap.generated.h"

class FMadWorldGenerator;
class UTexture2D;

/** A drawn map: Size x Size pixels, VoxelsPerPixel voxels each, centred on Centre (voxels). */
struct FMadMapImage
{
	int32 Size = 0;
	int32 VoxelsPerPixel = 8;
	FIntPoint Centre = FIntPoint::ZeroValue;
	TArray<FColor> Pixels;

	/** Pixels as sRGB BGRA bytes for the texture, filled on the worker that drew them. */
	TArray<uint8> Bgra;
};

namespace MadFall::WorldMap
{
	/** Exploration is remembered per chunk column. */
	inline constexpr int32 CellVoxels = 32;

	/** How far around the survivor counts as seen, voxels. */
	inline constexpr int32 RevealRadiusVoxels = 128;

	/** The colour of unexplored map (linear bytes, like the rest: near black once shown as sRGB). */
	inline const FColor Unexplored = FColor(2, 2, 3);

	/** Marks cells within Radius of a point as explored. Returns how many were new. */
	MADFALLGAMEPLAY_API int32 Reveal(TSet<FIntPoint>& Explored, const FVector2D& AtVoxels, int32 RadiusVoxels);

	MADFALLGAMEPLAY_API FIntPoint CellOf(double VoxelX, double VoxelY);

	/**
	 * Draws the map from the generator: biome colours, flat water, hill shading
	 * from the height gradient (light from the north-west), and unexplored cells
	 * dark. Safe on any thread.
	 */
	MADFALLGAMEPLAY_API void BuildImage(const FMadWorldGenerator& Generator, const TArray<FColor>& BiomeColours, FColor Water,
		const TSet<FIntPoint>& Explored, FIntPoint Centre, int32 Size, int32 VoxelsPerPixel, FMadMapImage& Out);

	/** Where a world voxel lands on an image, in pixels (north up, east right); may be outside [0, Size). */
	MADFALLGAMEPLAY_API FVector2D ToPixel(const FMadMapImage& Image, const FVector2D& Voxels);
}

/**
 * The world map (M): what the survivor has seen, drawn from the generator.
 *
 * Exploration is a set of chunk columns within 128 voxels of anywhere the
 * survivor has been, saved with the world. The picture itself is not saved or
 * streamed: like far terrain, it is sampled straight from the generator's
 * analytic surface and biome fields on a worker when the map is opened, zoomed
 * or walked far across, then uploaded as one small texture. Player edits and
 * POI interiors are not drawn - it is a survey map, and markers carry the rest.
 *
 *   `mad.map.toggle`, `mad.map.zoom <1|2|3>`, `mad.map.reveal <radius>`, `mad.map.status`
 */
UCLASS()
class MADFALLGAMEPLAY_API UMadWorldMapSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	bool IsOpen() const { return bOpen; }
	void SetOpen(bool bInOpen);

	/** 0 closest (4 voxels a pixel), 2 widest (16). */
	void SetZoom(int32 InZoom);
	int32 GetZoom() const { return Zoom; }
	void ZoomBy(int32 Steps) { SetZoom(Zoom + Steps); }

	/** The texture to draw, and the image it shows; null until the first draw lands. */
	UTexture2D* GetTexture() const { return Texture; }
	const FMadMapImage& GetImage() const { return Shown; }
	bool IsDrawing() const { return bDrawing; }

	bool IsExplored(const FIntPoint& Cell) const { return Explored.Contains(Cell); }
	int32 RevealAround(const FVector2D& AtVoxels, int32 RadiusVoxels);
	int32 NumExplored() const { return Explored.Num(); }

	void ExportState(TArray<FIntPoint>& Out) const;
	void ImportState(const TArray<FIntPoint>& In);

	FString DescribeStatus() const;

	static constexpr int32 ImageSize = 256;

private:
	void StartDraw(const FIntPoint& Centre);
	void EnsureTexture();
	void ApplyDraw();

	TSet<FIntPoint> Explored;
	bool bOpen = false;
	int32 Zoom = 1;
	float RevealTimer = 0.0f;
	float RedrawTimer = 0.0f;
	int32 ExploredAtLastDraw = -1;

	bool bDrawing = false;
	UE::Tasks::FTask DrawTask;
	TSharedPtr<FMadMapImage> Pending;
	FMadMapImage Shown;

	TArray<FColor> BiomeColours;
	FColor WaterColour = FColor(38, 77, 115);

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Texture;

	int32 DrawCount = 0;
	double LastUploadMs = 0.0;
};

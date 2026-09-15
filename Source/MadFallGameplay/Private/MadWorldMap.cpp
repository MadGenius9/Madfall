// Copyright MadFall. All Rights Reserved.

#include "MadWorldMap.h"

#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MadBiomeRegistry.h"
#include "MadBlockRegistry.h"
#include "MadFallGameplay.h"
#include "MadFrameBudget.h"
#include "MadSurfaceRegistry.h"
#include "MadVoxelWorldSubsystem.h"
#include "MadWorldGenerator.h"
#include "Misc/App.h"
#include "Misc/DefaultValueHelper.h"
#include "TextureResource.h"

namespace
{
	const int32 ZoomVoxelsPerPixel[] = { 4, 8, 16 };

	FVector2D PlayerVoxels(const UWorld& World)
	{
		const APlayerController* Controller = World.GetFirstPlayerController();
		const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		return Pawn ? FVector2D(Pawn->GetActorLocation().X, Pawn->GetActorLocation().Y) / MadFall::VoxelSizeUU : FVector2D::ZeroVector;
	}
}

// --- pure functions ------------------------------------------------------------------

FIntPoint MadFall::WorldMap::CellOf(double VoxelX, double VoxelY)
{
	return FIntPoint(FMath::FloorToInt32(VoxelX / CellVoxels), FMath::FloorToInt32(VoxelY / CellVoxels));
}

int32 MadFall::WorldMap::Reveal(TSet<FIntPoint>& Explored, const FVector2D& AtVoxels, int32 RadiusVoxels)
{
	const FIntPoint Min = CellOf(AtVoxels.X - RadiusVoxels, AtVoxels.Y - RadiusVoxels);
	const FIntPoint Max = CellOf(AtVoxels.X + RadiusVoxels, AtVoxels.Y + RadiusVoxels);
	const double RadiusSq = FMath::Square(static_cast<double>(RadiusVoxels) + CellVoxels * 0.71);
	int32 Added = 0;
	for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
	{
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			const FVector2D Centre((X + 0.5) * CellVoxels, (Y + 0.5) * CellVoxels);
			if (FVector2D::DistSquared(Centre, AtVoxels) <= RadiusSq)
			{
				bool bAlready = false;
				Explored.Add(FIntPoint(X, Y), &bAlready);
				Added += bAlready ? 0 : 1;
			}
		}
	}
	return Added;
}

void MadFall::WorldMap::BuildImage(const FMadWorldGenerator& Generator, const TArray<FColor>& BiomeColours, FColor Water,
	const TSet<FIntPoint>& Explored, FIntPoint Centre, int32 Size, int32 VoxelsPerPixel, FMadMapImage& Out)
{
	Out.Size = Size;
	Out.VoxelsPerPixel = VoxelsPerPixel;
	Out.Centre = Centre;
	Out.Pixels.Init(Unexplored, Size * Size);

	const float SeaTop = static_cast<float>(Generator.GetSettings().SeaLevel + 1);
	const double Half = Size * 0.5;
	TArray<float> Heights;
	Heights.Init(TNumericLimits<float>::Lowest(), Size * Size);

	// North (+X) is up and east (+Y) is right, like the compass: a pixel's column
	// is its Y offset and its row, counted down, is its X offset.
	auto VoxelOf = [&](int32 Px, int32 Py)
	{
		return FVector2D(Centre.X - (Py - Half + 0.5) * VoxelsPerPixel, Centre.Y + (Px - Half + 0.5) * VoxelsPerPixel);
	};

	// Heights first, only where explored, so a mostly unknown map costs little.
	for (int32 Py = 0; Py < Size; ++Py)
	{
		for (int32 Px = 0; Px < Size; ++Px)
		{
			const FVector2D V = VoxelOf(Px, Py);
			const double VX = V.X;
			const double VY = V.Y;
			if (Explored.Contains(CellOf(VX, VY)))
			{
				Heights[Px + Py * Size] = Generator.GetSurfaceHeight(static_cast<float>(VX), static_cast<float>(VY));
			}
		}
	}

	for (int32 Py = 0; Py < Size; ++Py)
	{
		for (int32 Px = 0; Px < Size; ++Px)
		{
			const float H = Heights[Px + Py * Size];
			if (H == TNumericLimits<float>::Lowest())
			{
				continue;
			}
			const FVector2D V = VoxelOf(Px, Py);
			const double VX = V.X;
			const double VY = V.Y;
			FColor Colour;
			if (H < SeaTop - 0.5f)
			{
				// Deeper water darker.
				const float Depth = FMath::Clamp((SeaTop - H) / 40.0f, 0.0f, 1.0f);
				Colour = FLinearColor::LerpUsingHSV(FLinearColor(Water), FLinearColor(Water) * 0.45f, Depth).ToFColor(false);
			}
			else
			{
				const int32 Biome = Generator.GetDominantBiome(static_cast<float>(VX), static_cast<float>(VY));
				Colour = BiomeColours.IsValidIndex(Biome) ? BiomeColours[Biome] : FColor(90, 110, 60);
				// Hill shading: light from the north-west, from neighbouring heights.
				auto At = [&](int32 X, int32 Y)
				{
					X = FMath::Clamp(X, 0, Size - 1);
					Y = FMath::Clamp(Y, 0, Size - 1);
					const float Sample = Heights[X + Y * Size];
					return Sample == TNumericLimits<float>::Lowest() ? H : FMath::Max(Sample, SeaTop);
				};
				const float Slope = (At(Px - 1, Py - 1) - At(Px + 1, Py + 1)) / (2.0f * VoxelsPerPixel);
				const float Light = FMath::Clamp(1.0f + Slope * 1.8f, 0.55f, 1.45f);
				// Higher ground a little paler, so ridges read even on flat colour.
				const float Altitude = FMath::Clamp((H - SeaTop) / 240.0f, 0.0f, 0.2f);
				FLinearColor Lit = FLinearColor(Colour) * Light;
				Lit = FMath::Lerp(Lit, FLinearColor(0.45f, 0.45f, 0.42f), Altitude);
				Colour = Lit.ToFColor(false);
			}
			Out.Pixels[Px + Py * Size] = Colour;
		}
	}
}

FVector2D MadFall::WorldMap::ToPixel(const FMadMapImage& Image, const FVector2D& Voxels)
{
	return FVector2D((Voxels.Y - Image.Centre.Y) / Image.VoxelsPerPixel + Image.Size * 0.5,
		-(Voxels.X - Image.Centre.X) / Image.VoxelsPerPixel + Image.Size * 0.5);
}

// --- subsystem ---------------------------------------------------------------------------

bool UMadWorldMapSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UMadWorldMapSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UMadVoxelWorldSubsystem>();
}

void UMadWorldMapSubsystem::Deinitialize()
{
	if (bDrawing)
	{
		DrawTask.Wait();
	}
	Super::Deinitialize();
}

TStatId UMadWorldMapSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMadWorldMapSubsystem, STATGROUP_Tickables);
}

int32 UMadWorldMapSubsystem::RevealAround(const FVector2D& AtVoxels, int32 RadiusVoxels)
{
	return MadFall::WorldMap::Reveal(Explored, AtVoxels, RadiusVoxels);
}

void UMadWorldMapSubsystem::SetOpen(bool bInOpen)
{
	bOpen = bInOpen;
	if (bOpen)
	{
		RedrawTimer = 0.0f;
		ExploredAtLastDraw = -1;
	}
}

void UMadWorldMapSubsystem::SetZoom(int32 InZoom)
{
	const int32 Clamped = FMath::Clamp(InZoom, 0, UE_ARRAY_COUNT(ZoomVoxelsPerPixel) - 1);
	if (Clamped != Zoom)
	{
		Zoom = Clamped;
		ExploredAtLastDraw = -1;
		RedrawTimer = 0.0f;
	}
}

void UMadWorldMapSubsystem::StartDraw(const FIntPoint& Centre)
{
	const UMadVoxelWorldSubsystem* VoxelWorld = GetWorld()->GetSubsystem<UMadVoxelWorldSubsystem>();
	const FMadWorldGenerator* Generator = VoxelWorld ? VoxelWorld->GetWorldGenerator() : nullptr;
	if (Generator == nullptr || bDrawing)
	{
		return;
	}
	if (BiomeColours.Num() == 0)
	{
		const FMadBiomeRegistry& Biomes = UMadVoxelWorldSubsystem::GetBiomeRegistry();
		const FMadBlockRegistry& Blocks = UMadVoxelWorldSubsystem::GetBlockRegistry();
		for (int32 Index = 0; Index < Biomes.Num(); ++Index)
		{
			BiomeColours.Add(MadFall::GetSurfaces().GetVertexColor(Blocks.GetBlockViewById(Biomes.Get(Index).SurfaceBlock).MaterialClass));
		}
		WaterColour = MadFall::GetSurfaces().GetVertexColor(FName(TEXT("madfall:water")));
	}

	bDrawing = true;
	ExploredAtLastDraw = Explored.Num();
	TSharedPtr<FMadMapImage> Image = MakeShared<FMadMapImage>();
	Pending = Image;
	// Copies: the explored set keeps growing on the game thread while this draws.
	DrawTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Generator, Image, Colours = BiomeColours, Water = WaterColour, Cells = Explored, Centre, VPP = ZoomVoxelsPerPixel[Zoom]]()
	{
		MadFall::WorldMap::BuildImage(*Generator, Colours, Water, Cells, Centre, ImageSize, VPP, *Image);
		// The sRGB conversion is a power per channel per pixel: here, not on the game thread.
		Image->Bgra.SetNumUninitialized(Image->Pixels.Num() * 4);
		for (int32 Index = 0; Index < Image->Pixels.Num(); ++Index)
		{
			const FColor& P = Image->Pixels[Index];
			const FColor C = FLinearColor(P.R / 255.0f, P.G / 255.0f, P.B / 255.0f).ToFColor(true);
			Image->Bgra[Index * 4 + 0] = C.B;
			Image->Bgra[Index * 4 + 1] = C.G;
			Image->Bgra[Index * 4 + 2] = C.R;
			Image->Bgra[Index * 4 + 3] = 255;
		}
	});
}

void UMadWorldMapSubsystem::EnsureTexture()
{
	if (Texture != nullptr || FApp::CanEverRender() == false)
	{
		return;
	}
	// Created while the world loads, not when the map first opens: making and
	// initialising a texture resource is the one expensive step.
	Texture = UTexture2D::CreateTransient(ImageSize, ImageSize, PF_B8G8R8A8);
	if (Texture != nullptr)
	{
		Texture->Filter = TF_Bilinear;
		// Pixels are built from linear vertex-colour bytes and written as sRGB,
		// so the canvas shows them as they look on the terrain.
		Texture->SRGB = true;
		Texture->UpdateResource();
	}
}

void UMadWorldMapSubsystem::ApplyDraw()
{
	if (!bDrawing || !DrawTask.IsCompleted() || !Pending.IsValid())
	{
		return;
	}
	bDrawing = false;
	Shown = MoveTemp(*Pending);
	Pending.Reset();
	++DrawCount;

	const double Start = FPlatformTime::Seconds();
	EnsureTexture();
	if (Texture != nullptr && Texture->GetResource() != nullptr)
	{
		// Converted here, copied to the GPU on the render thread: no lock and no
		// UpdateResource on the game thread, which measured 4 ms for this texture.
		const int32 Bytes = Shown.Bgra.Num();
		uint8* Data = static_cast<uint8*>(FMemory::Malloc(Bytes));
		FMemory::Memcpy(Data, Shown.Bgra.GetData(), Bytes);
		static FUpdateTextureRegion2D Region(0, 0, 0, 0, ImageSize, ImageSize);
		Texture->UpdateTextureRegions(0, 1, &Region, ImageSize * 4, 4, Data,
			[](uint8* SrcData, const FUpdateTextureRegion2D*) { FMemory::Free(SrcData); });
	}
	LastUploadMs = (FPlatformTime::Seconds() - Start) * 1000.0;
}

void UMadWorldMapSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	MAD_FRAME_SCOPE(Other);

	UWorld* World = GetWorld();
	EnsureTexture();
	if (World->GetFirstPlayerController() == nullptr || World->GetFirstPlayerController()->GetPawn() == nullptr)
	{
		return;
	}
	const FVector2D Player = PlayerVoxels(*World);

	RevealTimer -= DeltaTime;
	if (RevealTimer <= 0.0f)
	{
		RevealTimer = 0.5f;
		RevealAround(Player, MadFall::WorldMap::RevealRadiusVoxels);
	}

	ApplyDraw();
	if (!bOpen)
	{
		return;
	}

	// Redraw when opened, zoomed, walked a good way across, or when enough new ground is seen.
	RedrawTimer -= DeltaTime;
	const double Extent = ImageSize * ZoomVoxelsPerPixel[Zoom];
	const bool bMoved = Shown.Size == 0 || FVector2D::Distance(FVector2D(Shown.Centre), Player) > Extent * 0.2;
	const bool bNewGround = Explored.Num() != ExploredAtLastDraw;
	if (!bDrawing && (ExploredAtLastDraw < 0 || bMoved || (bNewGround && RedrawTimer <= 0.0f)))
	{
		RedrawTimer = 2.0f;
		StartDraw(FIntPoint(FMath::RoundToInt32(Player.X), FMath::RoundToInt32(Player.Y)));
	}
}

void UMadWorldMapSubsystem::ExportState(TArray<FIntPoint>& Out) const
{
	Out = Explored.Array();
	Out.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
}

void UMadWorldMapSubsystem::ImportState(const TArray<FIntPoint>& In)
{
	Explored = TSet<FIntPoint>(In);
	ExploredAtLastDraw = -1;
}

FString UMadWorldMapSubsystem::DescribeStatus() const
{
	return FString::Printf(TEXT("Map: %s, zoom %d (%d voxels a pixel), %d cell(s) explored, %d draw(s)%s, last upload %.2f ms."),
		bOpen ? TEXT("open") : TEXT("closed"), Zoom, ZoomVoxelsPerPixel[Zoom], Explored.Num(), DrawCount,
		bDrawing ? TEXT(", drawing") : TEXT(""), LastUploadMs);
}

static FAutoConsoleCommandWithWorld GMadMapToggleCommand(
	TEXT("mad.map.toggle"), TEXT("Opens or closes the world map."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (UMadWorldMapSubsystem* Map = World ? World->GetSubsystem<UMadWorldMapSubsystem>() : nullptr)
		{
			Map->SetOpen(!Map->IsOpen());
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadMapZoomCommand(
	TEXT("mad.map.zoom"), TEXT("mad.map.zoom <0|1|2>: closest to widest."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		int32 Zoom = 1;
		if (UMadWorldMapSubsystem* Map = World ? World->GetSubsystem<UMadWorldMapSubsystem>() : nullptr; Map && Args.Num() > 0 && FDefaultValueHelper::ParseInt(Args[0], Zoom))
		{
			Map->SetZoom(Zoom);
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GMadMapRevealCommand(
	TEXT("mad.map.reveal"), TEXT("mad.map.reveal <radius in voxels>: marks the area around the survivor as explored."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		int32 Radius = 512;
		UMadWorldMapSubsystem* Map = World ? World->GetSubsystem<UMadWorldMapSubsystem>() : nullptr;
		if (Map != nullptr)
		{
			if (Args.Num() > 0) { FDefaultValueHelper::ParseInt(Args[0], Radius); }
			const int32 Added = Map->RevealAround(PlayerVoxels(*World), FMath::Clamp(Radius, 0, 8192));
			UE_LOG(LogMadFallGameplay, Display, TEXT("Revealed %d cell(s)."), Added);
		}
	}));

static FAutoConsoleCommandWithWorld GMadMapStatusCommand(
	TEXT("mad.map.status"), TEXT("World map state."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (const UMadWorldMapSubsystem* Map = World ? World->GetSubsystem<UMadWorldMapSubsystem>() : nullptr)
		{
			UE_LOG(LogMadFallGameplay, Display, TEXT("%s"), *Map->DescribeStatus());
		}
	}));
